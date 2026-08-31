/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * runner.h — native 执行器公共声明。
 * 设备端一键 root 编排器：行为 = 主项目 root_full_permissive_restore.ps1
 * 的逐 STAGE 移植（离线验证版），全部写目标硬编码（见 devices_x200_b57.h）。
 */
#ifndef RUNNER_H
#define RUNNER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- 日志 ---- */
#define LOG(fmt, ...)  do { printf("[GL] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)
#define LOGS(fmt, ...) do { printf(fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

/* ---- 运行配置（main 解析） ---- */
typedef struct {
    const char *work;      /* payload 工作目录（app 私有目录，解压后的资产） */
    const char *tag;       /* 本次运行 tag（时间戳），用于文件命名 */
    const char *bindir;    /* 可执行二进制目录（nativeLibraryDir） */
    const char *ksym_path; /* self-test 用 kallsyms 样例文件 */
    int self_test;         /* 1 = 只跑门禁/解析单测，不触发提权 */
} RunnerCfg;

/* ---- 通用工具（util.c） ---- */
char *read_file(const char *path);              /* malloc 返回，调用方 free；失败返回 NULL */
int   write_file(const char *path, const char *data);
long  get_loadavg_1min(void);                   /* -1 = 失败 */
long  get_uptime_seconds(void);                 /* 开机秒数；-1 = 失败 */
uint64_t get_memtotal_bytes(void);              /* 0 = 失败 */
char *get_boot_id(void);                        /* malloc；失败 NULL */
int   sh_run(const char *cmd, char *out, size_t outsz); /* /system/bin/sh -c，捕获输出 */
int   getenforce_permissive(void);              /* 读 getenforce；1=Permissive 0=Enforcing -1=失败 */
int   enforce_readable(void);                    /* 1=能读 /sys/fs/selinux/enforce（Permissive 的证据） */
int   process_has_seccomp(void);                 /* 1=进程带 seccomp 过滤器（zygote app=2, adb shell=0） */
const char *get_osrelease(char *buf, size_t sz); /* 内核版本；/proc 读不到时 fallback uname(2) */
long  get_kptr_restrict(void);                  /* -1 = 失败 */
int   dev_alive(void);                          /* 简单存活检查 */
void  sleep_s(int s);
void  sleep_ms(int ms);

/* exec_timeout：fork+exec 带超时（替代 timeout 命令，避免 sh 中转）。
 * path 可执行文件；argv[0] 约定 = path 自身；envp 可 NULL（继承）。
 * 输出重定向到 log_path（NULL 则 /dev/null）。
 * 返回：0=正常退出；1=超时被杀；-1=fork/exec 失败。正常退出时 *exit_code 收到子进程退出码。 */
int exec_timeout(const char *path, char *const argv[], char *const envp[],
                 const char *log_path, int timeout_s, int *exit_code);

/* kill_all_named：按 /proc/[pid]/stat comm 前缀清杀同 uid 进程（跳过自身）。
 * 用于清 glt/w2host fork 残留（exec_timeout 超时只杀主进程）。 */
void kill_all_named(const char *prefix);

/* ---- GL 槽位（gl.c） ---- */
/* boot_id + 状态文件轮换；跳过 slot 1（宿主有毒）；返回 lock 地址（0x 文本） */
const char *gl_lock_next(const char *work, char *buf, size_t bufsz);
void gl_state_reset(const char *work);          /* permissive_restore 清零宿主后重置 */

/* ---- kallsyms（kallsyms.c） ---- */
typedef struct {
    uint64_t addr;
    char type;
    char name[128];
} Sym;

typedef struct {
    Sym *items;
    size_t count;
    size_t cap;
    uint64_t kbase;   /* _text 地址（解析时填入） */
} SymTab;

/* 从文件解析（self-test 用）。成功 0，失败 -1。 */
int symtab_load(SymTab *st, const char *path);
/* 从 /proc/kallsyms 解析。成功 0，失败 -1。 */
int symtab_load_proc(SymTab *st);
void symtab_free(SymTab *st);
/* 精确找符号（addr!=0，首个命中）。找到返回 addr，否则 0。 */
uint64_t symtab_find(const SymTab *st, const char *name);
/* capsym 筛选（offsets_auto.py 同规则）：
 *   .*__key 的 b/B 符号，kbase<=addr<kbase+0x4000000，(addr>>16)&1，
 *   排序：init_completion.__key 优先 → cap 位 score(1/12/19/21) 降序 → addr 升序。
 * 返回地址，无候选返回 kbase。 */
uint64_t symtab_capsym(const SymTab *st, uint64_t kbase);

/* ---- rootcmd（rootcmd.c，w2host AF_UNIX 客户端） ---- */
/* 发送单条命令，返回 stdout 文本（malloc）；失败返回 NULL。 */
char *rootcmd_call(const char *sock_path, const char *cmd);

/* ---- ko 迷你 patcher（patcher.c） ---- */
/* 把 src.ko 复制到 dst，并用符号表把 symtab 中未定义/已知符号的 st_value
 * 改写为符号绝对地址（patch_ko_all.py 的 C 移植）。
 * 返回 0 成功，-1 失败（dst 不写入/部分写入时删除）。 */
int patch_ko(const char *src, const char *dst, const SymTab *st);

/* ---- STAGE 实现（stages.c） ---- */
int stage0_gate(const RunnerCfg *cfg);
int stage1_permissive(const RunnerCfg *cfg);
int stage2_kptr(const RunnerCfg *cfg);
int stage3_base(const RunnerCfg *cfg, SymTab *ksym);
int stage4_quiesce(const RunnerCfg *cfg);
int stage4_leak(const RunnerCfg *cfg, char *cands, size_t candsz);
int stage5_capsroot(const RunnerCfg *cfg, const SymTab *ksym, const char *cands);
int stage6_insmod(const RunnerCfg *cfg, const SymTab *ksym);
int stage7_verify(const RunnerCfg *cfg);

#endif /* RUNNER_H */
