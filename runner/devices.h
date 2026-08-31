/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * devices.h — 机型偏移表选择器。
 *
 * 选择规则：编译期由 build_runner.sh 通过 -DDEVICE_<ID> 指定
 * （ID 大写化、连字符/点转下划线，如 x200_b57 → DEVICE_X200_B57）。
 * 未定义任何 DEVICE_* 时回退默认机型 x200_b57（host self-test 与
 * 直接 gcc 编译不受影响）。
 *
 * 换机型流程（详见 docs/NEW_DEVICE.md）：
 *   1. PC 版（ghostlock-x200-root）offsets_auto.py package 生成
 *      devices/<id>/（win_offs.json + device.json + prebuilt/...）
 *   2. tools/gen_device_header.py 由上述 json 生成 runner/devices_<id>.h
 *   3. bash runner/build_runner.sh <ndk> <id>
 *   4. bash tools/build_app.sh <sdk> <id>
 * 本文件为唯一需要手工登记机型的地方（每机型一行）。
 */
#ifndef DEVICES_H
#define DEVICES_H

#if defined(DEVICE_X200_B57)
#  include "devices_x200_b57.h"

#elif defined(DEVICE_PD2458)
#  include "devices_pd2458.h"

/* ---- 新增机型在此追加（对照格式，id 大写化） ----
#elif defined(DEVICE_<NEW_ID>)
#  include "devices_<new_id>.h"
---- 追加结束 ---- */

#else
/* 未指定机型：回退默认 x200_b57（host 自检 / 直接编译）。 */
#  define DEVICE_X200_B57
#  include "devices_x200_b57.h"
#endif

#endif /* DEVICES_H */
