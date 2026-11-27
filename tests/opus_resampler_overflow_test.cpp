// Exercises the Opus resampler heap overflow.
//
// Startup (src/butt.cpp) calls snd_init while pa_frames is still the 2048
// default, which mallocs the resampler buffers, and only afterwards reads
// the configuration and opens the device at
// (buffer_ms / 1000) * samplerate frames. Connecting or recording with Opus
// at a rate other than 48 kHz makes snd_callback convert frameCount * channels
// floats into that smaller buffer.
//
// These tests fail while that overflow is present (AddressSanitizer aborts
// in src_short_to_float_array or src_process) and pass once the buffers are
// sized from the device frame count, including after the buffer time changes.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <mutex>
#include <signal.h>
#include <sys/mman.h>
#include <unordered_map>
#include <unistd.h>

#include <samplerate.h>

#include "butt.h"
#include "cfg.h"
#include "port_audio.h"
#include "ringbuffer.h"

extern int pa_frames;
extern struct ringbuf stream_rb;
extern struct ringbuf rec_rb;

bool record = false;
bool recording = false;
bool connected = false;
bool streaming = false;
bool disconnect = false;
bool try_connect = false;
bool song_timeout_running = false;
bool app_timeout_running = false;

int stream_socket = -1;
double kbytes_sent = 0;
double kbytes_written = 0;
unsigned int record_start_hour = 0;

lame_enc lame_stream;
lame_enc lame_rec;
vorbis_enc vorbis_stream;
vorbis_enc vorbis_rec;
opus_enc opus_stream;
opus_enc opus_rec;
flac_enc flac_rec;
aac_enc aac_stream;
aac_enc aac_rec;

static std::atomic<int> g_converted_samples{0};

// libsamplerate is not built with AddressSanitizer, so a write past the
// resampler buffer is not visible to an instrumented caller. Place each
// buffer this program allocates so the following page cannot be touched.
struct GuardBlock {
    void *base;
    size_t map_len;
    size_t user_size;
};

static std::mutex g_alloc_mu;
static std::unordered_map<void *, GuardBlock> g_blocks;

extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);
extern "C" void *__real_realloc(void *, size_t);
extern "C" void *__real_calloc(size_t, size_t);

static void *guard_alloc(size_t n)
{
    if (n == 0)
        n = 1;
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t guard_off = (sizeof(GuardBlock) + n + page - 1) & ~(page - 1);
    size_t map_len = guard_off + page;
    void *base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return NULL;

    uintptr_t guard = (uintptr_t)base + guard_off;
    uintptr_t user = (guard - n) & ~(uintptr_t)15;
    if (user < (uintptr_t)base + sizeof(GuardBlock))
    {
        munmap(base, map_len);
        guard_off += page;
        map_len = guard_off + page;
        base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED)
            return NULL;
        guard = (uintptr_t)base + guard_off;
        user = (guard - n) & ~(uintptr_t)15;
    }
    if (mprotect((void *)guard, page, PROT_NONE) != 0)
    {
        munmap(base, map_len);
        return NULL;
    }

    GuardBlock block;
    block.base = base;
    block.map_len = map_len;
    block.user_size = n;
    std::lock_guard<std::mutex> lock(g_alloc_mu);
    g_blocks[(void *)user] = block;
    return (void *)user;
}

static void guard_free(void *p)
{
    if (p == NULL)
        return;
    GuardBlock block;
    {
        std::lock_guard<std::mutex> lock(g_alloc_mu);
        std::unordered_map<void *, GuardBlock>::iterator it = g_blocks.find(p);
        if (it == g_blocks.end())
        {
            __real_free(p);
            return;
        }
        block = it->second;
        g_blocks.erase(it);
    }
    munmap(block.base, block.map_len);
}

extern "C" void *__wrap_malloc(size_t n)
{
    return guard_alloc(n);
}

extern "C" void __wrap_free(void *p)
{
    guard_free(p);
}

extern "C" void *__wrap_realloc(void *p, size_t n)
{
    if (p == NULL)
        return guard_alloc(n);
    size_t old_size = 0;
    {
        std::lock_guard<std::mutex> lock(g_alloc_mu);
        std::unordered_map<void *, GuardBlock>::iterator it = g_blocks.find(p);
        if (it == g_blocks.end())
            return __real_realloc(p, n);
        old_size = it->second.user_size;
    }
    void *np = guard_alloc(n);
    if (np == NULL)
        return NULL;
    std::memcpy(np, p, old_size < n ? old_size : n);
    guard_free(p);
    return np;
}

extern "C" void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (nmemb != 0 && size > (size_t)-1 / nmemb)
        return NULL;
    size_t bytes = nmemb * size;
    void *p = guard_alloc(bytes);
    if (p != NULL)
        std::memset(p, 0, bytes);
    return p;
}

