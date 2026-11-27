/*
 * Spec for the codec-loader heap over-read.
 *
 * cfg_set_values() copies the audio and record codec pointers out of the
 * parser and then reallocates each block to five bytes without writing a
 * terminator. A value longer than four characters has no NUL inside that
 * block. Later C-string uses of those pointers (the settings and connect
 * comparisons, strlen in the record-extension helper, the connect-status
 * snprintf, and fprintf %s in cfg_write_file) therefore read past the block.
 *
 * These checks fail while that shrink is in place and pass once each stored
 * codec is a terminated C string. A known name (mp3, ogg, opus, aac, and for
 * recording also flac and wav) must round-trip. Any other value must round-trip
 * with its terminator intact, or be replaced by the loader's existing default
 * for a missing codec, "mp3".
 *
 * Operations that share the bad pointers but are not called here, because
 * they need the FLTK widgets or a live server: fill_cfg_widgets widget
 * updates, button_connect_cb's network connect, and test_file_extension's
 * filename rewrite. The strcmp / strlen / %s reads those functions perform
 * are executed below on the pointers cfg_set_values stored.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "cfg.h"
#include "port_audio.h"

void print_info(const char *info, int info_type)
{
    (void)info;
    (void)info_type;
}

/* cfg_set_values asks for the device list before it shrinks the codec.
   The real enumerator only reads PortAudio and cfg.audio.channel. */
snd_dev_t **snd_get_devices(int *dev_count)
{
    snd_dev_t **list = (snd_dev_t **)malloc(sizeof(snd_dev_t *));
    list[0] = (snd_dev_t *)calloc(1, sizeof(snd_dev_t));
    list[0]->name = strdup("Default PCM device (default)");
    list[0]->dev_id = -1;
    *dev_count = 1;
    return list;
}

static int g_failures = 0;

static void fail(const char *msg)
{
    std::fprintf(stderr, "FAIL: %s\n", msg);
    g_failures++;
}

static std::string escape(const char *s)
{
    std::string out;
    for (size_t i = 0; s[i] != '\0' && i < 80; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c < 127 && c != '\\')
            out.push_back((char)c);
        else {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\x%02x", c);
            out += hex;
        }
    }
    return out;
}

/* Literals compared in fill_cfg_widgets, button_connect_cb, icecast,
   shoutcast, and port_audio. Every literal is applied so a match on the
   first name cannot skip a later read. */
static void compare_audio_codec(const char *codec)
{
    (void)std::strcmp(codec, "mp3");
    (void)std::strcmp(codec, "ogg");
    (void)std::strcmp(codec, "opus");
    (void)std::strcmp(codec, "aac");
}

static void compare_record_codec(const char *codec)
{
    (void)std::strcmp(codec, "mp3");
    (void)std::strcmp(codec, "ogg");
    (void)std::strcmp(codec, "opus");
    (void)std::strcmp(codec, "flac");
    (void)std::strcmp(codec, "wav");
    (void)std::strcmp(codec, "aac");
}

static bool known_audio(const char *s)
{
    return !std::strcmp(s, "mp3") || !std::strcmp(s, "ogg")
        || !std::strcmp(s, "opus") || !std::strcmp(s, "aac");
}

static bool known_record(const char *s)
{
    return known_audio(s) || !std::strcmp(s, "flac") || !std::strcmp(s, "wav");
}

/* Scan the stored codec the way the product does: strcmp against the known
   names, strlen (record extension), and snprintf %s (connect status). */
static void use_codec(const char *codec, const char *supplied, bool record)
{
    if (record)
        compare_record_codec(codec);
    else
        compare_audio_codec(codec);

    /* strlen is what test_file_extension uses before it appends the codec.
       snprintf %s is what button_connect_cb writes into the status text
       and what cfg_write_file writes with fprintf. */
    size_t n = std::strlen(codec);
    char formatted[256];
    std::snprintf(formatted, sizeof(formatted), "Codec:\t\t%s\n", codec);

    bool known = record ? known_record(supplied) : known_audio(supplied);
    bool preserved = std::strcmp(codec, supplied) == 0;
    bool defaulted = !known && std::strcmp(codec, "mp3") == 0;
    if (!preserved && !defaulted) {
        std::fprintf(stderr,
                     "FAIL: %s codec scanned as \"%s\" (len %zu); "
                     "expected \"%s\" or the default mp3\n",
                     record ? "record" : "audio",
                     escape(codec).c_str(), n, supplied);
        g_failures++;
    } else if (std::strstr(formatted, codec) == NULL) {
        std::fprintf(stderr,
                     "FAIL: connect-status text \"%s\" dropped the codec\n",
                     escape(formatted).c_str());
        g_failures++;
    }
}

