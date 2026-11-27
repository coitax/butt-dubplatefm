/* Spec for the configuration-loader heap overflow.
 *
 * These tests fail while src/cfg.cpp walks every semicolon-separated server
 * or stream-info name after allocating only num_of_srv / num_of_icy slots.
 * They pass once that walk cannot touch memory past the allocated arrays.
 *
 * Both product entries into the loader are covered:
 *   - startup: butt.cpp passes the -c path through cfg_path and calls
 *     cfg_set_values(NULL)
 *   - import: button_cfg_import_cb calls cfg_set_values(filename)
 *
 * The add/remove server and stream-info callbacks in src/FLTK/fl_callbacks.cpp
 * are not covered. They resize the array in the same step as they change the
 * count, and they do not parse a declared count separately from a
 * semicolon-separated list. A num_of_srv of 0 or less never enters the walk,
 * so that case is not the reported bug either. An oversized count that matches
 * the number of names is a separate allocation problem and is not asserted.
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

#include <FL/fl_utf8.h>

#include "cfg.h"
#include "port_audio.h"

extern "C" const char *__asan_default_options()
{
    return "detect_leaks=0:abort_on_error=1:halt_on_error=1";
}

/* FLTK 1.3.8 fl_fopen (src/fl_utf8.cxx, non-Windows) is a direct call to
 * fopen. The libfltk.so.1.3 shipped with the 1.3.8-6.1 package is the same
 * jump. Defining it here keeps the loader test off the GUI library so an
 * error dialog cannot block the process, and so the open step stays the one
 * the parser actually uses.
 */
extern "C" FILE *fl_fopen(const char *f, const char *mode)
{
    return fopen(f, mode);
}

void fl_alert(const char *fmt, ...)
{
    (void)fmt;
}

void print_info(const char *info, int info_type)
{
    (void)info;
    (void)info_type;
}

/* snd_get_devices reads the audio device list, not the server or stream-info
 * lists. The loader calls it before those lists are parsed. A one-entry
 * result lets the device-number clamp in cfg_set_values finish.
 */
snd_dev_t **snd_get_devices(int *dev_count)
{
    snd_dev_t **dev_list = (snd_dev_t **)malloc(sizeof(snd_dev_t *));
    dev_list[0] = (snd_dev_t *)calloc(1, sizeof(snd_dev_t));
    dev_list[0]->name = strdup("test-device");
    dev_list[0]->dev_id = 0;
    *dev_count = 1;
    return dev_list;
}

static const char *g_argv0;

enum Expect {
    EXPECT_LOADED = 0,
    EXPECT_CONTAINED = 1
};

struct Case {
    const char *name;
    Expect expect;
    bool startup_path;
    int num_of_srv;
    int num_of_icy;
    const char *srv_ent;
    const char *icy_ent;
    bool omit_alpha_address;
    const char *expect_srv0;
    int expect_port0;
    const char *expect_srv1;
    const char *expect_icy0;
    const char *expect_icy1;
};

static std::string server_section(const char *name, int port, bool with_address)
{
    std::string out = "[";
    out += name;
    out += "]\n";
    if (with_address)
        out += "address = 127.0.0.1\n";
    out += "port = " + std::to_string(port) + "\n";
    out += "password = placeholder\n";
    out += "type = 0\n\n";
    return out;
}

static std::string icy_section(const char *name)
{
    std::string out = "[";
    out += name;
    out += "]\n";
    out += "pub = 0\n\n";
    return out;
}

static std::string build_config(const Case &c)
{
    std::string out;
    out += "[main]\n";
    out += "server = alpha\n";
    out += "icy = streamone\n";
    out += "srv_ent = ";
    out += c.srv_ent;
    out += "\n";
    out += "icy_ent = ";
    out += c.icy_ent;
    out += "\n";
    out += "num_of_srv = " + std::to_string(c.num_of_srv) + "\n";
    out += "num_of_icy = " + std::to_string(c.num_of_icy) + "\n";
    out += "gain = 1.0\n";
    out += "connect_at_startup = 0\n";
    out += "song_update = 0\n";
    out += "app_update = 0\n";
    out += "app_update_service = 0\n\n";
    out += "[audio]\n";
    out += "device = 0\n";
    out += "samplerate = 44100\n";
    out += "bitrate = 128\n";
    out += "channel = 2\n";
    out += "codec = mp3\n";
    out += "resample_mode = 0\n";
    out += "buffer_ms = 50\n\n";
    out += "[record]\n";
    out += "bitrate = 192\n";
    out += "codec = mp3\n";
    out += "start_rec = 0\n";
    out += "sync_to_hour = 0\n";
    out += "split_time = 0\n";
    out += "filename = rec.mp3\n";
    out += "folder = ./\n\n";
    out += "[dsp]\n";
    out += "compressor = 0\n";
    out += "compQuantity = 0.3\n";
    out += "equalizer = 0\n";
    out += "gain1 = 0.0\n";
    out += "gain2 = 0.0\n";
    out += "gain3 = 0.0\n";
    out += "gain4 = 0.0\n";
    out += "gain5 = 0.0\n\n";
    out += "[gui]\n";
    out += "attach = 0\n";
    out += "ontop = 0\n";
    out += "lcd_auto = 0\n\n";
    out += server_section("alpha", 8000, !c.omit_alpha_address);
    out += server_section("beta", 8001, true);
    out += server_section("gamma", 8002, true);
    out += server_section("delta", 8003, true);
    out += icy_section("streamone");
    out += icy_section("streamtwo");
    out += icy_section("streamthree");
    return out;
}

