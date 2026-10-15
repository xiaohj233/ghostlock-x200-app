/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * util.c — 设备端通用工具：文件读写、负载/内存/状态读取、带超时执行。
 */
#include "runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>

/* ---- 文件 ---- */
/* 流式读取直到 EOF。不能用 fseek/ftell：sysfs/procfs 文件不支持 seek
 * （ESPIPE / ftell=0）→ 读到空串 → 判定函数全瞎。
 * 2026-08-29 真机实证：STAGE1 写原语已落地（设备 Permissive + kptr=0），
 * runner 仍判 enforcing（read_file 空串）；get_kptr_restrict 返回 -1。 */
char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got;
    while ((got = fread(buf + len, 1, cap - len, f)) > 0) {
        len += got;
        if (len == cap) {
            cap *= 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t n = strlen(data);
    int ok = (fwrite(data, 1, n, f) == n) ? 0 : -1;
    fclose(f);
    return ok;
}

/* ---- 系统读数 ---- */
/* 内核版本：优先 /proc/sys/kernel/osrelease（adb/shell 域可读）；
 * app 域（untrusted_app）在 Enforcing 下被 SELinux 拒 → fallback uname(2)
 * （zygote seccomp 不拦 uname）。 */
const char *get_osrelease(char *buf, size_t sz) {
    char *s = read_file("/proc/sys/kernel/osrelease");
    if (s) {
        snprintf(buf, sz, "%s", s);
        free(s);
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
        if (n > 0) return buf;
    }
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(buf, sz, "%s", u.release);
        return buf;
    }
    return NULL;
}

/* 开机秒数（/proc/uptime 第一个字段）。失败返回 -1。
 * PC root.ps1 在 STAGE 前有 uptime>240s 稳定等待（root.ps1:245-270，
 * 注释原文「设备未稳定可能导致内核 panic」）；app 版曾因重启后立即
 * 竞速（开机风暴窗口）部分命中 panic，故补同款门。 */
long get_uptime_seconds(void) {
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return -1;
    double up = 0;
    int n = fscanf(f, "%lf", &up);
    fclose(f);
    if (n != 1) return -1;
    return (long)up;
}

long get_loadavg_1min(void) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return -1;
    double a = 0;
    int n = fscanf(f, "%lf", &a);
    fclose(f);
    if (n != 1) return -1;
    return (long)(a * 100.0);
}

uint64_t get_memtotal_bytes(void) {
    char *s = read_file("/proc/meminfo");
    if (!s) return 0;
    uint64_t kb = 0;
    char *p = strstr(s, "MemTotal:");
    if (p) sscanf(p, "MemTotal: %llu kB", (unsigned long long *)&kb);
    free(s);
    return kb * 1024ULL;
}

char *get_boot_id(void) {
    char *s = read_file("/proc/sys/kernel/random/boot_id");
    if (!s) return NULL;
    /* 去换行 */
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
    return s;
}

int getenforce_permissive(void) {
    char *s = read_file("/sys/fs/selinux/enforce");
    if (s) {
        int v = (s[0] == '0');
        free(s);
        return v;
    }
    /* 回退 getenforce 命令 */
    char out[64] = {0};
    if (sh_run("getenforce", out, sizeof(out)) == 0) {
        if (strstr(out, "Permissive")) return 1;
        if (strstr(out, "Enforcing")) return 0;
    }
    return -1;
}

/* untrusted_app 域在 Enforcing 下读 /sys/fs/selinux/enforce 被 SELinux 拒
 * （EACCES）；Permissive 后即可读。故"能否打开该文件"本身可作 permissive
 * 证据（上游 ghostlock-app check_selinux_off 同思路）。返回 1=可读。 */
int enforce_readable(void) {
    return access("/sys/fs/selinux/enforce", R_OK) == 0;
}

/* 进程是否带 seccomp 过滤器（zygote 派生 app=2；adb shell=0）。
 * 上游 ghostlock-app process_has_seccomp 同思路；用于评估竞速写原语
 * 与 finit_module 的可用性（seccomp 不禁 futex/pselect，但可能禁其他）。 */