static bool write_config(const char *path, const char *audio, const char *record)
{
    FILE *fp = std::fopen(path, "wb");
    if (fp == NULL)
        return false;
    std::fprintf(fp,
                 "[main]\n"
                 "server = station\n"
                 "num_of_srv = 1\n"
                 "num_of_icy = 0\n"
                 "srv_ent = station\n"
                 "song_update = 0\n"
                 "app_update = 0\n"
                 "app_update_service = 0\n"
                 "gain = 1.0\n"
                 "connect_at_startup = 0\n"
                 "\n"
                 "[audio]\n"
                 "device = 0\n"
                 "samplerate = 44100\n"
                 "bitrate = 128\n"
                 "channel = 2\n"
                 "codec = %s\n"
                 "resample_mode = 0\n"
                 "buffer_ms = 50\n"
                 "\n"
                 "[record]\n"
                 "bitrate = 192\n"
                 "codec = %s\n"
                 "start_rec = 0\n"
                 "sync_to_hour = 0\n"
                 "split_time = 0\n"
                 "filename = recording.mp3\n"
                 "folder = ./\n"
                 "\n"
                 "[dsp]\n"
                 "compressor = 0\n"
                 "compQuantity = 0.3\n"
                 "equalizer = 0\n"
                 "gain1 = 0.0\n"
                 "gain2 = 0.0\n"
                 "gain3 = 0.0\n"
                 "gain4 = 0.0\n"
                 "gain5 = 0.0\n"
                 "\n"
                 "[gui]\n"
                 "attach = 0\n"
                 "ontop = 0\n"
                 "lcd_auto = 0\n"
                 "\n"
                 "[station]\n"
                 "address = 127.0.0.1\n"
                 "port = 8000\n"
                 "password = placeholder\n"
                 "type = 1\n"
                 "mount = live\n"
                 "usr = source\n",
                 audio, record);
    std::fclose(fp);
    return true;
}

static void expect_written_codec(const char *path, const char *key_owner,
                                  const char *expect, int which)
{
    FILE *fp = std::fopen(path, "rb");
    if (fp == NULL) {
        fail("could not read config written by cfg_write_file");
        return;
    }
    char line[512];
    int seen = 0;
    const char *found = NULL;
    while (std::fgets(line, sizeof(line), fp) != NULL) {
        if (std::strncmp(line, "codec = ", 8) != 0)
            continue;
        if (seen == which) {
            char *nl = std::strchr(line, '\n');
            if (nl)
                *nl = '\0';
            found = line + 8;
            if (std::strcmp(found, expect) != 0) {
                std::fprintf(stderr,
                             "FAIL: %s codec written as \"%s\"; expected \"%s\"\n",
                             key_owner, escape(found).c_str(), expect);
                g_failures++;
            }
            break;
        }
        seen++;
    }
    if (found == NULL) {
        std::fprintf(stderr, "FAIL: written config has no codec entry for %s\n",
                     key_owner);
        g_failures++;
    }
    std::fclose(fp);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <audio-codec> <record-codec>\n", argv[0]);
        return 2;
    }
    const char *audio = argv[1];
    const char *record = argv[2];

    char in_path[] = "/tmp/butt-codec-oob-in-XXXXXX";
    char out_path[] = "/tmp/butt-codec-oob-out-XXXXXX";
    int in_fd = mkstemp(in_path);
    int out_fd = mkstemp(out_path);
    if (in_fd < 0 || out_fd < 0) {
        fail("mkstemp");
        return 1;
    }
    std::fclose(fdopen(in_fd, "w"));
    close(out_fd);

    if (!write_config(in_path, audio, record)) {
        fail("could not write input config");
        return 1;
    }

    if (cfg_set_values(in_path) != 0) {
        fail("cfg_set_values rejected the config");
        return 1;
    }

    use_codec(cfg.audio.codec, audio, false);
    use_codec(cfg.rec.codec, record, true);

    if (cfg_write_file(out_path) != 0)
        fail("cfg_write_file failed");
    else if (g_failures == 0) {
        /* The bytes written for each codec must be the terminated value
           just scanned, not a longer run that picked up neighboring heap. */
        expect_written_codec(out_path, "audio", cfg.audio.codec, 0);
        expect_written_codec(out_path, "record", cfg.rec.codec, 1);
    }

    std::remove(in_path);
    std::remove(out_path);
    return g_failures == 0 ? 0 : 1;
}
