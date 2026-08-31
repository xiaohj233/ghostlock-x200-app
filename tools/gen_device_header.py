#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 GhostLock-X200-App contributors
"""
gen_device_header.py — 由 PC 版机型模块生成 app 侧机型偏移头。

PC 版（ghostlock-x200-root）的 offsets_auto.py package 已把机型全部偏移
整合为 devices/<id>/（device.json + win_offs.json + offsets.json + target.h
+ pselect.json + manifest.json）。本脚本读取其中三个 json，映射生成 app
runner 侧的 devices_<id>.h（编译期硬编码偏移表），使人肉抄偏移归零。

映射来源（优先级）：
  - win_offs.json  首选：physmap_base / sym_offs{selinux_state,kptr_restrict,
    zeroes_buf} / w2host{cred_ip_offs, win_lo, win_hi}
  - offsets.json   回退/补全：struct_offsets.cred_cap_permitted（DEV_CAP_OFF）,
    w2host 窗口（win_offs 缺失时）
  - device.json    元数据：kernel_release / device / verified / notes /
    w2_cand_min（DEV_W2_CAND_MIN_ADD = w2_cand_min - physmap_base）

派生宏（编译期合成，与 devices_x200_b57.h 模板一致）：
  DEV_SE_P0 / DEV_W0 / DEV_GL_LOCK_BASE
固定设计值（跨机型通用，无需 PC 数据）：
  DEV_GL_SLOT_SIZE=0x10 / DEV_GL_SLOT_BAD=1 / DEV_CAPSYM_WIN=0x4000000
  DEV_VR_TAG_A/B/TP（clear_vr_tag 默认 6/44/1024；若 PC 模块带 vr_offsets.json
  则以其为准）

用法:
  python3 tools/gen_device_header.py <pc_root> <device_id> [--out <path>]
    pc_root     = ghostlock-x200-root 仓库根
    device_id   = 机型 id（对应 <pc_root>/devices/<id>/）
    --out       = 输出路径（缺省: 本仓库 runner/devices_<id>.h）
    --from      = 用已有设备模块的数据生成另一 id 的头（演示/占位用；
                  仅改 device_id 与注释，偏移值同源）
"""

import argparse
import json
import os
import re
import sys

# ---- 字段映射与默认值 ----
DEFAULTS = {
    "DEV_CAP_OFF": 56,           # cred_cap_permitted 偏移（同 6.6 内核族通用）
    "DEV_W2_CAND_MIN_ADD": 0x40000000,  # 排除低 1GB（内核镜像/固件）
    "DEV_CAPSYM_WIN": 0x4000000, # capsym 筛选窗口（offsets_auto.py 同规则）
    "DEV_VR_TAG_A": 6,           # clear_vr_tag b57 内置默认
    "DEV_VR_TAG_B": 44,
    "DEV_VR_TP": 1024,
    "DEV_GL_SLOT_SIZE": 0x10,
    "DEV_GL_SLOT_BAD": 1,
}


def load_json(path, required=True):
    if not os.path.isfile(path):
        if required:
            sys.exit(f"ERROR: 缺少 {path}")
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def hexval(s):
    """'0x...' 或 '0X...' → int"""
    if isinstance(s, int):
        return s
    return int(s, 16)


def parse_addr(v):
    """json 里的地址值 → 0x%x 字符串"""
    if v is None:
        return None
    return f"0x{v:016x}" if isinstance(v, int) else f"{v}"


def pick_offs(win, offs, key):
    """优先 win_offs.sym_offs.<key>，回退 offsets 内的 *_p0 减 physmap"""
    if win and win.get("sym_offs", {}).get(key) is not None:
        return hexval(win["sym_offs"][key])
    if offs:
        p0 = offs.get(f"{key}_p0")
        if p0 is not None and offs.get("selinux_enforcing_p0") and False:
            return None  # 不用绝对 p0 反推（physmap 可能不在 offs）
    return None


