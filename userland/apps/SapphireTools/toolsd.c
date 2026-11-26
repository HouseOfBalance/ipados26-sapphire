/*
 * toolsd: root helper daemon for SapphireTools.app (launched by launchd as root).
 * Listens on /var/run/sapphire-toolsd.sock (0666; peers restricted to uid 0 or 501)
 * and executes allowlisted privileged operations on behalf of the GUI app:
 *   SYSCTL_SET <name> <value>   (only kern.sweet_* and kern.hakios.*)
 *   WRITE_METAL_ARGS [content]  (writes /var/tmp/metal.args safely & atomically)
 *   KILL <proc1> [proc2 ...]    (only allowlisted daemons, exact p_comm match)
 *   SSH_START / SSH_STOP
 *   REBOOT
 *   DMESG                       (returns sweetlog / dmesg tail)
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
extern int reboot(int howto);
#ifndef RB_AUTOBOOT
#define RB_AUTOBOOT 0
#endif
#ifndef SZOMB
#define SZOMB 5
#endif

#define SOCK_PATH       "/var/run/sapphire-toolsd.sock"
#define METAL_ARGS_PATH "/var/tmp/metal.args"
#define SWEETLOG_PATH   "/var/log/sweetlog.txt"
#define DMESG_MAX_BYTES (64 * 1024)

static void write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (w == 0) break;
        p += (size_t)w;
        len -= (size_t)w;
    }
}

static int allowed_sysctl(const char *name) {
    return strncmp(name, "kern.sweet_", 11) == 0 ||
           strncmp(name, "kern.hakios.", 12) == 0;
}

static int allowed_proc(const char *name) {
    static const char *const kAllowed[] = {
        "backboardd", "SpringBoard", "fpd", "faced", "btmonitor",
        "vibd", "thermald", "sensord", "mediaserverd",
        "CommCenter", "wifid", "bluetoothd", "camd",
        "sshd", "dropbear", "sharingd", NULL
    };
    for (int i = 0; kAllowed[i]; i++) {
        if (strcmp(name, kAllowed[i]) == 0) return 1;
    }
    return 0;
}

static void kill_by_name(const char *target) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t sz = 0;
    if (sysctl(mib, 4, NULL, &sz, NULL, 0) != 0 || sz == 0) return;
    sz += 16 * sizeof(struct kinfo_proc);
    struct kinfo_proc *procs = malloc(sz);
    if (!procs) return;
    if (sysctl(mib, 4, procs, &sz, NULL, 0) == 0) {
        size_t n = sz / sizeof(struct kinfo_proc);
        pid_t self = getpid();
        for (size_t i = 0; i < n; i++) {
            if (procs[i].kp_proc.p_stat == SZOMB) continue;
            if (strcmp(procs[i].kp_proc.p_comm, target) == 0) {
                pid_t pid = procs[i].kp_proc.p_pid;
                if (pid > 1 && pid != self) {
                    kill(pid, SIGKILL);
                }
            }
        }
    }
    free(procs);
}

static int read_sweetlog_tail(char **out_buf, size_t *out_len) {
    int fd = open(SWEETLOG_PATH, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        return -1;
    }
    off_t start = 0;
    bool cut = false;
    if (st.st_size > (off_t)DMESG_MAX_BYTES) {
        start = st.st_size - (off_t)DMESG_MAX_BYTES;
        cut = true;
    }
    size_t want = (size_t)(st.st_size - start);
    char *buf = malloc(want + 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    ssize_t r = pread(fd, buf, want, start);
    close(fd);
    if (r <= 0) {
        free(buf);
        return -1;
    }
    buf[r] = '\0';
    char *p = buf;
    size_t len = (size_t)r;
    if (cut) {
        char *nl = memchr(buf, '\n', len);
        if (nl && (size_t)(nl - buf + 1) < len) {
            size_t skip = (size_t)(nl - buf + 1);
            p += skip;
            len -= skip;
            memmove(buf, p, len + 1);
        }
    }
    *out_buf = buf;
    *out_len = len;
    return 0;
}

static int run_dmesg_capture(char **out_buf, size_t *out_len) {
    static const char *const kBins[] = {
        "/sbin/dmesg", "/usr/sbin/dmesg", "/usr/bin/dmesg",
        "/var/jb/usr/sbin/dmesg", "/var/jb/usr/bin/dmesg", NULL
    };
    for (int i = 0; kBins[i]; i++) {
        if (access(kBins[i], X_OK) != 0) continue;
        int pfd[2];
        if (pipe(pfd) != 0) continue;
        fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
        fcntl(pfd[1], F_SETFD, FD_CLOEXEC);

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, pfd[1], STDERR_FILENO);

        char *argv[] = { (char *)kBins[i], NULL };
        pid_t pid = 0;
        int rc = posix_spawn(&pid, kBins[i], &fa, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        close(pfd[1]);
        if (rc != 0) {
            close(pfd[0]);
            continue;
        }

        char *buf = malloc(DMESG_MAX_BYTES + 1);
        size_t used = 0;
        struct timespec ts0;
        clock_gettime(CLOCK_MONOTONIC, &ts0);
        int64_t deadline_ms = (int64_t)ts0.tv_sec * 1000 + ts0.tv_nsec / 1000000 + 3000;

        while (buf != NULL) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t left = deadline_ms - ((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
            if (left <= 0) break;
            struct pollfd pf = { .fd = pfd[0], .events = POLLIN };
            int pr = poll(&pf, 1, (int)left);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) break;
            char chunk[4096];
            ssize_t n = read(pfd[0], chunk, sizeof(chunk));
            if (n <= 0) break;
            if ((size_t)n >= DMESG_MAX_BYTES) {
                memcpy(buf, chunk + ((size_t)n - DMESG_MAX_BYTES), DMESG_MAX_BYTES);
                used = DMESG_MAX_BYTES;
            } else {
                if (used + (size_t)n > DMESG_MAX_BYTES) {
                    size_t drop = (used + (size_t)n) - DMESG_MAX_BYTES;
                    memmove(buf, buf + drop, used - drop);
                    used -= drop;
                }
                memcpy(buf + used, chunk, (size_t)n);
                used += (size_t)n;
            }
        }
        close(pfd[0]);

        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == 0) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        }
        if (buf && used > 0) {
            buf[used] = '\0';
            *out_buf = buf;
            *out_len = used;
            return 0;
        }
        free(buf);
    }
    return -1;
}

static void handle_dmesg(int fd) {
    char *log_buf = NULL;
    size_t log_len = 0;
    if (read_sweetlog_tail(&log_buf, &log_len) == 0 ||
        run_dmesg_capture(&log_buf, &log_len) == 0) {
        write_all(fd, "OK\n", 3);
        if (log_len > 0) {
            write_all(fd, log_buf, log_len);
            if (log_buf[log_len - 1] != '\n') {
                write_all(fd, "\n", 1);
            }
        }
        free(log_buf);
    } else {
        write_all(fd, "ERR ENOENT\n", 11);
    }
}

static void handle_client(int fd) {
    uid_t euid = (uid_t)-1;
    gid_t egid = (gid_t)-1;
    if (getpeereid(fd, &euid, &egid) != 0 || (euid != 0 && euid != 501)) {
        write_all(fd, "ERR EPERM\n", 10);
        return;
    }

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';

    char resp[512];
    if (strcmp(buf, "PING") == 0 || strncmp(buf, "PING ", 5) == 0) {
        snprintf(resp, sizeof(resp), "OK uid=%d euid=%d\n", (int)getuid(), (int)geteuid());
        write_all(fd, resp, strlen(resp));
    } else if (strncmp(buf, "SYSCTL_SET ", 11) == 0) {
        char *sp = strchr(buf + 11, ' ');
        if (!sp) {
            write_all(fd, "ERR EINVAL\n", 11);
            return;
        }
        *sp = '\0';
        const char *name = buf + 11;
        const char *val = sp + 1;
        if (!allowed_sysctl(name)) {
            write_all(fd, "ERR EPERM\n", 10);
            return;
        }
        char *end = NULL;
        errno = 0;
        long long iv = strtoll(val, &end, 0);
        bool is_num = (*val != '\0' && end != NULL && *end == '\0' && errno == 0);
        int iv32 = (int)iv;
        bool ok = false;
        if (is_num) {
            ok = (sysctlbyname(name, NULL, NULL, &iv32, sizeof(iv32)) == 0 ||
                  sysctlbyname(name, NULL, NULL, &iv, sizeof(iv)) == 0);
        }
        if (!ok) {
            ok = (sysctlbyname(name, NULL, NULL, (void *)val, strlen(val) + 1) == 0 ||
                  sysctlbyname(name, NULL, NULL, (void *)val, strlen(val)) == 0);
        }
        if (ok) {
            write_all(fd, "OK\n", 3);
        } else {
            snprintf(resp, sizeof(resp), "ERR %d %s\n", errno, strerror(errno));
            write_all(fd, resp, strlen(resp));
        }
    } else if (strcmp(buf, "WRITE_METAL_ARGS") == 0 ||
               strncmp(buf, "WRITE_METAL_ARGS\n", 17) == 0 ||
               strncmp(buf, "WRITE_METAL_ARGS ", 17) == 0) {
        const char *body = (buf[16] == '\0') ? "" : (buf + 17);
        while (*body == '\n' || *body == '\r') body++;
        const char *tmp_path = METAL_ARGS_PATH ".tmp";
        unlink(tmp_path);
        int tfd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
        if (tfd < 0) {
            snprintf(resp, sizeof(resp), "ERR %d %s\n", errno, strerror(errno));
            write_all(fd, resp, strlen(resp));
            return;
        }
        size_t blen = strlen(body);
        bool write_ok = true;
        if (blen > 0) {
            if (write(tfd, body, blen) != (ssize_t)blen) write_ok = false;
            else if (body[blen - 1] != '\n' && write(tfd, "\n", 1) != 1) write_ok = false;
        }
        (void)fchmod(tfd, 0644);
        (void)fsync(tfd);
        close(tfd);
        if (!write_ok) {
            unlink(tmp_path);
            write_all(fd, "ERR EIO\n", 8);
            return;
        }
        struct stat dst_st;
        if (lstat(METAL_ARGS_PATH, &dst_st) == 0 && S_ISLNK(dst_st.st_mode)) {
            unlink(METAL_ARGS_PATH);
        }
        if (rename(tmp_path, METAL_ARGS_PATH) == 0) {
            write_all(fd, "OK\n", 3);
        } else {
            int e = errno;
            unlink(tmp_path);
            snprintf(resp, sizeof(resp), "ERR %d %s\n", e, strerror(e));
            write_all(fd, resp, strlen(resp));
        }
    } else if (strncmp(buf, "KILL ", 5) == 0) {
        char *p = buf + 5;
        char *tok;
        while ((tok = strsep(&p, " ")) != NULL) {
            if (*tok == '\0') continue;
            if (!allowed_proc(tok)) continue;
            kill_by_name(tok);
        }
        write_all(fd, "OK\n", 3);
    } else if (strcmp(buf, "SSH_START") == 0) {
        const char *bins[] = { "/var/jb/usr/sbin/sshd", "/usr/sbin/sshd", "/usr/local/sbin/sshd", NULL };
        int found = 0, started = 0;
        for (int i = 0; bins[i]; i++) {
            if (access(bins[i], X_OK) != 0) continue;
            found = 1;
            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init(&fa);
            posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
            posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
            posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
            pid_t pid = 0;
            char *argv[] = { (char *)bins[i], NULL };
            int rc = posix_spawn(&pid, bins[i], &fa, NULL, argv, environ);
            posix_spawn_file_actions_destroy(&fa);
            if (rc == 0) {
                int st = 0;
                while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
                if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                    started = 1;
                    break;
                }
            }
        }
        if (started) write_all(fd, "OK\n", 3);
        else if (found) write_all(fd, "ERR sshd failed\n", 16);
        else write_all(fd, "ERR sshd not found\n", 19);
    } else if (strcmp(buf, "SSH_STOP") == 0) {
        kill_by_name("sshd");
        kill_by_name("dropbear");
        write_all(fd, "OK\n", 3);
    } else if (strcmp(buf, "DMESG") == 0 || strncmp(buf, "DMESG ", 6) == 0) {
        handle_dmesg(fd);
    } else if (strcmp(buf, "REBOOT") == 0) {
        write_all(fd, "OK\n", 3);
        sync();
        reboot(RB_AUTOBOOT);
    } else {
        write_all(fd, "ERR UNKNOWN\n", 12);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return 1;
    fcntl(srv, F_SETFD, FD_CLOEXEC);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path));

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 1;
    chmod(SOCK_PATH, 0666);
    if (listen(srv, 8) != 0) return 1;

    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        fcntl(c, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle_client(c);
        close(c);
    }
    return 0;
}