static void segv_handler(int)
{
    void *frames[32];
    int n = backtrace(frames, 32);
    const char msg[] = "guard page fault in resampler buffer\n";
    if (write(2, msg, sizeof(msg) - 1))
        ;
    backtrace_symbols_fd(frames, n, 2);
    _exit(139);
}

// Observe the length handed to the real converter, then perform the real write.
extern "C" void __real_src_short_to_float_array(const short *in, float *out, int len);
extern "C" void __wrap_src_short_to_float_array(const short *in, float *out, int len)
{
    int prev = g_converted_samples.load();
    while (len > prev && !g_converted_samples.compare_exchange_weak(prev, len))
        ;
    __real_src_short_to_float_array(in, out, len);
}

// The encode/send thread runs after the resampler write. These stand-ins do
// not send anywhere; the overflow is decided before they are called.
static void unused_encoder_reached(const char *)
{
}

void print_info(const char *info, int)
{
    if (info != NULL)
        std::fprintf(stderr, "info: %s\n", info);
}

void update_samplerates(void) {}
void vu_meter(short, short) {}

int lame_enc_encode(lame_enc *, short *, char *, int, int) { unused_encoder_reached("lame"); return 0; }
int vorbis_enc_encode(vorbis_enc *, short *, char *, int) { unused_encoder_reached("vorbis"); return 0; }
int vorbis_enc_reinit(vorbis_enc *) { unused_encoder_reached("vorbis reinit"); return 0; }
void vorbis_enc_write_header(vorbis_enc *) { unused_encoder_reached("vorbis header"); }
int opus_enc_encode(opus_enc *, short *, char *, int) { unused_encoder_reached("opus"); return 0; }
int opus_enc_reinit(opus_enc *) { unused_encoder_reached("opus reinit"); return 0; }
void opus_enc_write_header(opus_enc *) { unused_encoder_reached("opus header"); }
int aac_enc_encode(aac_enc *, short *, char *, int, int) { unused_encoder_reached("aac"); return 0; }
int flac_enc_encode(flac_enc *, short *, int, int) { unused_encoder_reached("flac"); return 0; }
int flac_enc_reinit(flac_enc *) { unused_encoder_reached("flac reinit"); return 0; }
int flac_enc_init_FILE(flac_enc *, FILE *) { unused_encoder_reached("flac init"); return 0; }
void flac_enc_close(flac_enc *) { unused_encoder_reached("flac close"); }
FLAC__uint64 flac_enc_get_bytes_written(void) { unused_encoder_reached("flac bytes"); return 0; }
int wav_write_header(FILE *, short, int, short) { unused_encoder_reached("wav"); return 0; }
int sc_send(char *, int) { unused_encoder_reached("shoutcast"); return 0; }
int ic_send(char *, int) { unused_encoder_reached("icecast"); return 0; }

