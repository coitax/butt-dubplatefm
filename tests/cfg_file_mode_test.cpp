/* Spec for configuration-file permissions.
 *
 * cfg_create_default and cfg_write_file store the stream source password.
 * These tests require the file to be created mode 0600, an existing looser
 * file to be mode 0600 before the password bytes are written, and another
 * local account to be unable to open the file. They fail while the writers
 * use a plain fl_fopen("wb") and leave the process umask in charge.
 *
 * The directory is mode 0755 so another account can traverse it. That is the
 * situation for a -c path or an export outside a private home directory.
 * A world-readable sentinel in the same directory must be readable by that
 * account; otherwise a failed read of the config would not prove the mode.
 */

#include "cfg.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

void print_info(const char *info, int info_type)
{
    (void)info;
    (void)info_type;
}

static const char kPlaceholder[] = "placeholder-source-secret";
static const char kPlaceholderBackup[] = "placeholder-source-secret-backup";
static const char kPasswordKey[] = "password = ";

static int g_failed = 0;

static void fail(const char *name, const char *why)
{
    fprintf(stderr, "FAIL %s: %s\n", name, why);
    g_failed++;
}

static void pass(const char *name)
{
    fprintf(stderr, "PASS %s\n", name);
}

static mode_t file_mode(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return st.st_mode & 0777;
}

static bool peek_bytes(pid_t pid, unsigned long addr, void *out, size_t n)
{
    unsigned char *dst = (unsigned char *)out;
    size_t off = 0;
    while (off < n) {
        errno = 0;
        unsigned long word = ptrace(PTRACE_PEEKDATA, pid, addr + off, NULL);
        if (errno != 0)
            return false;
        size_t chunk = sizeof(word);
        if (chunk > n - off)
            chunk = n - off;
        memcpy(dst + off, &word, chunk);
        off += chunk;
    }
    return true;
}

static bool read_cstr(pid_t pid, unsigned long addr, char *out, size_t cap)
{
    size_t i = 0;
    while (i + 1 < cap) {
        unsigned char buf[sizeof(long)];
        if (!peek_bytes(pid, addr + i, buf, sizeof(buf)))
            return false;
        for (size_t b = 0; b < sizeof(buf) && i + 1 < cap; b++) {
            out[i++] = (char)buf[b];
            if (buf[b] == 0) {
                out[i - 1] = 0;
                return true;
            }
        }
    }
    out[cap - 1] = 0;
    return false;
}

static bool buffer_has_password_key(const char *buf, size_t n)
{
    const size_t key_len = sizeof(kPasswordKey) - 1;
    if (n < key_len)
        return false;
    for (size_t i = 0; i + key_len <= n; i++) {
        if (memcmp(buf + i, kPasswordKey, key_len) == 0)
            return true;
    }
    return false;
}

struct trace_result {
    int cfg_status;
    bool saw_password_write;
    bool password_write_private;
    bool saw_create_open;
    bool create_open_private;
    std::string error;
};

static bool write_contains_password(pid_t pid, unsigned long addr, size_t n)
{
    if (n == 0 || n > 1024 * 1024)
        return false;
    std::string buf(n, '\0');
    if (!peek_bytes(pid, addr, &buf[0], n))
        return false;
    return buffer_has_password_key(buf.data(), buf.size());
}

static bool writev_contains_password(pid_t pid, unsigned long iov_addr, unsigned long count)
{
    if (count > 64)
        count = 64;
    for (unsigned long i = 0; i < count; i++) {
        unsigned long base = 0;
        unsigned long len = 0;
        if (!peek_bytes(pid, iov_addr + i * (sizeof(unsigned long) * 2), &base, sizeof(base)))
            return false;
        if (!peek_bytes(pid, iov_addr + i * (sizeof(unsigned long) * 2) + sizeof(unsigned long),
                        &len, sizeof(len)))
            return false;
        if (write_contains_password(pid, base, (size_t)len))
            return true;
    }
    return false;
}

