// Spec for source-login credential exposure.
//
// These tests compile the product's Icecast, Shoutcast, and socket code and
// accept the TCP connection those functions open. They fail while the source
// password is recoverable by a peer that has not presented a verified
// certificate: either the bytes are still plaintext, or the client finishes
// TLS against a certificate this process generated and never installed as
// trusted, then writes the password. They pass once the client refuses to
// send that password on either of those channels.
//
// Covered operations: ic_connect PUT, ic_connect SOURCE fallback,
// ic_update_song, sc_connect, and sc_update_song.

#include "cfg.h"
#include "butt.h"
#include "icecast.h"
#include "shoutcast.h"

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

char *util_base64_enc(char *data);

config_t cfg;
bool connected;
int stream_socket;
sec_timer stream_timer;

void print_info(const char *info, int info_type)
{
    (void)info;
    (void)info_type;
}

// Obvious placeholders. These are not credentials for any real server.
static const char kUser[] = "PLACEHOLDER_SOURCE_USER";
static const char kPassword[] = "PLACEHOLDER_SOURCE_PASSWORD";

static char *dupstr(const char *s)
{
    char *copy = (char *)malloc(strlen(s) + 1);
    if (copy == NULL)
        abort();
    memcpy(copy, s, strlen(s) + 1);
    return copy;
}

static void free_cfg(void)
{
    if (cfg.srv != NULL) {
        if (cfg.srv[0] != NULL) {
            free(cfg.srv[0]->name);
            free(cfg.srv[0]->addr);
            free(cfg.srv[0]->pwd);
            free(cfg.srv[0]->mount);
            free(cfg.srv[0]->usr);
            free(cfg.srv[0]);
        }
        free(cfg.srv);
    }
    free(cfg.audio.codec);
    free(cfg.main.song);
    memset(&cfg, 0, sizeof(cfg));
}

static void setup_server(int type, int port)
{
    free_cfg();
    cfg.selected_srv = 0;
    cfg.main.num_of_srv = 1;
    cfg.main.num_of_icy = 0;
    cfg.srv = (server_t **)malloc(sizeof(server_t *));
    cfg.srv[0] = (server_t *)calloc(1, sizeof(server_t));
    cfg.srv[0]->name = dupstr("station");
    cfg.srv[0]->addr = dupstr("127.0.0.1");
    cfg.srv[0]->pwd = dupstr(kPassword);
    cfg.srv[0]->usr = dupstr(kUser);
    cfg.srv[0]->mount = dupstr("livemount");
    cfg.srv[0]->port = port;
    cfg.srv[0]->type = type;
    cfg.audio.codec = dupstr("mp3");
    cfg.audio.bitrate = 128;
    cfg.audio.channel = 2;
    cfg.audio.samplerate = 44100;
    cfg.main.song = dupstr("track-title");
    connected = false;
    stream_socket = -1;
}

