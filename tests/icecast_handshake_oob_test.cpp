// Exercises ic_connect() against a local TCP peer.
//
// The peer stands in for an Icecast server the broadcaster was pointed at,
// or for whoever answers that plaintext source connection. Payloads are the
// status line bytes that peer writes on the socket. The test links the
// product's icecast.cpp and sockfuncs.cpp; it does not reimplement them.
//
// Build with AddressSanitizer (tests/run_icecast_handshake_oob.sh). A 249-byte
// status line whose first space sits at offset 246, 247, or 248 must be
// rejected: ic_connect() returns non-zero and connected stays false. The
// search then writes the NUL at offset+4, which is past the 250-byte buffer.
// AddressSanitizer aborts on that write, so this program does not exit 0
// until the handshake stops at the buffer.

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

#include "butt.h"
#include "cfg.h"
#include "icecast.h"
#include "timer.h"

config_t cfg;
char *cfg_path = NULL;
bool unsaved_changes = false;
bool connected = false;
int stream_socket = -1;
sec_timer stream_timer;

void print_info(const char *info, int info_type)
{
    (void)info;
    (void)info_type;
}

namespace {

const size_t kStatusRead = 249;

struct Server {
    int listen_fd;
    std::vector<std::vector<unsigned char> > payloads;
    std::atomic<int> accepts;
    std::atomic<bool> stop;
};

void configure(int port)
{
    static server_t server;
    static server_t *list[1];
    static char addr[] = "127.0.0.1";
    static char mount[] = "/live";
    static char user[] = "source";
    static char password[] = "placeholder";
    static char name[] = "test";
    static char codec[] = "mp3";

    memset(&cfg, 0, sizeof(cfg));
    memset(&server, 0, sizeof(server));
    server.name = name;
    server.addr = addr;
    server.pwd = password;
    server.mount = mount;
    server.usr = user;
    server.port = port;
    server.type = ICECAST;
    list[0] = &server;
    cfg.srv = list;
    cfg.selected_srv = 0;
    cfg.main.num_of_srv = 1;
    cfg.main.num_of_icy = 0;
    cfg.audio.codec = codec;
    cfg.audio.bitrate = 128;
    cfg.audio.channel = 2;
    cfg.audio.samplerate = 44100;
    connected = false;
}

int listen_loopback(int *port_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 2) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (sockaddr *)&addr, &len) != 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(addr.sin_port);
    return fd;
}

bool read_request(int fd)
{
    std::string req;
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0)
            return false;
        char buf[512];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            return false;
        req.append(buf, buf + n);
    }
    return req.find("\r\n\r\n") != std::string::npos;
}

void *serve(void *arg)
{
    Server *server = static_cast<Server *>(arg);
    while (!server->stop.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server->listen_fd, &rfds);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int sel = select(server->listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (server->stop.load())
            break;
        if (sel <= 0)
            continue;
        int fd = accept(server->listen_fd, NULL, NULL);
        if (fd < 0)
            continue;
        if (!read_request(fd)) {
            close(fd);
            continue;
        }
        int naccept = server->accepts.load();
        size_t idx = (size_t)naccept;
        if (idx >= server->payloads.size())
            idx = server->payloads.size() - 1;
        const std::vector<unsigned char> &payload = server->payloads[idx];
        size_t sent = 0;
        while (sent < payload.size()) {
            ssize_t n = send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
            if (n <= 0)
                break;
            sent += (size_t)n;
        }
        server->accepts.fetch_add(1);
        shutdown(fd, SHUT_WR);
        char drain[256];
        while (recv(fd, drain, sizeof(drain), 0) > 0) {
        }
        close(fd);
    }
    return NULL;
}

std::vector<unsigned char> bytes_of(const char *text)
{
    return std::vector<unsigned char>(text, text + strlen(text));
}

std::vector<unsigned char> filled_status(size_t space_at, char d0, char d1, char d2)
{
    std::vector<unsigned char> payload(kStatusRead, 'A');
    payload[space_at] = ' ';
    if (space_at + 1 < payload.size())
        payload[space_at + 1] = (unsigned char)d0;
    if (space_at + 2 < payload.size())
        payload[space_at + 2] = (unsigned char)d1;
    if (space_at + 3 < payload.size())
        payload[space_at + 3] = (unsigned char)d2;
    return payload;
}