int process_has_seccomp(void) {
    char *s = read_file("/proc/self/status");
    if (!s) return 0;
    int seccomp = 0;
    char *line = s;
    char *nl;
    while ((nl = strchr(line, '\n')) != NULL) {
        *nl = 0;
        if (strncmp(line, "Seccomp:", 8) == 0) { seccomp = atoi(line + 8); break; }
        line = nl + 1;
    }
    free(s);
    return seccomp != 0;
}

long get_kptr_restrict(void) {
    char *s = read_file("/proc/sys/kernel/kptr_restrict");
    if (!s) return -1;
    long v = strtol(s, NULL, 10);
    free(s);
    return v;
}

int dev_alive(void) { return access("/proc/version", R_OK) == 0; }

void sleep_s(int s) { sleep((unsigned)s); }
void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

int sh_run(const char *cmd, char *out, size_t outsz) {
    int pfd[2];
    if (pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[1]);
        execl("/system/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    size_t used = 0;
    if (out && outsz > 0) {
        ssize_t r;
        while (used + 1 < outsz && (r = read(pfd[0], out + used, outsz - used - 1)) > 0)
            used += (size_t)r;
        out[used] = '\0';
    }
    close(pfd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* 单调时钟毫秒（防负载高时 sleep 漂移导致超时严重超支，实测 20s→81s） */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

typedef struct {
    char comm[16];
    char state;
    unsigned long long start;
} ProcIdentity;

static int proc_identity(pid_t pid, ProcIdentity *info) {
    char path[64], data[2048];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n;
    do { n = read(fd, data, sizeof(data) - 1); } while (n < 0 && errno == EINTR);
    int error = errno;
    close(fd);
    if (n <= 0) { errno = n < 0 ? error : EIO; return -1; }
    data[n] = '\0';
    char *left = strchr(data, '('), *right = strrchr(data, ')');
    if (!left || !right || right <= left || (size_t)(right - left - 1) >= sizeof(info->comm)) {
        errno = EIO; return -1;
    }
    memcpy(info->comm, left + 1, (size_t)(right - left - 1));
    info->comm[right - left - 1] = '\0';
    char *save = NULL, *token = strtok_r(right + 1, " ", &save);
    for (int field = 3; field <= 22; field++, token = strtok_r(NULL, " ", &save)) {
        if (!token) { errno = EIO; return -1; }
        if (field == 3) info->state = token[0];
        if (field == 22) {
            char *end;
            errno = 0;
            info->start = strtoull(token, &end, 10);
            if (errno || *end) { errno = EIO; return -1; }
        }
    }
    return 0;
}

/* File identity covers renamed comm values, legacy lib*.so names and children
 * adopted by init. Path identity also covers a previous, replaced executable
 * reported by procfs as "... (deleted)". Names alone never authorize a kill. */
int cleanup_exec(const char *path, int timeout_ms) {
    char canonical[PATH_MAX];
    struct stat target;
    if (!realpath(path, canonical) || stat(canonical, &target) != 0) {
        LOG("PROCESS_CLEANUP FAIL: cannot inspect executable (%s)", strerror(errno));
        return -1;
    }
    const char *base = strrchr(canonical, '/');
    base = base ? base + 1 : canonical;
    char short_name[16];
    snprintf(short_name, sizeof(short_name), "%.15s", base);
    long long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
    int first = 1, empty_passes = 0;
    for (;;) {
        DIR *dir = opendir("/proc");
        if (!dir) { LOG("PROCESS_CLEANUP FAIL: cannot scan procfs"); return -1; }
        int remaining = 0, failed = 0;
        struct dirent *entry;
        for (;;) {
            errno = 0;
            entry = readdir(dir);
            if (!entry) {
                if (errno) { LOG("PROCESS_CLEANUP FAIL: incomplete process scan"); failed = 1; }
                break;
            }
            char *end;
            long value = strtol(entry->d_name, &end, 10);
            if (*end || value <= 1 || value > INT_MAX || value == getpid()) continue;
            pid_t pid = (pid_t)value;
            ProcIdentity identity;
            if (proc_identity(pid, &identity) != 0) continue; /* disappeared/inaccessible unrelated task */
            if (identity.state == 'Z' || identity.state == 'X') {
                /* Only reap this executable's children through their normal
                 * wait path; an unrelated child status must remain available. */
                continue;
            }
            char proc_path[64], executable[PATH_MAX];
            snprintf(proc_path, sizeof(proc_path), "/proc/%d/exe", (int)pid);
            ssize_t size = readlink(proc_path, executable, sizeof(executable) - 1);
            if (size < 0) {
                int error = errno;
                ProcIdentity current;
                if (strcmp(identity.comm, short_name) == 0 &&
                    proc_identity(pid, &current) == 0 && current.start == identity.start &&
                    current.state != 'Z' && current.state != 'X') {
                    LOG("PROCESS_CLEANUP FAIL: cannot verify candidate pid=%d (%s)", (int)pid, strerror(error));
                    failed = 1;
                }
                continue;
            }
            executable[size] = '\0';
            const char suffix[] = " (deleted)";
            size_t length = strlen(executable);
            if (length >= sizeof(suffix) - 1 &&
                strcmp(executable + length - (sizeof(suffix) - 1), suffix) == 0)
                executable[length - (sizeof(suffix) - 1)] = '\0';
            struct stat actual;
            int same_path = strcmp(executable, canonical) == 0;
            int same_file = stat(proc_path, &actual) == 0 && actual.st_dev == target.st_dev && actual.st_ino == target.st_ino;
            if (!same_path && !same_file) continue;
            /* Recheck start time immediately before signalling: stale PID
             * files and a reused PID must not select another process. */
            ProcIdentity current;
            if (proc_identity(pid, &current) != 0 || current.start != identity.start ||
                current.state == 'Z' || current.state == 'X') continue;
            remaining++;
            if (first && remaining <= 8)
                LOG("PROCESS_CLEANUP: pid=%d state=%c comm=%s", (int)pid, current.state, current.comm);
            if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
                LOG("PROCESS_CLEANUP FAIL: signal pid=%d (%s)", (int)pid, strerror(errno));
                failed = 1;
            }
        }
        closedir(dir);
        if (failed) return -1;
        first = 0;
        if (!remaining) {
            if (++empty_passes >= 2) return 0;
        } else empty_passes = 0;
        if (now_ms() >= deadline) {
            LOG("PROCESS_CLEANUP FAIL: %d live process(es) remain; stopped", remaining);
            errno = EBUSY;
            return -1;
        }
        sleep_ms(25);
    }
}

void runner_event(const RunnerCfg *cfg, const char *event, int result) {
    if (!cfg || !cfg->tag || !cfg->work) return;
    size_t length = strlen(cfg->tag);
    if (!length || length > 64 || strspn(cfg->tag, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") != length) return;
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/runner_%s.log", cfg->work, cfg->tag);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) { LOG("TRACE WARN: cannot open run journal (%s)", strerror(errno)); return; }
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    if (dprintf(fd, "time=%lld.%03ld monotonic_ms=%lld pid=%d event=%s result=%d\n",
                (long long)wall.tv_sec, wall.tv_nsec / 1000000, now_ms(), (int)getpid(), event, result) < 0 || fsync(fd) != 0)
        LOG("TRACE WARN: cannot persist run journal (%s)", strerror(errno));
    close(fd);
}

int exec_timeout(const char *path, char *const argv[], char *const envp[],
                 const char *log_path, int timeout_s, int *exit_code) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (setpgid(0, 0) != 0) _exit(126);
        int fd = open(log_path ? log_path : "/dev/null",
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        if (envp)
            execve(path, argv, envp);
        else
            execv(path, argv);
        _exit(127);
    }
    /* Both sides establish the group; EACCES means the child exec'd first. */
    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) {
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        if (exit_code) *exit_code = -1;
        return -1;
    }
    int st = 0;
    long long deadline = now_ms() + (long long)timeout_s * 1000LL;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            if (exit_code) *exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            return 0;
        }
        if (r < 0 && errno != EINTR) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            if (exit_code) *exit_code = -1;
            return -1;
        }
        if (now_ms() >= deadline) break;
        sleep_ms(50);
    }
    /* 超时：进程组击杀 + 主进程兜底 + 名杀兜底（组外/脱离组残党） */
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    for (int i = 0; i < 40; i++) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        sleep_ms(50);
    }
    if (exit_code) *exit_code = -1;
    return 1;
}