static int b64_digit(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static bool b64_decode(const std::string &in, std::string &out)
{
    out.clear();
    int val = 0;
    int bits = -8;
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ')
            continue;
        int digit = b64_digit((char)c);
        if (digit < 0)
            return false;
        val = (val << 6) | digit;
        bits += 6;
        if (bits >= 0) {
            out.push_back((char)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return true;
}

static std::string user_pass(void)
{
    std::string pair = kUser;
    pair.push_back(':');
    pair += kPassword;
    return pair;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Undo percent-encoding so a password hidden in a query string still counts.
static std::string percent_decode(const std::string &in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int hi = hex_nibble(in[i + 1]);
            int lo = hex_nibble(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

// How a peer can recover the source password from these bytes.
// "none" means it cannot. Other values name the transform that revealed it.
static const char *exposure_reason(const std::string &bytes)
{
    if (bytes.find(kPassword) != std::string::npos)
        return "literal";

    std::string pair = user_pass();
    if (bytes.find(pair) != std::string::npos)
        return "user-pass";

    char *owned = dupstr(pair.c_str());
    char *encoded = util_base64_enc(owned);
    bool encoded_hit = encoded != NULL && bytes.find(encoded) != std::string::npos;
    free(encoded);
    free(owned);
    if (encoded_hit)
        return "base64";

    const std::string marker = "Authorization: Basic ";
    size_t pos = 0;
    while ((pos = bytes.find(marker, pos)) != std::string::npos) {
        pos += marker.size();
        size_t end = pos;
        while (end < bytes.size() && bytes[end] != '\r' && bytes[end] != '\n' &&
               bytes[end] != ' ')
            end++;
        std::string decoded;
        if (b64_decode(bytes.substr(pos, end - pos), decoded) && decoded == pair)
            return "basic-token";
    }

    std::string decoded = percent_decode(bytes);
    if (decoded != bytes) {
        const char *again = exposure_reason(decoded);
        if (strcmp(again, "none") != 0)
            return "percent-encoded";
    }
    return "none";
}

static bool credential_exposed(const std::string &bytes)
{
    return strcmp(exposure_reason(bytes), "none") != 0;
}

struct Session {
    std::string raw;
    std::string app;
    bool tls;
    bool handshake_ok;
    std::string kind;
};

static long record_read(BIO *bio, int oper, const char *argp, size_t len,
                        int argi, long argl, int ret, size_t *processed)
{
    (void)len;
    (void)argi;
    (void)argl;
    (void)processed;
    std::string *raw = (std::string *)BIO_get_callback_arg(bio);
    if (raw != NULL && BIO_cb_post(oper) &&
        (oper & ~BIO_CB_RETURN) == BIO_CB_READ && ret > 0 && argp != NULL)
        raw->append(argp, (size_t)ret);
    return ret;
}

static void classify(Session &session)
{
    const std::string &body = session.tls ? session.app : session.raw;
    if (session.tls && !session.handshake_ok)
        session.kind = "tls-rejected";
    else if (body.compare(0, 6, "SOURCE") == 0)
        session.kind = "SOURCE";
    else if (body.compare(0, 3, "PUT") == 0)
        session.kind = "PUT";
    else if (body.compare(0, 3, "GET") == 0)
        session.kind = "GET";
    else if (session.tls)
        session.kind = "tls-app";
    else if (!session.raw.empty() && (unsigned char)session.raw[0] == 0x16)
        session.kind = "tls-record";
    else
        session.kind = "non-http";
}

class Catcher {
public:
    std::vector<std::string> responses;
    std::vector<Session> sessions;
    uint16_t port;
    SSL_CTX *ctx;

    Catcher(void) : port(0), ctx(NULL), listen_fd_(-1), run_(false) {}

    ~Catcher(void)
    {
        stop();
        if (ctx != NULL)
            SSL_CTX_free(ctx);
    }

    bool open(const char *cert, const char *key)
    {
        ctx = SSL_CTX_new(TLS_server_method());
        if (ctx == NULL)
            return false;
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        if (SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) != 1)
            return false;
        if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1)
            return false;

        listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd_ < 0)
            return false;
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);
        if (bind(listen_fd_, (sockaddr *)&addr, sizeof(addr)) != 0)
            return false;
        if (listen(listen_fd_, 16) != 0)
            return false;

        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd_, (sockaddr *)&addr, &len) != 0)
            return false;
        port = ntohs(addr.sin_port);
        return port != 0;
    }

    void start(void)
    {
        run_ = true;
        thread_ = std::thread(&Catcher::loop, this);
    }

    void stop(void)
    {
        if (!run_ && listen_fd_ < 0)
            return;
        run_ = false;
        if (listen_fd_ >= 0) {
            shutdown(listen_fd_, SHUT_RDWR);
            close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable())
            thread_.join();
    }

private:
    int listen_fd_;
    std::atomic<bool> run_;
    std::thread thread_;

    void loop(void)
    {
        while (run_) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(listen_fd_, &fds);
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
            int ready = select(listen_fd_ + 1, &fds, NULL, NULL, &tv);
            if (ready <= 0)
                continue;
            int fd = accept(listen_fd_, NULL, NULL);
            if (fd < 0)
                continue;
            serve(fd);
            close(fd);
        }
    }

    void set_timeouts(int fd)
    {
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    std::string next_response(void)
    {
        if (responses.empty())
            return "HTTP/1.0 200 OK\r\n\r\n";
        size_t index = sessions.size();
        if (index >= responses.size())
            index = responses.size() - 1;
        return responses[index];
    }

    void serve(int fd)
    {
        set_timeouts(fd);
        Session session;
        session.tls = false;
        session.handshake_ok = false;

        unsigned char peek[1];
        ssize_t peeked = recv(fd, peek, 1, MSG_PEEK);
        if (peeked == 1 && peek[0] == 0x16)
            serve_tls(fd, session);
        else
            serve_plain(fd, session);

        classify(session);
        sessions.push_back(session);
    }

    void serve_plain(int fd, Session &session)
    {
        char buf[1024];
        while (session.raw.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
                break;
            session.raw.append(buf, (size_t)n);
        }
        std::string response = next_response();
        if (!response.empty())
            send(fd, response.data(), response.size(), MSG_NOSIGNAL);
    }

    void serve_tls(int fd, Session &session)
    {
        session.tls = true;
        SSL *ssl = SSL_new(ctx);
        BIO *bio = BIO_new_socket(fd, BIO_NOCLOSE);
        BIO_set_callback_ex(bio, record_read);
        BIO_set_callback_arg(bio, (char *)&session.raw);
        SSL_set_bio(ssl, bio, bio);

        if (SSL_accept(ssl) == 1) {
            session.handshake_ok = true;
            char buf[1024];
            while (session.app.find("\r\n\r\n") == std::string::npos) {
                int n = SSL_read(ssl, buf, sizeof(buf));
                if (n <= 0)
                    break;
                session.app.append(buf, (size_t)n);
            }
            std::string response = next_response();
            if (!response.empty())
                SSL_write(ssl, response.data(), (int)response.size());
        }
        SSL_free(ssl);
    }
};

