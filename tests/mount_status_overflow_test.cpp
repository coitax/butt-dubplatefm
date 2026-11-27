// Drives the real config parser, Icecast login, and connection-status
// copy. A fit case must print the mount and username. An overflow case
// stores strings whose status line does not fit in 256 bytes; the call
// must still return after the server accepts the login.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <FL/Fl_Choice.H>

#include "butt.h"
#include "cfg.h"
#include "fl_callbacks.h"
#include "flgui.h"
#include "icecast.h"
#include "port_audio.h"
#include "shoutcast.h"
#include "flac_encode.h"
#include "vorbis_encode.h"
#include "opus_encode.h"
#include "fl_timer_funcs.h"

bool record = false;
bool recording = false;
bool connected = false;
bool streaming = false;
bool disconnect = false;
bool try_connect = false;
bool song_timeout_running = false;
bool app_timeout_running = false;
bool try_to_connect = false;
bool pa_new_frames = false;
int stream_socket = -1;
sec_timer rec_timer;
sec_timer stream_timer;
vorbis_enc vorbis_stream;
opus_enc opus_stream;
flac_enc flac_rec;
const char *(*current_track_app)(void) = NULL;

static std::mutex g_info_mu;
static std::vector<std::string> g_infos;
static std::string g_request;
static std::atomic<int> g_accepts(0);
static int g_listen_fd = -1;
static int g_client_fd = -1;

static void fail(const char *msg)
{
    std::fprintf(stderr, "FAIL: %s\n", msg);
    std::exit(1);
}

void print_info(const char *info, int)
{
    if (info == NULL)
        return;
    std::lock_guard<std::mutex> lock(g_info_mu);
    g_infos.push_back(info);
}

void print_lcd(const char *, int, int) {}

void snd_reset_samplerate_conv(int) {}
void snd_start_stream(void) {}
void snd_stop_rec(void) {}
void snd_start_rec(void) {}

snd_dev_t **snd_get_devices(int *dev_count)
{
    *dev_count = 0;
    return NULL;
}

void vorbis_enc_write_header(vorbis_enc *)
{
    fail("ogg header path is not part of this case");
}

void opus_enc_write_header(opus_enc *)
{
    fail("opus header path is not part of this case");
}

currentTrackFunction getCurrentTrackFunctionFromId(int)
{
    fail("app-update path is not part of this case");
    return NULL;
}

void expand_string(char **) {}

int flac_enc_init(flac_enc *)
{
    return 0;
}

int flac_enc_init_FILE(flac_enc *, FILE *)
{
    return 0;
}

void display_info_timer(void *) {}
void is_connected_timer(void *) {}
void songfile_timer(void *) {}
void app_timer(void *) {}
void split_recording_timer(void *) {}

static int status_len(const char *mount, const char *user)
{
    return std::snprintf(NULL, 0, "Mounting point:\t%s\nUser:\t\t%s\n", mount, user);
}

static int listen_local(void)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        fail("socket");
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0)
        fail("bind");
    if (listen(fd, 1) != 0)
        fail("listen");
    return fd;
}

static int bound_port(int fd)
{
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (sockaddr *)&addr, &len) != 0)
        fail("getsockname");
    return ntohs(addr.sin_port);
}

static void *accept_login(void *)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(g_listen_fd, &rfds);
    timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    int ready = select(g_listen_fd + 1, &rfds, NULL, NULL, &tv);
    if (ready <= 0)
        return NULL;
    int client = accept(g_listen_fd, NULL, NULL);
    if (client < 0)
        return NULL;
    g_client_fd = client;
    g_accepts.fetch_add(1);
    std::string req;
    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(client, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (select(client + 1, &rfds, NULL, NULL, &tv) <= 0)
            break;
        char buf[1024];
        ssize_t n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        req.append(buf, buf + n);
        if (req.find("\r\n\r\n") != std::string::npos)
            break;
    }
    g_request = req;
    const char resp[] = "HTTP/1.0 200 OK\r\n\r\n";
    send(client, resp, sizeof(resp) - 1, 0);
    return NULL;
}

static std::string repeat_char(char c, size_t n)
{
    return std::string(n, c);
}

static void write_config(const std::string &path, int port,
                          const std::string &mount, const std::string &user)
{
    FILE *fp = std::fopen(path.c_str(), "wb");
    if (fp == NULL)
        fail("fopen config");
    std::fprintf(fp,
                 "[main]\n"
                 "server = station\n"
                 "num_of_srv = 1\n"
                 "num_of_icy = 0\n"
                 "srv_ent = station\n"
                 "song_update = 0\n"
                 "app_update = 0\n"
                 "connect_at_startup = 1\n"
                 "\n"
                 "[audio]\n"
                 "samplerate = 44100\n"
                 "bitrate = 128\n"
                 "channel = 2\n"
                 "codec = mp3\n"
                 "\n"
                 "[record]\n"
                 "start_rec = 0\n"
                 "codec = mp3\n"
                 "\n"
                 "[station]\n"
                 "address = 127.0.0.1\n"
                 "port = %d\n"
                 "password = placeholder-password\n"
                 "type = %d\n"
                 "mount = %s\n"
                 "usr = %s\n",
                 port, ICECAST, mount.c_str(), user.c_str());
    std::fclose(fp);
}

