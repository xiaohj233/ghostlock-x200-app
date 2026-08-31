#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 GhostLock-X200-App contributors
#
# build_runner_host.sh — 编译本机(x86_64)版 runner 仅用于 --self-test。
# 离线验证符号解析 / 门禁 / patcher 逻辑；提权路径永远走 aarch64 版。
# 用法: bash build_runner_host.sh   → build/runner_host

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)/build"
mkdir -p "$OUT_DIR"

cc="${CC:-gcc}"
$cc -O2 -Wall -Wextra \
  -I"$SCRIPT_DIR" \
  "$SCRIPT_DIR/runner.c" "$SCRIPT_DIR/util.c" "$SCRIPT_DIR/kallsyms.c" \
  "$SCRIPT_DIR/gl.c" "$SCRIPT_DIR/rootcmd.c" "$SCRIPT_DIR/patcher.c" \
  "$SCRIPT_DIR/stages.c" \
  -o "$OUT_DIR/runner_host"
echo "OK: $OUT_DIR/runner_host"
