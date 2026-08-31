#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 GhostLock-X200-App contributors
#
# build_runner.sh — 编译 native runner（静态，aarch64，Android bionic）。
#
# ⚠️ NDK 版本要求（真机验证 2026-08-26 实证）：
#   NDK r27+ (clang 18+) 静态链接生成 TLS 段，对齐=8，而 ARM64 Bionic 要求
#   >=64 → 真机 exec 报 "TLS segment is underaligned: alignment is 8 ..."
#   （com.ghostlock.x200.app 首跑失败证据）。主项目 glt_esync 用老 NDK
#   （clang 7/8 时代，r19c-r21e）静态链接无 TLS 段，真机 40+ 次可跑。
#   → 本脚本**优先用老 NDK（r21e/r19c）**；无老 NDK 时才回退新 NDK（会失败，
#   仅用于编译期检查）。
#
# 用法: bash build_runner.sh [NDK_ROOT] [device_id]
#   NDK_ROOT 缺省探测顺序:
#     /root/android-ndk-r21e (老, 首选) / /usr/lib/android-sdk/ndk/* (老) /
#     $ANDROID_NDK_HOME / $ANDROID_NDK / /root/ndk_r27/android-ndk-r27 (新, 兜底)
#   device_id 缺省 x200_b57; 编译 runner/devices_<id>.h（devices.h 经
#     -DDEVICE_<ID> 选择）。换机型流程见 docs/NEW_DEVICE.md。
#
# 产物: ../build/runner  (ELF, static, EXEC)
#       ../build/last_device_id (机型标记, build_app.sh 一致性校验用)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)/build"
mkdir -p "$OUT_DIR"

# --- device_id → 机型头 + 编译宏 ---
DEVICE_ID="${2:-x200_b57}"
DEVICE_H="$SCRIPT_DIR/devices_$DEVICE_ID.h"
if [ ! -f "$DEVICE_H" ]; then
  echo "ERROR: 找不到机型头 $DEVICE_H" >&2
  echo "  新机型: 先跑 tools/gen_device_header.py <pc_root> $DEVICE_ID（见 docs/NEW_DEVICE.md）" >&2
  exit 1
fi
DEVICE_MACRO="DEVICE_$(printf '%s' "$DEVICE_ID" | tr '[:lower:]' '[:upper:]' | tr -c '[:alnum:]' '_')"
echo "DEVICE_ID: $DEVICE_ID (macro $DEVICE_MACRO, header $DEVICE_H)"

# --- 探测 NDK（老版本优先：TLS 布局兼容真机 Bionic） ---
NDK=""
for cand in "${1:-}" /root/android-ndk-r21e /root/android-ndk-r19c \
            /usr/lib/android-sdk/ndk/* /opt/android-ndk* ; do
  [[ -z "$NDK" && -n "$cand" && -d "$cand" ]] && NDK="$cand"
done
# 环境变量其次（新 NDK 兜底，编译会成功但真机 TLS 拒）
if [[ -z "$NDK" && -n "${ANDROID_NDK_HOME:-}" && -d "$ANDROID_NDK_HOME" ]]; then NDK="$ANDROID_NDK_HOME"; fi
if [[ -z "$NDK" && -n "${ANDROID_NDK:-}" && -d "$ANDROID_NDK" ]]; then NDK="$ANDROID_NDK"; fi
if [[ -z "$NDK" && -d "/root/ndk_r27/android-ndk-r27" ]]; then NDK="/root/ndk_r27/android-ndk-r27"; fi
if [[ -z "$NDK" ]]; then
  echo "ERROR: NDK not found (老 NDK r21e 优先; 新 NDK r27 会产生 TLS 对齐问题)" >&2
  exit 1
fi

CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang"
if [[ ! -x "$CC" ]]; then
  CC="$(ls "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/"aarch64-linux-android*-clang 2>/dev/null | head -1 || true)"
fi
if [[ -z "$CC" || ! -x "$CC" ]]; then
  echo "ERROR: aarch64 clang not found in $NDK" >&2
  exit 1
fi

echo "CC: $CC"
# 纯 -static（不 -fPIE/-pie）：与主项目 glt_esync 同款链接方式（老 NDK 下无 TLS 段）。
# -D$DEVICE_MACRO: devices.h 选择对应机型偏移表（编译期硬编码，运行时零分析）。
"$CC" -O2 -static -Wall -Wextra \
  -I"$SCRIPT_DIR" \
  -D"$DEVICE_MACRO" \
  "$SCRIPT_DIR/runner.c" "$SCRIPT_DIR/util.c" "$SCRIPT_DIR/kallsyms.c" \
  "$SCRIPT_DIR/gl.c" "$SCRIPT_DIR/rootcmd.c" "$SCRIPT_DIR/patcher.c" \
  "$SCRIPT_DIR/stages.c" \
  -o "$OUT_DIR/runner"

# 机型标记：build_app.sh 用它校验 runner 与 payload 机型一致（防错配）
printf '%s' "$DEVICE_ID" > "$OUT_DIR/last_device_id"

echo "OK: $OUT_DIR/runner (device=$DEVICE_ID)"
# 真机兼容检查：TLS 段必须不存在或对齐 >= 64
if command -v readelf >/dev/null 2>&1; then
  tls_align=$(readelf -lW "$OUT_DIR/runner" 2>/dev/null | awk '/TLS/{getline; print $NF}' | head -1)
  if [[ -n "$tls_align" && "$tls_align" != "0x40" && "$tls_align" != "0x10000" && "$tls_align" != "0x4000" ]]; then
    echo "WARN: TLS 段对齐=$tls_align（Bionic 要求>=64），真机可能拒绝执行；建议换老 NDK" >&2
  else
    echo "TLS 检查: OK（无 TLS 段或对齐合规）"
  fi
fi
"$CC" -dumpmachine >/dev/null
ls -l "$OUT_DIR/runner"
