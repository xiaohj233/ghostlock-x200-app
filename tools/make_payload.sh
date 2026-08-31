#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 GhostLock-X200-App contributors
#
# make_payload.sh — 从主项目 ghostlock-x200-root 归集产物到本项目 payload/ 目录。
#
# 解耦契约：主项目只"供货"以下 6 个产物文件，本项目只"进货"（拷贝 + sha256 校验）。
# 换机型 = 在 payload/ 下新增 <device_id>/ 目录 + 提供 offsets.json（见 DESIGN.md §3.1）。
#
# 用法: bash tools/make_payload.sh [主项目根] [device_id]
#   缺省: 主项目根 = 通过参数 1 指定（例: 指向 clone 的 ghostlock-x200-root）, device_id = x200_b57
#
# 验证: 脚本输出每个文件的 sha256，与主项目 prebuilt/modules 对应文件逐字节一致；
#       生成 payload/<id>/MANIFEST.sha256 供构建链与真机校验。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MAIN_PROJ="${1:?用法: bash tools/make_payload.sh <主项目根目录> [device_id]}"
DEVICE_ID="${2:-x200_b57}"

if [ ! -d "$MAIN_PROJ/prebuilt" ]; then
  echo "ERR: 主项目目录无效（无 prebuilt/）: $MAIN_PROJ" >&2
  exit 1
fi

PAYLOAD_DIR="$ROOT_DIR/payload/$DEVICE_ID"
MOD_DIR="$PAYLOAD_DIR/modules"
mkdir -p "$PAYLOAD_DIR" "$MOD_DIR"

declare -A SRC=(
  [glt_esync]="$MAIN_PROJ/prebuilt/glt_esync"
  [w2host]="$ROOT_DIR/exploit/build/w2host"   # 本仓库独立维护（ring64MB 修复版）
  [ksud]="$MAIN_PROJ/prebuilt/ksud"
  [clear_vr_tag.ko]="$MAIN_PROJ/modules/clear_vr_tag/clear_vr_tag.ko"
  [kernelsu.ko]="$MAIN_PROJ/modules/kernelsu/kernelsu.ko"
  [permissive_restore.ko]="$MAIN_PROJ/modules/permissive_restore/permissive_restore.ko"
)
# w2host 说明：主项目原版 perf ring 1MB，真机 14 次泄漏仅 ~57% 命中；本仓库
# exploit/src/w2host.c 已扩 ring 64MB（真机 5/5 命中）。构建产物放
# exploit/build/w2host（r19c 编译），缺失时回退主项目原版并告警。
if [ ! -f "${SRC[w2host]}" ]; then
  echo "  WARN: 本仓库 exploit/build/w2host 缺失，回退主项目原版 prebuilt/w2host"
  echo "        （b57 请用 ring64MB 修复版：bash exploit/build_w2host.sh 后拷到 exploit/build/）"
  SRC[w2host]="$MAIN_PROJ/prebuilt/w2host"
fi

echo "==> 归集 payload/$DEVICE_ID"
rm -f "$PAYLOAD_DIR"/glt_esync "$PAYLOAD_DIR"/w2host "$PAYLOAD_DIR"/ksud "$MOD_DIR"/*.ko

for name in glt_esync w2host ksud clear_vr_tag.ko kernelsu.ko permissive_restore.ko; do
  src="${SRC[$name]}"
  if [ ! -f "$src" ]; then echo "ERR: 缺少产物 $name ($src)" >&2; exit 1; fi
  if [[ "$name" == *.ko ]]; then dst="$MOD_DIR/$name"; else dst="$PAYLOAD_DIR/$name"; fi
  cp -f "$src" "$dst"
  chmod 755 "$dst" 2>/dev/null || true
  echo "  $(sha256sum "$dst" | cut -c1-16)  $dst"
done

# 生成清单（供构建链打包 assets 与真机校验）
(
  cd "$PAYLOAD_DIR"
  sha256sum glt_esync w2host ksud modules/*.ko
) > "$PAYLOAD_DIR/MANIFEST.sha256"
echo "==> MANIFEST.sha256 已生成: $PAYLOAD_DIR/MANIFEST.sha256"

# 机型身份：拷贝 PC 版 devices/<id>/device.json（若模块存在）。
# build_app.sh 用它校验 payload 与 runner/devices_<id>.h 的内核 release 一致。
if [ -f "$MAIN_PROJ/devices/$DEVICE_ID/device.json" ]; then
  cp -f "$MAIN_PROJ/devices/$DEVICE_ID/device.json" "$PAYLOAD_DIR/device.json"
  echo "==> 机型身份 device.json 已归集（$DEVICE_ID）"
else
  rm -f "$PAYLOAD_DIR/device.json"
  echo "  WARN: 主项目无 devices/$DEVICE_ID/device.json，未携带机型身份（build_app.sh 将仅 WARN）"
fi

echo "MAKE_PAYLOAD_OK"