struct Harness {
    std::string dir;
    std::string cert;
    std::string key;
    bool ready;

    Harness(void) : ready(false) {}

    ~Harness(void)
    {
        if (!cert.empty())
            unlink(cert.c_str());
        if (!key.empty())
            unlink(key.c_str());
        if (!dir.empty())
            rmdir(dir.c_str());
    }

    bool init(void)
    {
        char tmpl[] = "/tmp/butt-login-test-XXXXXX";
        char *made = mkdtemp(tmpl);
        if (made == NULL)
            return false;
        dir = made;
        cert = dir + "/cert.pem";
        key = dir + "/key.pem";
        std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout '" + key +
                          "' -out '" + cert +
                          "' -days 1 -nodes -subj /CN=untrusted.invalid >/dev/null 2>&1";
        if (system(cmd.c_str()) != 0)
            return false;
        ready = true;
        return true;
    }
};

static bool exposed_session(const Session &session)
{
    return credential_exposed(session.raw) || credential_exposed(session.app);
}

static bool any_exposed(const std::vector<Session> &sessions)
{
    for (size_t i = 0; i < sessions.size(); i++) {
        if (exposed_session(sessions[i]))
            return true;
    }
    return false;
}

static void describe(const std::vector<Session> &sessions)
{
    fprintf(stderr, "  sessions=%zu\n", sessions.size());
    for (size_t i = 0; i < sessions.size(); i++) {
        const std::string &body = sessions[i].tls ? sessions[i].app : sessions[i].raw;
        const char *raw_reason = exposure_reason(sessions[i].raw);
        const char *app_reason = exposure_reason(sessions[i].app);
        fprintf(stderr,
                "  [%zu] kind=%s tls=%d handshake=%d bytes=%zu raw=%s app=%s\n",
                i, sessions[i].kind.c_str(), sessions[i].tls ? 1 : 0,
                sessions[i].handshake_ok ? 1 : 0, body.size(), raw_reason, app_reason);
    }
}

static void close_product_socket(void)
{
    if (cfg.srv != NULL && cfg.srv[0] != NULL && cfg.srv[0]->type == SHOUTCAST)
        sc_disconnect();
    else
        ic_disconnect();
    connected = false;
}