int run_case(const char *name,
             const std::vector<std::vector<unsigned char> > &payloads,
             bool want_success,
             int min_accepts)
{
    int port = 0;
    int listen_fd = listen_loopback(&port);
    if (listen_fd < 0) {
        fprintf(stderr, "FAIL %s: could not listen: %s\n", name, strerror(errno));
        return 1;
    }
    Server server;
    server.listen_fd = listen_fd;
    server.payloads = payloads;
    server.accepts.store(0);
    server.stop.store(false);
    pthread_t thread;
    if (pthread_create(&thread, NULL, serve, &server) != 0) {
        fprintf(stderr, "FAIL %s: pthread_create\n", name);
        close(listen_fd);
        return 1;
    }

    configure(port);
    int rc = ic_connect();
    bool was_connected = connected;
    if (was_connected)
        ic_disconnect();

    server.stop.store(true);
    shutdown(listen_fd, SHUT_RDWR);
    close(listen_fd);
    pthread_join(thread, NULL);

    if (server.accepts.load() < min_accepts) {
        fprintf(stderr, "FAIL %s: peer answered %d status line(s), need %d (rc=%d)\n",
                name, server.accepts.load(), min_accepts, rc);
        return 1;
    }
    if (want_success) {
        if (rc != 0 || !was_connected) {
            fprintf(stderr, "FAIL %s: expected a completed handshake, rc=%d connected=%d\n",
                    name, rc, was_connected ? 1 : 0);
            return 1;
        }
    } else if (rc == 0 || was_connected) {
        fprintf(stderr, "FAIL %s: malformed status was accepted, rc=%d connected=%d\n",
                name, rc, was_connected ? 1 : 0);
        return 1;
    }
    fprintf(stderr, "PASS %s rc=%d\n", name, rc);
    return 0;
}

int run_one(const char *name, const std::vector<unsigned char> &payload, bool want_success)
{
    std::vector<std::vector<unsigned char> > payloads;
    payloads.push_back(payload);
    return run_case(name, payloads, want_success, 1);
}

}  // namespace

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    const char *only = (argc > 1) ? argv[1] : NULL;
    int failed = 0;

    struct Case {
        const char *name;
        int kind;
        size_t space_at;
    };
    const Case cases[] = {
        {"well_formed_200_connects", 0, 0},
        {"unauthorized_401_is_rejected", 1, 0},
        {"space_at_245_status_200_connects", 2, 245},
        {"space_at_246_is_rejected_without_overflow", 2, 246},
        {"space_at_247_is_rejected_without_overflow", 2, 247},
        {"space_at_248_is_rejected_without_overflow", 2, 248},
        {"put_404_then_source_overflow_is_rejected", 4, 247},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (only != NULL && strcmp(only, cases[i].name) != 0)
            continue;
        int rc = 0;
        if (cases[i].kind == 0) {
            rc = run_one(cases[i].name, bytes_of("HTTP/1.0 200 OK\r\n"), true);
        } else if (cases[i].kind == 1) {
            rc = run_one(cases[i].name, bytes_of("HTTP/1.0 401 Unauthorized\r\n"), false);
        } else if (cases[i].kind == 2) {
            char d0 = '9', d1 = '9', d2 = '9';
            bool success = false;
            if (cases[i].space_at == 245) {
                d0 = '2';
                d1 = '0';
                d2 = '0';
                success = true;
            }
            std::vector<unsigned char> payload = filled_status(cases[i].space_at, d0, d1, d2);
            if (payload.size() != kStatusRead || payload[cases[i].space_at] != ' ') {
                fprintf(stderr, "FAIL %s: payload was not built as a 249-byte status line\n",
                        cases[i].name);
                failed++;
                continue;
            }
            rc = run_one(cases[i].name, payload, success);
        } else {
            std::vector<std::vector<unsigned char> > payloads;
            payloads.push_back(bytes_of("HTTP/1.0 404 Not Found\r\n"));
            payloads.push_back(filled_status(247, '9', '9', '9'));
            rc = run_case(cases[i].name, payloads, false, 2);
        }
        failed += rc;
    }

    if (only != NULL && failed == 0) {
        bool found = false;
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            if (strcmp(only, cases[i].name) == 0)
                found = true;
        }
        if (!found) {
            fprintf(stderr, "unknown case %s\n", only);
            return 2;
        }
    }
    return failed == 0 ? 0 : 1;
}