static trace_result trace_worker(char *argv0, const char *op, const char *path, const char *umask_octal,
                                  bool expect_new_file)
{
    trace_result result;
    result.cfg_status = -1;
    result.saw_password_write = false;
    result.password_write_private = true;
    result.saw_create_open = false;
    result.create_open_private = true;

    pid_t pid = fork();
    if (pid < 0) {
        result.error = "fork failed";
        return result;
    }
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(120);
        raise(SIGSTOP);
        execl(argv0, argv0, "--worker", op, path, umask_octal, (char *)NULL);
        _exit(121);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        result.error = "waitpid failed";
        return result;
    }
    if (ptrace(PTRACE_SETOPTIONS, pid, NULL, PTRACE_O_TRACESYSGOOD) != 0) {
        result.error = "PTRACE_SETOPTIONS failed";
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return result;
    }

    int config_fd = -1;
    bool entry = true;
    bool after_exec = false;
    unsigned long entry_syscall = 0;
    unsigned long entry_arg1 = 0;
    unsigned long entry_arg2 = 0;
    bool entry_path_match = false;
    unsigned long deliver = 0;

    while (true) {
        if (ptrace(PTRACE_SYSCALL, pid, NULL, (void *)deliver) != 0) {
            result.error = "PTRACE_SYSCALL failed";
            break;
        }
        deliver = 0;
        if (waitpid(pid, &status, 0) < 0) {
            result.error = "waitpid failed";
            break;
        }
        if (WIFEXITED(status)) {
            result.cfg_status = WEXITSTATUS(status);
            break;
        }
        if (WIFSIGNALED(status)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "worker killed by signal %d", WTERMSIG(status));
            result.error = buf;
            break;
        }
        if (!WIFSTOPPED(status))
            continue;
        int sig = WSTOPSIG(status);
        if (sig != (SIGTRAP | 0x80)) {
            /* A post-exec SIGTRAP is not a syscall stop. Delivering it kills the worker. */
            if (sig == SIGTRAP || sig == SIGSTOP)
                deliver = 0;
            else
                deliver = (unsigned long)sig;
            continue;
        }

        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
            result.error = "PTRACE_GETREGS failed";
            break;
        }

        if (entry) {
            entry_syscall = regs.orig_rax;
            entry_arg1 = regs.rsi;
            entry_arg2 = regs.rdx;
            entry_path_match = false;
            if (after_exec && entry_syscall == SYS_openat) {
                char opened[PATH_MAX];
                if (read_cstr(pid, regs.rsi, opened, sizeof(opened)) && strcmp(opened, path) == 0)
                    entry_path_match = true;
            } else if (after_exec && entry_syscall == SYS_open) {
                char opened[PATH_MAX];
                if (read_cstr(pid, regs.rdi, opened, sizeof(opened)) && strcmp(opened, path) == 0)
                    entry_path_match = true;
            } else if (after_exec && config_fd >= 0 &&
                       (entry_syscall == SYS_write || entry_syscall == SYS_pwrite64) &&
                       (int)regs.rdi == config_fd) {
                if (write_contains_password(pid, regs.rsi, (size_t)regs.rdx)) {
                    result.saw_password_write = true;
                    if (file_mode(path) != 0600)
                        result.password_write_private = false;
                }
            } else if (after_exec && config_fd >= 0 &&
                       (entry_syscall == SYS_writev || entry_syscall == SYS_pwritev) &&
                       (int)regs.rdi == config_fd) {
                if (writev_contains_password(pid, regs.rsi, regs.rdx)) {
                    result.saw_password_write = true;
                    if (file_mode(path) != 0600)
                        result.password_write_private = false;
                }
            }
            entry = false;
        } else {
            if (entry_syscall == SYS_execve && (long)regs.rax >= 0)
                after_exec = true;
            if (after_exec && entry_path_match &&
                (entry_syscall == SYS_openat || entry_syscall == SYS_open) &&
                (long)regs.rax >= 0) {
                config_fd = (int)regs.rax;
                unsigned long flags = (entry_syscall == SYS_openat) ? entry_arg2 : entry_arg1;
                if (expect_new_file && (flags & O_CREAT)) {
                    result.saw_create_open = true;
                    if (file_mode(path) != 0600)
                        result.create_open_private = false;
                }
            }
            entry = true;
        }
    }

    if (result.cfg_status < 0 && result.error.empty())
        result.error = "worker did not exit";
    if (pid > 0 && !WIFEXITED(status)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    return result;
}