static int connect_local(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool harness_plaintext(Harness &harness)
{
    Catcher catcher;
    if (!catcher.open(harness.cert.c_str(), harness.key.c_str()))
        return false;
    catcher.start();

    int fd = connect_local(catcher.port);
    if (fd < 0) {
        catcher.stop();
        return false;
    }
    std::string pair = user_pass();
    char *owned = dupstr(pair.c_str());
    char *encoded = util_base64_enc(owned);
    std::string payload = std::string(kPassword) + "\nAuthorization: Basic " +
                          encoded + "\r\n\r\n";
    free(encoded);
    free(owned);
    send(fd, payload.data(), payload.size(), MSG_NOSIGNAL);
    close(fd);
    usleep(200000);
    catcher.stop();
    return catcher.sessions.size() == 1 && exposed_session(catcher.sessions[0]);
}

static bool harness_unverified_tls(Harness &harness)
{
    Catcher catcher;
    if (!catcher.open(harness.cert.c_str(), harness.key.c_str()))
        return false;
    catcher.start();

    int fd = connect_local(catcher.port);
    if (fd < 0) {
        catcher.stop();
        return false;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    bool wrote = false;
    if (SSL_connect(ssl) == 1) {
        std::string pair = user_pass();
        char *owned = dupstr(pair.c_str());
        char *encoded = util_base64_enc(owned);
        std::string payload = std::string("PUT /livemount HTTP/1.1\r\nAuthorization: Basic ") +
                              encoded + "\r\n" + kPassword + "\r\n\r\n";
        free(encoded);
        free(owned);
        wrote = SSL_write(ssl, payload.data(), (int)payload.size()) == (int)payload.size();
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    usleep(200000);
    catcher.stop();
    return wrote && catcher.sessions.size() == 1 && catcher.sessions[0].handshake_ok &&
           exposed_session(catcher.sessions[0]) &&
           !credential_exposed(catcher.sessions[0].raw);
}

static int run_product(Harness &harness, int type, int port_offset,
                       const std::vector<std::string> &responses, int (*fn)(void),
                       const char *name)
{
    Catcher catcher;
    if (!catcher.open(harness.cert.c_str(), harness.key.c_str())) {
        fprintf(stderr, "FAIL %s: listener did not open\n", name);
        return 2;
    }
    catcher.responses = responses;
    catcher.start();
    setup_server(type, (int)catcher.port + port_offset);
    int rc = fn();
    if (connected)
        close_product_socket();
    usleep(100000);
    catcher.stop();

    // No bytes at all means the password was not sent. A peer that never
    // receives the login cannot recover it.
    if (any_exposed(catcher.sessions)) {
        fprintf(stderr,
                "FAIL %s: source password was recoverable from bytes sent to an unverified peer (product returned %d)\n",
                name, rc);
        describe(catcher.sessions);
        return 1;
    }
    printf("PASS %s (product returned %d)\n", name, rc);
    return 0;
}

static int test_icecast_put(Harness &harness)
{
    return run_product(harness, ICECAST, 0,
                       std::vector<std::string>(1, "HTTP/1.0 200 OK\r\n\r\n"),
                       ic_connect, "icecast_put_login_does_not_expose_source_password");
}

static int test_icecast_source_fallback(Harness &harness)
{
    std::vector<std::string> responses;
    responses.push_back("HTTP/1.0 404 Not Found\r\n\r\n");
    responses.push_back("HTTP/1.0 200 OK\r\n\r\n");
    return run_product(harness, ICECAST, 0, responses, ic_connect,
                       "icecast_source_fallback_does_not_expose_source_password");
}

static int test_icecast_metadata(Harness &harness)
{
    return run_product(harness, ICECAST, 0, std::vector<std::string>(),
                       ic_update_song, "icecast_metadata_update_does_not_expose_source_password");
}

static int test_shoutcast_handshake(Harness &harness)
{
    // sc_connect dials the configured port plus one.
    return run_product(harness, SHOUTCAST, -1,
                       std::vector<std::string>(1, "OK\n"),
                       sc_connect, "shoutcast_handshake_does_not_expose_source_password");
}

static int test_shoutcast_metadata(Harness &harness)
{
    return run_product(harness, SHOUTCAST, 0, std::vector<std::string>(),
                       sc_update_song, "shoutcast_song_update_does_not_expose_source_password");
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    char *owned = dupstr(user_pass().c_str());
    char *encoded = util_base64_enc(owned);
    std::string decoded;
    bool encoder_roundtrip = encoded != NULL && b64_decode(encoded, decoded) &&
                             decoded == user_pass();
    free(encoded);
    free(owned);
    if (!encoder_roundtrip) {
        fprintf(stderr, "FAIL harness: product base64 encoder did not round-trip\n");
        return 2;
    }

    Harness harness;
    if (!harness.init()) {
        fprintf(stderr, "FAIL harness: could not prepare an untrusted certificate\n");
        return 2;
    }
    if (!harness_plaintext(harness)) {
        fprintf(stderr, "FAIL harness: plaintext capture did not observe the placeholder\n");
        return 2;
    }
    printf("PASS harness_plaintext_detector\n");
    if (!harness_unverified_tls(harness)) {
        fprintf(stderr, "FAIL harness: unverified TLS capture did not observe the placeholder\n");
        return 2;
    }
    printf("PASS harness_unverified_tls_detector\n");

    int failed = 0;
    failed += test_icecast_put(harness) != 0;
    failed += test_icecast_source_fallback(harness) != 0;
    failed += test_icecast_metadata(harness) != 0;
    failed += test_shoutcast_handshake(harness) != 0;
    failed += test_shoutcast_metadata(harness) != 0;
    free_cfg();

    if (failed != 0) {
        fprintf(stderr, "%d login test(s) failed\n", failed);
        return 1;
    }
    printf("all login tests passed\n");
    return 0;
}