def main():
    ap = argparse.ArgumentParser(description="由 PC 版机型模块生成 app 侧偏移头")
    ap.add_argument("pc_root", help="ghostlock-x200-root 仓库根")
    ap.add_argument("device_id", help="机型 id（<pc_root>/devices/<id>/）")
    ap.add_argument("--out", help="输出路径（缺省: 本仓库 runner/devices_<id>.h）")
    ap.add_argument("--from", dest="src_id", help="用已有设备模块数据生成（演示/占位）")
    args = ap.parse_args()

    src_id = args.src_id or args.device_id
    mod_dir = os.path.join(args.pc_root, "devices", src_id)
    if not os.path.isdir(mod_dir):
        sys.exit(f"ERROR: PC 版机型模块不存在: {mod_dir}")

    win = load_json(os.path.join(mod_dir, "win_offs.json"), required=False)
    offs = load_json(os.path.join(mod_dir, "offsets.json"), required=False)
    dev = load_json(os.path.join(mod_dir, "device.json"), required=False)
    vr = load_json(os.path.join(mod_dir, "vr_offsets.json"), required=False)
    if win is None and offs is None:
        sys.exit("ERROR: 模块缺 win_offs.json 与 offsets.json，无法生成")

    # ---- 偏移取值 ----
    physmap = None
    for j in (dev, win):
        if j and j.get("physmap_base"):
            physmap = hexval(j["physmap_base"])
            break
    if physmap is None:
        sys.exit("ERROR: 找不到 physmap_base")

    def sym_off(key):
        if win and win.get("sym_offs", {}).get(key) is not None:
            return hexval(win["sym_offs"][key])
        if offs and offs.get(f"{key}_p0") is not None:
            return hexval(offs[f"{key}_p0"]) - physmap
        return None

    selinux_off = sym_off("selinux_state")
    kptr_off = sym_off("kptr_restrict")
    zeroes_off = sym_off("zeroes_buf")
    missing = [k for k, v in (("selinux_state", selinux_off),
                              ("kptr_restrict", kptr_off),
                              ("zeroes_buf", zeroes_off)) if v is None]
    if missing:
        sys.exit(f"ERROR: 模块缺符号偏移: {missing}（需要 win_offs.sym_offs 或 *_p0）")

    # w2host 窗口
    w2src = (win or {}).get("w2host") or (offs or {}).get("w2host") or {}
    cred_ips = w2src.get("cred_ip_offs")
    win_lo = hexval(w2src["win_lo"]) if w2src.get("win_lo") is not None else None
    win_hi = hexval(w2src["win_hi"]) if w2src.get("win_hi") is not None else None
    if not (cred_ips and win_lo is not None and win_hi is not None):
        sys.exit("ERROR: 模块缺 w2host 窗口（cred_ip_offs/win_lo/win_hi）")
    cred_ips_lit = ", ".join(f"0x{hexval(x):x}ULL" for x in cred_ips)

    # cap_off：offsets.json.struct_offsets.cred_cap_permitted（回退 56）
    cap_off = DEFAULTS["DEV_CAP_OFF"]
    if offs and offs.get("struct_offsets", {}).get("cred_cap_permitted") is not None:
        cap_off = int(offs["struct_offsets"]["cred_cap_permitted"])

    # W2_CAND_MIN_ADD：device.json.w2_cand_min - physmap（回退 0x40000000）
    cand_add = DEFAULTS["DEV_W2_CAND_MIN_ADD"]
    if dev and dev.get("w2_cand_min"):
        cand_add = hexval(dev["w2_cand_min"]) - physmap

    # clear_vr_tag：vr_offsets.json（若有）覆盖默认
    tag_a = vr.get("tag_a", DEFAULTS["DEV_VR_TAG_A"]) if vr else DEFAULTS["DEV_VR_TAG_A"]
    tag_b = vr.get("tag_b", DEFAULTS["DEV_VR_TAG_B"]) if vr else DEFAULTS["DEV_VR_TAG_B"]
    tp = vr.get("tp", DEFAULTS["DEV_VR_TP"]) if vr else DEFAULTS["DEV_VR_TP"]

    # 元数据
    kernel_release = (dev or {}).get("kernel_release") or (win or {}).get("kernel_release") \
        or (offs or {}).get("kernel")
    if not kernel_release:
        sys.exit("ERROR: 缺 kernel_release")
    dev_name = (dev or {}).get("device", src_id)
    verified = (dev or {}).get("verified", False)
    notes = (dev or {}).get("notes", "")

    # ---- 输出 ----
    out_path = args.out or os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "runner", f"devices_{args.device_id}.h")

    guard = f"DEVICES_{re.sub(r'[^A-Za-z0-9]', '_', args.device_id).upper()}_H"
    src_note = f"PC 版机型模块 devices/{src_id}/"

    hdr = f"""/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * devices_{args.device_id}.h — 机型硬编码偏移表（由 tools/gen_device_header.py 生成）。
 *
 * 来源：{src_note}
 *   device.json  kernel_release = {kernel_release}
 *   device.json  device = {dev_name}  verified = {verified}
 *   notes: {notes}
 * 生成：tools/gen_device_header.py
 * 警告：本文件为自动生成，请勿手改偏移；重生成请重跑
 *   python3 tools/gen_device_header.py <pc_root> {args.device_id}
 */
#ifndef {guard}
#define {guard}

#define DEV_KERNEL_RELEASE \\
    "{kernel_release}"

/* ---- physmap 基址与符号偏移（win_offs.json / offsets.json） ---- */
#define DEV_PHYSMAP_BASE      {parse_addr(physmap)}
#define DEV_SELINUX_STATE_OFF 0x{selinux_off:x}ULL
#define DEV_KPTR_RESTRICT_OFF 0x{kptr_off:x}ULL
#define DEV_ZEROES_BUF_OFF    0x{zeroes_off:x}ULL

/* 派生写目标（编译期算好，运行时直接用） */
#define DEV_SE_P0        (DEV_PHYSMAP_BASE + DEV_SELINUX_STATE_OFF)
#define DEV_W0           (DEV_PHYSMAP_BASE + DEV_KPTR_RESTRICT_OFF - 8)
#define DEV_GL_LOCK_BASE (DEV_PHYSMAP_BASE + DEV_ZEROES_BUF_OFF)

/* GL 槽位 */
#define DEV_GL_SLOT_SIZE 0x{DEFAULTS['DEV_GL_SLOT_SIZE']:x}ULL
#define DEV_GL_SLOT_BAD  {DEFAULTS['DEV_GL_SLOT_BAD']}

/* task_struct->cred 内 cap_permitted 偏移（STAGE5 写目标） */
#define DEV_CAP_OFF {cap_off}

/* w2host 采样窗口（cred 泄漏） */
#define DEV_W2_IP_LO   0x{win_lo:x}ULL
#define DEV_W2_IP_HI   0x{win_hi:x}ULL
#define DEV_W2_CRED_IPS {{ {cred_ips_lit} }}
#define DEV_W2_CAND_MIN_ADD 0x{cand_add:x}ULL

/* clear_vr_tag 参数（默认值；如 PC 模块带 vr_offsets.json 则以其为准） */
#define DEV_VR_TAG_A {tag_a}
#define DEV_VR_TAG_B {tag_b}
#define DEV_VR_TP    {tp}

/* capsym 筛选窗口（相对 kbase，同 offsets_auto.py live 提取） */
#define DEV_CAPSYM_WIN 0x{DEFAULTS['DEV_CAPSYM_WIN']:x}ULL

#endif /* {guard} */
"""
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(hdr)
    print(f"OK: {out_path}")
    print(f"  kernel_release = {kernel_release}")
    print(f"  physmap={parse_addr(physmap)} selinux={selinux_off:#x} "
          f"kptr={kptr_off:#x} zeroes={zeroes_off:#x} cap={cap_off}")
    print(f"  w2: lo={win_lo:#x} hi={win_hi:#x} cred_ips={cred_ips}")


if __name__ == "__main__":
    main()