static int other_account_can_read(const char *path)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        execlp("sudo", "sudo", "-n", "-u", "nobody", "--",
               "/usr/bin/test", "-r", path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) == 127)
        return -1;
    return WEXITSTATUS(status) == 0 ? 1 : 0;
}

static bool password_value_present(const char *path, const char *required)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, kPasswordKey, sizeof(kPasswordKey) - 1) != 0)
            continue;
        char *value = line + (sizeof(kPasswordKey) - 1);
        size_t len = strlen(value);
        while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r'))
            value[--len] = 0;
        if (len == 0)
            continue;
        if (required != NULL && strcmp(value, required) != 0)
            continue;
        found = true;
    }
    fclose(fp);
    return found;
}

static void fill_cfg_with_placeholder(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.main.num_of_srv = 2;
    cfg.main.num_of_icy = 1;
    cfg.main.srv = (char *)"station";
    cfg.main.srv_ent = (char *)"station;backup";
    cfg.main.icy = (char *)"stream";
    cfg.main.icy_ent = (char *)"stream";
    cfg.main.gain = 1.0f;
    cfg.audio.codec = (char *)"mp3";
    cfg.rec.codec = (char *)"mp3";
    cfg.rec.filename = (char *)"rec.mp3";
    cfg.rec.folder = (char *)"/tmp";

    static server_t ice;
    static server_t shout;
    static server_t *servers[2];
    memset(&ice, 0, sizeof(ice));
    memset(&shout, 0, sizeof(shout));
    ice.name = (char *)"station";
    ice.addr = (char *)"127.0.0.1";
    ice.pwd = (char *)kPlaceholder;
    ice.mount = (char *)"live";
    ice.usr = (char *)"source";
    ice.port = 8000;
    ice.type = ICECAST;
    shout.name = (char *)"backup";
    shout.addr = (char *)"127.0.0.1";
    shout.pwd = (char *)kPlaceholderBackup;
    shout.port = 8001;
    shout.type = SHOUTCAST;
    servers[0] = &ice;
    servers[1] = &shout;
    cfg.srv = servers;

    static icy_t icy;
    static icy_t *icys[1];
    memset(&icy, 0, sizeof(icy));
    icy.name = (char *)"stream";
    icy.pub = (char *)"0";
    icys[0] = &icy;
    cfg.icy = icys;
}

static int run_worker_main(int argc, char **argv)
{
    if (argc != 5)
        return 122;
    const char *op = argv[2];
    const char *path = argv[3];
    unsigned int mask = 0;
    if (sscanf(argv[4], "%o", &mask) != 1)
        return 123;
    umask((mode_t)mask);
    cfg_path = strdup(path);
    if (strcmp(op, "create-default") == 0)
        return cfg_create_default();
    fill_cfg_with_placeholder();
    if (strcmp(op, "write-null") == 0)
        return cfg_write_file(NULL);
    if (strcmp(op, "write-path") == 0)
        return cfg_write_file(cfg_path);
    return 124;
}

static std::string make_dir(void)
{
    char tmpl[] = "/tmp/butt-cfg-mode-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (dir == NULL) {
        perror("mkdtemp");
        exit(2);
    }
    if (chmod(dir, 0755) != 0) {
        perror("chmod");
        exit(2);
    }
    return std::string(dir);
}