static bool write_file(const char *path, const std::string &body)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL)
        return false;
    size_t n = fwrite(body.data(), 1, body.size(), fp);
    if (fclose(fp) != 0)
        return false;
    return n == body.size();
}

static bool read_file(const char *path, std::string *out)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return false;
    char buf[4096];
    out->clear();
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        out->append(buf, n);
    bool bad = ferror(fp) != 0;
    fclose(fp);
    return !bad;
}

static int touch_loaded(const Case &c)
{
    if (cfg.main.num_of_srv < 1 || cfg.srv == NULL || cfg.srv[0] == NULL || cfg.srv[0]->name == NULL)
        return 3;
    if (std::strcmp(cfg.srv[0]->name, "alpha") != 0)
        return 3;
    if (std::strcmp(cfg.srv[0]->pwd, "placeholder") != 0)
        return 3;
    for (int i = 0; i < cfg.main.num_of_srv; i++) {
        if (cfg.srv[i] == NULL || cfg.srv[i]->name == NULL || cfg.srv[i]->addr == NULL)
            return 3;
        if (cfg.srv[i]->port < 1)
            return 3;
    }
    if (cfg.main.num_of_icy < 1 || cfg.icy == NULL || cfg.icy[0] == NULL || cfg.icy[0]->name == NULL)
        return 3;
    if (std::strcmp(cfg.icy[0]->name, "streamone") != 0)
        return 3;
    for (int i = 0; i < cfg.main.num_of_icy; i++) {
        if (cfg.icy[i] == NULL || cfg.icy[i]->name == NULL || cfg.icy[i]->pub == NULL)
            return 3;
    }
    if (c.expect == EXPECT_LOADED) {
        if (cfg.main.num_of_srv != c.num_of_srv || cfg.main.num_of_icy != c.num_of_icy)
            return 3;
        if (cfg.srv[0]->port != c.expect_port0)
            return 3;
        if (c.expect_srv0 != NULL && std::strcmp(cfg.srv[0]->name, c.expect_srv0) != 0)
            return 3;
        if (c.expect_srv1 != NULL) {
            if (cfg.main.num_of_srv < 2 || std::strcmp(cfg.srv[1]->name, c.expect_srv1) != 0)
                return 3;
        }
        if (c.expect_icy0 != NULL && std::strcmp(cfg.icy[0]->name, c.expect_icy0) != 0)
            return 3;
        if (c.expect_icy1 != NULL) {
            if (cfg.main.num_of_icy < 2 || std::strcmp(cfg.icy[1]->name, c.expect_icy1) != 0)
                return 3;
        }
    }
    return 0;
}

static int child_main(const Case &c, const char *path)
{
    int rc;
    if (c.startup_path) {
        cfg_path = (char *)path;
        rc = cfg_set_values(NULL);
    } else {
        rc = cfg_set_values((char *)path);
    }

    if (rc != 0)
        return 1;

    return touch_loaded(c);
}

static const Case *find_case(const char *name)
{
    static const Case kCases[] = {
        {"matching_import_one", EXPECT_LOADED, false, 1, 1, "alpha", "streamone", false,
         "alpha", 8000, NULL, "streamone", NULL},
        {"matching_import_two", EXPECT_LOADED, false, 2, 2, "alpha;beta", "streamone;streamtwo", false,
         "alpha", 8000, "beta", "streamone", "streamtwo"},
        {"matching_startup_one", EXPECT_LOADED, true, 1, 1, "alpha", "streamone", false,
         "alpha", 8000, NULL, "streamone", NULL},
        {"extra_servers_on_startup_path", EXPECT_CONTAINED, true, 1, 1, "alpha;beta;gamma", "streamone", false,
         NULL, 0, NULL, NULL, NULL},
        {"extra_servers_on_import_path", EXPECT_CONTAINED, false, 1, 1, "alpha;beta", "streamone", false,
         NULL, 0, NULL, NULL, NULL},
        {"extra_icy_on_import_path", EXPECT_CONTAINED, false, 1, 1, "alpha", "streamone;streamtwo", false,
         NULL, 0, NULL, NULL, NULL},
        {"extra_servers_and_icy_on_import_path", EXPECT_CONTAINED, false, 1, 1,
         "alpha;beta", "streamone;streamtwo;streamthree", false, NULL, 0, NULL, NULL, NULL},
        {"extra_servers_past_two_slots", EXPECT_CONTAINED, false, 2, 1, "alpha;beta;gamma;delta", "streamone", false,
         NULL, 0, NULL, NULL, NULL},
        {"extra_servers_double_semicolon", EXPECT_CONTAINED, false, 1, 1, "alpha;;beta", "streamone", false,
         NULL, 0, NULL, NULL, NULL},
        {"missing_address_rejects_without_overflow", EXPECT_CONTAINED, false, 1, 1, "alpha", "streamone", true,
         NULL, 0, NULL, NULL, NULL},
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        if (std::strcmp(kCases[i].name, name) == 0)
            return &kCases[i];
    }
    return NULL;
}