static void prepare_widgets(void)
{
    alignas(flgui) static unsigned char raw[sizeof(flgui)];
    std::memset(raw, 0, sizeof(raw));
    fl_g = reinterpret_cast<flgui *>(raw);
    fl_g->choice_cfg_dev = new Fl_Choice(0, 0, 80, 24, "dev");
    fl_g->choice_cfg_codec = new Fl_Choice(0, 0, 80, 24, "codec");
}

static bool status_has_identity(const std::string &mount, const std::string &user)
{
    std::lock_guard<std::mutex> lock(g_info_mu);
    for (size_t i = 0; i < g_infos.size(); i++) {
        const std::string &msg = g_infos[i];
        if (msg.find("Mounting point:") != std::string::npos &&
            msg.find(mount) != std::string::npos &&
            msg.find(user) != std::string::npos)
            return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc != 3)
        fail("usage: mount_status_overflow_test <fit|overflow> <name>");
    std::string kind = argv[1];
    std::string name = argv[2];
    setlocale(LC_ALL, "C");
    signal(SIGPIPE, SIG_IGN);

    std::string mount = "live";
    std::string user = "source";
    if (name == "short") {
        mount = "live";
        user = "source";
    } else if (name == "gui-max") {
        mount = repeat_char('m', 100);
        user = repeat_char('u', 100);
    } else if (name == "mount") {
        mount = repeat_char('M', 1000);
        user = "source";
    } else if (name == "user") {
        mount = "live";
        user = repeat_char('U', 1000);
    } else if (name == "both") {
        mount = repeat_char('M', 200);
        user = repeat_char('U', 200);
    } else {
        fail("unknown case");
    }

    g_listen_fd = listen_local();
    int port = bound_port(g_listen_fd);
    pthread_t server;
    if (pthread_create(&server, NULL, accept_login, NULL) != 0)
        fail("pthread_create server");

    char tmpl[] = "/tmp/butt-mount-status-XXXXXX";
    int cfgfd = mkstemp(tmpl);
    if (cfgfd < 0)
        fail("mkstemp");
    close(cfgfd);
    write_config(tmpl, port, mount, user);

    int loaded = cfg_set_values(tmpl);
    std::remove(tmpl);
    if (loaded != 0) {
        if (kind == "overflow") {
            pthread_join(server, NULL);
            close(g_listen_fd);
            return 0;
        }
        fail("fit config was rejected");
    }
    if (cfg.main.num_of_srv < 1 || cfg.srv[cfg.selected_srv] == NULL)
        fail("server entry missing after parse");
    if (cfg.srv[cfg.selected_srv]->mount == NULL || cfg.srv[cfg.selected_srv]->usr == NULL)
        fail("mount or user missing after parse");
    if (cfg.main.connect_at_startup != 1)
        fail("connect_at_startup was not loaded");

    const char *stored_mount = cfg.srv[cfg.selected_srv]->mount;
    const char *stored_user = cfg.srv[cfg.selected_srv]->usr;
    int stored_status = status_len(stored_mount, stored_user);
    bool still_overflows = stored_status >= 256;

    if (kind == "fit" && still_overflows)
        fail("fit case does not fit in the 256-byte status buffer");
    if (kind == "overflow" && !still_overflows) {
        kind = "fit";
    }
    if (kind == "overflow") {
        if (std::strcmp(stored_mount, mount.c_str()) != 0)
            fail("parser changed the mount before the status copy");
        if (std::strcmp(stored_user, user.c_str()) != 0)
            fail("parser changed the user before the status copy");
    }

    prepare_widgets();
    connected = false;
    button_connect_cb();

    pthread_join(server, NULL);
    if (g_client_fd >= 0)
        close(g_client_fd);
    close(g_listen_fd);

    if (!connected)
        fail("Icecast login did not complete, status copy was not reached");
    if (g_accepts.load() < 1)
        fail("login server accepted no connection");
    if (g_request.find("PUT ") == std::string::npos && g_request.find("SOURCE ") == std::string::npos)
        fail("login request was not an Icecast source request");
    std::string mount_prefix = mount.substr(0, 32);
    if (g_request.find(mount_prefix) == std::string::npos)
        fail("login request did not carry the configured mount");

    if (kind == "fit") {
        if (!status_has_identity(stored_mount, stored_user))
            fail("status line did not contain the configured mount and user");
    }
    return 0;
}