static int fail(const char *message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static int expected_frames(int buffer_ms, int samplerate)
{
    return (int)((buffer_ms / 1000.0) * samplerate);
}

static bool write_config(const char *path, int buffer_ms, int samplerate,
                         const char *stream_codec, const char *rec_codec)
{
    FILE *fp = std::fopen(path, "w");
    if (fp == NULL)
        return false;
    // num_of_srv is 0 so the parser never reads a server password.
    // The stream thread reads the server type before it waits. connected stays
    // false, so it never sends and the password placeholder is not used.
    std::fprintf(fp,
        "[main]\n"
        "server = station\n"
        "num_of_srv = 1\n"
        "srv_ent = station\n"
        "num_of_icy = 0\n"
        "song_update = 0\n"
        "app_update = 0\n"
        "app_update_service = 0\n"
        "connect_at_startup = 0\n"
        "gain = 1.0\n"
        "\n"
        "[station]\n"
        "address = 127.0.0.1\n"
        "port = 8000\n"
        "password = placeholder\n"
        "type = 1\n"
        "mount = /live\n"
        "usr = source\n"
        "\n"
        "[audio]\n"
        "device = 0\n"
        "samplerate = %d\n"
        "bitrate = 128\n"
        "channel = 2\n"
        "codec = %s\n"
        "resample_mode = 4\n"
        "buffer_ms = %d\n"
        "\n"
        "[record]\n"
        "bitrate = 128\n"
        "codec = %s\n"
        "start_rec = 0\n"
        "sync_to_hour = 0\n"
        "split_time = 0\n"
        "filename = rec.opus\n"
        "folder = /tmp\n"
        "\n"
        "[dsp]\n"
        "compressor = 0\n"
        "equalizer = 0\n"
        "gain1 = 0\n"
        "gain2 = 0\n"
        "gain3 = 0\n"
        "gain4 = 0\n"
        "gain5 = 0\n"
        "\n"
        "[gui]\n"
        "attach = 0\n"
        "ontop = 0\n"
        "lcd_auto = 0\n",
        samplerate, stream_codec, buffer_ms, rec_codec);
    std::fclose(fp);
    return true;
}

// Same order as main() in src/butt.cpp: allocate, then read the config, then open.
static int boot(const char *config_path)
{
    if (snd_init() != 0)
        return fail("snd_init failed");
    if (cfg_set_values(const_cast<char *>(config_path)) != 0)
        return fail("cfg_set_values failed");
    if (cfg.audio.dev_count <= 0)
        return fail("no capture device");
    if (snd_open_stream() != 0)
        return fail("snd_open_stream failed");
    return 0;
}

static int wait_for_conversion(struct ringbuf *rb, int samples, int frames)
{
    // One resampled period is about frames * 48000/44100 frames, stereo, 16-bit.
    // Require most of that so a callback that dropped the device period does not pass.
    const int min_bytes = (int)(frames * (48000.0 / cfg.audio.samplerate) * cfg.audio.channel * sizeof(short) * 0.85);
    for (int i = 0; i < 200; i++)
    {
        if (g_converted_samples.load() >= samples && rb_filled(rb) >= min_bytes)
        {
            if (g_converted_samples.load() != samples)
                return fail("converter length was not frameCount * channels");
            return 0;
        }
        usleep(50 * 1000);
    }
    std::fprintf(stderr, "converted=%d filled=%d pa_frames=%d expected_samples=%d min_bytes=%d\n",
                 g_converted_samples.load(), rb_filled(rb), pa_frames, samples, min_bytes);
    return fail("timed out waiting for a full resampler period");
}

static int run_mode(const char *mode)
{
    const bool record_mode = std::strstr(mode, "record") != NULL;
    const bool reinit_mode = std::strncmp(mode, "reinit-", 7) == 0;
    const bool large_mode = std::strncmp(mode, "large-", 6) == 0;

    const int samplerate = 44100;
    const int open_buffer_ms = reinit_mode ? 20 : (large_mode ? 1000 : 50);
    const char *stream_codec = record_mode ? "mp3" : "opus";
    const char *rec_codec = record_mode ? "opus" : "mp3";

    char config_path[] = "/tmp/butt-opus-resampler-XXXXXX";
    int fd = mkstemp(config_path);
    if (fd < 0)
        return fail("mkstemp");
    close(fd);
    if (!write_config(config_path, open_buffer_ms, samplerate, stream_codec, rec_codec))
        return fail("could not write config");

    int rc = boot(config_path);
    std::remove(config_path);
    if (rc != 0)
        return rc;

    const int opened = expected_frames(open_buffer_ms, samplerate);
    if (pa_frames != opened)
    {
        std::fprintf(stderr, "pa_frames=%d expected=%d\n", pa_frames, opened);
        return fail("device was not opened at the configured frame count");
    }

    int active_buffer_ms = open_buffer_ms;
    if (reinit_mode)
    {
        // input_cfg_buffer_cb stores the new buffer time and calls snd_reinit once.
        active_buffer_ms = 200;
        cfg.audio.buffer_ms = active_buffer_ms;
        snd_reinit();
        const int grown = expected_frames(active_buffer_ms, samplerate);
        if (pa_frames != grown)
        {
            std::fprintf(stderr, "pa_frames=%d expected=%d\n", pa_frames, grown);
            return fail("snd_reinit did not open the new buffer time");
        }
    }

    const int samples = pa_frames * cfg.audio.channel;
    cfg.rec.path = const_cast<char *>("/tmp/butt-opus-resampler-unused.opus");

    // connected stays false, so the stream thread never sends.
    // Recording follows button_record_cb: open the file, set record, then start.
    FILE *rec_file = NULL;
    if (record_mode)
    {
        rec_file = std::fopen(cfg.rec.path, "wb");
        if (rec_file == NULL)
            return fail("could not open record file");
        cfg.rec.fd = rec_file;
        record = true;
        snd_start_rec();
    }
    else
        snd_start_stream();

    struct ringbuf *rb = record_mode ? &rec_rb : &stream_rb;
    rc = wait_for_conversion(rb, samples, pa_frames);
    if (rec_file != NULL)
        std::remove(cfg.rec.path);
    // Leave the device running. Stopping it races the callback and the
    // record thread; the process exit releases both.
    return rc;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = segv_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s startup-stream|startup-record|reinit-stream|reinit-record|large-stream|large-record\n", argv[0]);
        return 2;
    }
    return run_mode(argv[1]);
}