static int run_case(const Case &c)
{
    char dir[] = "/tmp/butt-cfg-XXXXXX";
    if (mkdtemp(dir) == NULL) {
        std::fprintf(stderr, "FAIL %s: mkdtemp: %s\n", c.name, std::strerror(errno));
        return 1;
    }
    std::string path = std::string(dir) + "/buttrc";
    std::string body = build_config(c);
    if (!write_file(path.c_str(), body)) {
        std::fprintf(stderr, "FAIL %s: could not write %s\n", c.name, path.c_str());
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "FAIL %s: fork: %s\n", c.name, std::strerror(errno));
        return 1;
    }
    if (pid == 0) {
        execl(g_argv0, g_argv0, "--child", c.name, path.c_str(), (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::fprintf(stderr, "FAIL %s: waitpid: %s\n", c.name, std::strerror(errno));
        return 1;
    }

    std::string after;
    bool file_ok = read_file(path.c_str(), &after) && after == body;
    bool extra_file = false;
    DIR *dirp = opendir(dir);
    if (dirp == NULL) {
        std::fprintf(stderr, "FAIL %s: could not read %s after the loader ran\n", c.name, dir);
        return 1;
    }
    struct dirent *ent;
    while ((ent = readdir(dirp)) != NULL) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0)
            continue;
        if (std::strcmp(ent->d_name, "buttrc") != 0)
            extra_file = true;
    }
    closedir(dirp);

    if (!file_ok) {
        std::fprintf(stderr, "FAIL %s: configuration file was created, removed, or modified\n", c.name);
        return 1;
    }
    if (extra_file) {
        std::fprintf(stderr, "FAIL %s: loader left an extra file beside the configuration\n", c.name);
        return 1;
    }

    if (WIFSIGNALED(status)) {
        std::fprintf(stderr,
                     "FAIL %s: loader crashed with signal %d (out-of-bounds access while parsing the list)\n",
                     c.name, WTERMSIG(status));
        return 1;
    }
    if (!WIFEXITED(status)) {
        std::fprintf(stderr, "FAIL %s: loader did not exit normally\n", c.name);
        return 1;
    }

    int code = WEXITSTATUS(status);
    if (code == 127) {
        std::fprintf(stderr, "FAIL %s: could not exec the test child\n", c.name);
        return 1;
    }
    if (code == 3 || code == 2) {
        std::fprintf(stderr, "FAIL %s: loader returned a loaded configuration that does not match the file\n", c.name);
        return 1;
    }

    if (c.expect == EXPECT_LOADED) {
        if (code != 0) {
            std::fprintf(stderr, "FAIL %s: matching list was rejected (exit %d)\n", c.name, code);
            return 1;
        }
        std::printf("PASS %s\n", c.name);
        std::fflush(stdout);
        return 0;
    }

    /* Contained means the process left the loader without a signal.
     * Exit 0: the walk stayed inside the allocated arrays and the entries
     * that were reported can be read.
     * Exit 1: the file was refused. The current loader uses exit(1) for a
     * parse error, which is also a refusal.
     */
    if (code == 0 || code == 1) {
        std::printf("PASS %s\n", c.name);
        std::fflush(stdout);
        return 0;
    }
    std::fprintf(stderr, "FAIL %s: unexpected exit status %d\n", c.name, code);
    return 1;
}

int main(int argc, char **argv)
{
    g_argv0 = argv[0];

    if (argc == 4 && std::strcmp(argv[1], "--child") == 0) {
        const Case *c = find_case(argv[2]);
        if (c == NULL)
            return 2;
        return child_main(*c, argv[3]);
    }

    static const char *kOrder[] = {
        "matching_import_one",
        "matching_import_two",
        "matching_startup_one",
        "extra_servers_on_startup_path",
        "extra_servers_on_import_path",
        "extra_icy_on_import_path",
        "extra_servers_and_icy_on_import_path",
        "extra_servers_past_two_slots",
        "extra_servers_double_semicolon",
        "missing_address_rejects_without_overflow",
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); i++) {
        const Case *c = find_case(kOrder[i]);
        if (c == NULL || run_case(*c) != 0)
            failed++;
    }
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("all configuration list bounds tests passed\n");
    return 0;
}