static void write_sentinel(const std::string &dir)
{
    std::string path = dir + "/sentinel";
    mode_t old = umask(0);
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    umask(old);
    if (fd < 0) {
        perror("sentinel");
        exit(2);
    }
    if (write(fd, "ok\n", 3) != 3) {
        perror("sentinel write");
        exit(2);
    }
    close(fd);
}

static void expect_other_can_read_sentinel(const char *name, const std::string &dir)
{
    std::string path = dir + "/sentinel";
    int can = other_account_can_read(path.c_str());
    if (can != 1)
        fail(name, "precondition failed: other account cannot read a mode 0644 file in this directory");
}

static bool check_case(const char *name, char *argv0, const char *op, const std::string &dir,
                       const char *leaf, const char *umask_octal, bool expect_new_file,
                       bool precreate_loose, const char *required_password)
{
    int before = g_failed;
    std::string path = dir + "/" + leaf;
    if (precreate_loose) {
        mode_t old = umask(0);
        int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
        umask(old);
        if (fd < 0) {
            fail(name, "could not precreate loose file");
            return false;
        }
        close(fd);
        if (file_mode(path.c_str()) != 0666) {
            fail(name, "precreated file was not mode 0666");
            return false;
        }
    }

    trace_result tr = trace_worker(argv0, op, path.c_str(), umask_octal, expect_new_file);
    if (!tr.error.empty()) {
        fail(name, tr.error.c_str());
        return false;
    }
    if (tr.cfg_status != 0) {
        fail(name, "configuration writer returned an error");
        return false;
    }
    if (!tr.saw_password_write) {
        fail(name, "did not observe the password entry being written");
        return false;
    }
    if (!tr.password_write_private)
        fail(name, "password entry was written while the file mode was not 0600");
    if (expect_new_file) {
        if (!tr.saw_create_open)
            fail(name, "did not observe the creating open of a new config file");
        else if (!tr.create_open_private)
            fail(name, "new config file was not created as mode 0600");
    }
    if (file_mode(path.c_str()) != 0600)
        fail(name, "config file mode after the writer returned was not 0600");
    if (!password_value_present(path.c_str(), required_password))
        fail(name, "config file is missing the source password entry");
    if (required_password != NULL && !password_value_present(path.c_str(), kPlaceholderBackup))
        fail(name, "config file is missing the backup server source password entry");

    expect_other_can_read_sentinel(name, dir);
    int can = other_account_can_read(path.c_str());
    if (can < 0)
        fail(name, "could not drop to another local account");
    else if (can == 1)
        fail(name, "another local account can read the config file");
    return g_failed == before;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--worker") == 0)
        return run_worker_main(argc, argv);

    std::string dir = make_dir();
    write_sentinel(dir);
    fprintf(stderr, "workdir %s\n", dir.c_str());

    if (check_case("create_default_umask_022", argv[0], "create-default", dir,
                   "default-022", "022", true, false, NULL))
        pass("create_default_umask_022");

    if (check_case("create_default_umask_000", argv[0], "create-default", dir,
                   "default-000", "000", true, false, NULL))
        pass("create_default_umask_000");

    if (check_case("write_file_export_path_umask_022", argv[0], "write-path", dir,
                   "export-022", "022", true, false, kPlaceholder))
        pass("write_file_export_path_umask_022");

    if (check_case("write_file_null_path_umask_022", argv[0], "write-null", dir,
                   "save-022", "022", true, false, kPlaceholder))
        pass("write_file_null_path_umask_022");

    if (check_case("write_file_existing_0666_tightened_before_password", argv[0], "write-path", dir,
                   "existing-0666", "022", false, true, kPlaceholder))
        pass("write_file_existing_0666_tightened_before_password");

    if (g_failed) {
        fprintf(stderr, "cfg file mode tests failed\n");
        return 1;
    }
    printf("cfg file mode tests passed\n");
    return 0;
}
