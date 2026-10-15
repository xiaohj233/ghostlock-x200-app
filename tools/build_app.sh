#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 GhostLock-X200-App contributors
#
# build_app.sh — 组装 assets → javac → d8 → aapt2 → zipalign → apksigner 打包 APK。
# 纯命令行构建，无需 Android Studio/Gradle。
#
# 依赖（在 Kali /root/androidsdk 或通过 SDK_DIR 指定）:
#   android-35/android.jar, aapt2, d8, zipalign, lib/apksigner.jar, lib/d8.jar
#
# 用法: bash tools/build_app.sh [SDK_DIR] [device_id]
#   缺省: SDK_DIR = /root/androidsdk
#         device_id = x200_b57（对应 payload/<id>/ 与 runner/devices_<id>.h）
#   产物: app/build/GhostLockX200-v1.1.0.apk（+ unsigned/unaligned 中间产物）
# 一致性防线（[1/7] 内执行）:
#   * payload/<id>/MANIFEST.sha256 存在（先跑 make_payload.sh）
#   * runner/devices_<id>.h 存在（先跑 build_runner.sh 或 gen_device_header.py）
#   * build/last_device_id == <id>（runner 按该机型编译）
#   * payload/<id>/device.json.kernel_release == header 的 DEV_KERNEL_RELEASE
#     （PC 版机型身份；device.json 缺失仅 WARN，存在但错配则 ERR 拒绝）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SDK_DIR="${1:-/root/androidsdk}"
DEVICE_ID="${2:-x200_b57}"
BT="$SDK_DIR/android-16"          # build-tools 36 解压于 android-16/（d8 需新版本兼容 JDK25）
PLATFORM="$SDK_DIR/android-35"    # platform-35 android.jar

APP_SRC="$ROOT_DIR/app/src/main"
BUILD="$ROOT_DIR/app/build"
OUT_APK="$BUILD/GhostLockX200-v1.1.0.apk"

VERSION_CODE=5
VERSION_NAME="1.1.0"
PACKAGE="com.ghostlock.x200.app"

# 可选 keystore（缺省自签 dev key）
KEYSTORE="${KEYSTORE:-$ROOT_DIR/app/ghostlock.keystore}"
KS_ALIAS="${KS_ALIAS:-ghostlock}"
KS_PASS="${KS_PASS:-ghostlock}"

echo "==> [1/7] 校验工具链"
for t in "$BT/aapt2" "$BT/d8" "$BT/zipalign" "$PLATFORM/android.jar"; do
  [ -f "$t" ] || { echo "ERR: 缺少 $t" >&2; exit 1; }
done
echo "  OK (SDK_DIR=$SDK_DIR)"

# --- 机型一致性防线（防 payload/runner/header 错配） ---
DEVICE_H="$ROOT_DIR/runner/devices_$DEVICE_ID.h"
[ -f "$DEVICE_H" ] || { echo "ERR: 机型头缺失 $DEVICE_H（gen_device_header.py 或 build_runner.sh 先行）" >&2; exit 1; }
[ -f "$ROOT_DIR/payload/$DEVICE_ID/MANIFEST.sha256" ] || { echo "ERR: payload/$DEVICE_ID 缺失，先运行 tools/make_payload.sh <pc_root> $DEVICE_ID" >&2; exit 1; }
if [ -f "$ROOT_DIR/build/last_device_id" ] && [ "$(cat "$ROOT_DIR/build/last_device_id")" != "$DEVICE_ID" ]; then
  echo "ERR: build/runner 是 $(cat "$ROOT_DIR/build/last_device_id") 编译的，与 $DEVICE_ID 不一致（重跑 runner/build_runner.sh <ndk> $DEVICE_ID）" >&2
  exit 1
fi
# payload 机型身份（device.json 来自 PC 版 devices/<id>/device.json，make_payload.sh 归集）
PAY_DEV="$ROOT_DIR/payload/$DEVICE_ID/device.json"
if [ -f "$PAY_DEV" ]; then
  PAY_RELEASE="$(python3 -c "import json,sys;print(json.load(open('$PAY_DEV')).get('kernel_release',''))" 2>/dev/null || true)"
  HDR_RELEASE="$(grep -A1 'DEV_KERNEL_RELEASE' "$DEVICE_H" | grep -oE '"[^"]+"' | tr -d '"' | head -1)"
  if [ -n "$PAY_RELEASE" ] && [ "$PAY_RELEASE" != "$HDR_RELEASE" ]; then
    echo "ERR: 机型身份错配 — payload/$DEVICE_ID/device.json 内核 '$PAY_RELEASE' ≠ devices_$DEVICE_ID.h '$HDR_RELEASE'" >&2
    exit 1
  fi
  echo "  机型一致性 OK: $DEVICE_ID ($PAY_RELEASE)"
else
  echo "  WARN: payload/$DEVICE_ID/device.json 缺失（建议重跑 make_payload.sh 以携带机型身份）"
fi

echo "==> [2/7] 组装 assets"
ASSETS="$BUILD/assets"
rm -rf "$BUILD"; mkdir -p "$ASSETS/www" "$BUILD/gen"
cp -r "$APP_SRC/assets/www/." "$ASSETS/www/"
printf '%s' "$DEVICE_ID" > "$ASSETS/device_id"   # MainActivity 据此解压对应 payload 子目录
# payload → assets/payload/<device_id>/（按 MANIFEST.sha256 白名单拷贝，杜绝历史备份/中间产物混入 APK）
if [ ! -d "$ROOT_DIR/payload/$DEVICE_ID" ]; then
  echo "ERR: payload 目录缺失，先运行 tools/make_payload.sh" >&2
  exit 1
fi
mkdir -p "$ASSETS/payload/$DEVICE_ID/modules"
cp -f "$ROOT_DIR/payload/$DEVICE_ID/MANIFEST.sha256" "$ASSETS/payload/$DEVICE_ID/"
awk 'NF==2{print $2}' "$ROOT_DIR/payload/$DEVICE_ID/MANIFEST.sha256" > "$BUILD/manifest_names.txt"
while read -r fname; do
  [ -n "$fname" ] || continue
  cp -f "$ROOT_DIR/payload/$DEVICE_ID/$fname" "$ASSETS/payload/$DEVICE_ID/$fname" \
    || { echo "ERR: 白名单拷贝失败 $fname" >&2; exit 1; }
done < "$BUILD/manifest_names.txt"
if [ -f "$ROOT_DIR/payload/$DEVICE_ID/device.json" ]; then
  cp -f "$ROOT_DIR/payload/$DEVICE_ID/device.json" "$ASSETS/payload/$DEVICE_ID/device.json"
fi
# runner 二进制 → assets/runner（Shizuku 模式运行时唯一来源：buildSyncScript
# unzip 到 /data/local/tmp/ghostboot 后 exec）。不再注入 lib/arm64-v8a/——
# app 域直跑已否决（untrusted_app 竞速结构性不可行），APK 内 native lib 是死重。
[ -f "$ROOT_DIR/build/runner" ] || { echo "ERR: build/runner 缺失，先跑 runner/build_runner.sh" >&2; exit 1; }
cp "$ROOT_DIR/build/runner" "$ASSETS/runner"
echo "  assets: $(du -sh "$ASSETS" | cut -f1)"

echo "==> [3/7] aapt2 compile + link"
AAPT2="$BT/aapt2"
RES="$BUILD/res"
rm -rf "$RES"; mkdir -p "$RES"
# 逐文件编译（vivo/标准 res 都适用）
find "$APP_SRC/res" -type f | while read -r f; do
  "$AAPT2" compile -o "$RES" "$f" 2>/dev/null || "$AAPT2" compile -o "$RES" "$f"
done
count=$(ls "$RES" 2>/dev/null | wc -l)
echo "  compiled: $count flat files"
[ "$count" -gt 0 ] || { echo "ERR: res 编译为空" >&2; exit 1; }

mkdir -p "$BUILD/gen"
"$AAPT2" link \
  -o "$BUILD/app.unaligned.apk" \
  -I "$PLATFORM/android.jar" \
  --manifest "$APP_SRC/AndroidManifest.xml" \
  --java "$BUILD/gen" \
  --min-sdk-version 26 \
  --target-sdk-version 35 \
  --version-code "$VERSION_CODE" \
  --version-name "$VERSION_NAME" \
  -A "$ASSETS" \
  -R "$RES"/*
echo "  linked: $(stat -c%s "$BUILD/app.unaligned.apk") bytes"


echo "==> [4/7] javac 编译 Java 源码"
# Shizuku 依赖（vendor/shizuku-api/*_extract/classes.jar）：api 引用 aidl，provider 独立
# 注意目录名：api artifact 解压为 aar_extract（非 api_extract）
# extract 目录不版本化：构建时从 .aar 自解压（clone 仓库后开箱可构建）
for AAR_SPEC in "aar_extract:api-13.1.5.aar" "aidl_extract:aidl-13.1.5.aar" "provider_extract:provider-13.1.5.aar"; do
  EXDIR="${AAR_SPEC%%:*}"; AARF="${AAR_SPEC##*:}"
  if [ ! -f "$ROOT_DIR/vendor/shizuku-api/$EXDIR/classes.jar" ]; then
    mkdir -p "$ROOT_DIR/vendor/shizuku-api/$EXDIR"
    unzip -o "$ROOT_DIR/vendor/shizuku-api/$AARF" classes.jar -d "$ROOT_DIR/vendor/shizuku-api/$EXDIR" >/dev/null 2>&1 \
      || echo "WARN: unzip $AARF 失败（缺 unzip？）"
  fi
done
SHIZUKU_CP="$ROOT_DIR/vendor/shizuku-api/aar_extract/classes.jar:$ROOT_DIR/vendor/shizuku-api/aidl_extract/classes.jar:$ROOT_DIR/vendor/shizuku-api/provider_extract/classes.jar"
javac --release 11 \
  -classpath "$PLATFORM/android.jar:$SHIZUKU_CP" \
  -d "$BUILD/classes" \
  $(find "$APP_SRC/java" -name '*.java') 2>&1
echo "  classes: $(find "$BUILD/classes" -name '*.class' | wc -l)"

echo "==> [5/7] d8 → classes.dex"
mkdir -p "$BUILD/dexout"
# d8 输入：class 文件 + Shizuku 3 个 classes.jar（aidl 接口须进 dex）
"$BT/d8" --release --lib "$PLATFORM/android.jar" \
  --min-api 26 \
  --output "$BUILD/dexout" \
  $(find "$BUILD/classes" -name '*.class') \
  "$ROOT_DIR/vendor/shizuku-api/aar_extract/classes.jar" \
  "$ROOT_DIR/vendor/shizuku-api/aidl_extract/classes.jar" \
  "$ROOT_DIR/vendor/shizuku-api/provider_extract/classes.jar"
cp "$BUILD/dexout/classes.dex" "$BUILD/classes.dex"
echo "  dex: $(stat -c%s "$BUILD/classes.dex") bytes"

echo "==> [6/7] 注入 classes.dex + zipalign"
"$BT/zipalign" -f 4 "$BUILD/app.unaligned.apk" "$BUILD/app.aligned.apk"
# 把 dex 加入 APK（用 zip 追加）
cd "$BUILD"
python3 - "$BUILD/app.aligned.apk" "$BUILD/classes.dex" <<'PY'
import sys, zipfile, shutil
apk, dex = sys.argv[1], sys.argv[2]
tmp = apk + ".tmp"
with zipfile.ZipFile(apk, 'r') as zin, zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        zout.writestr(item, zin.read(item.filename))
    zout.write(dex, "classes.dex")
shutil.move(tmp, apk)
print("  classes.dex injected")
PY
# 重新对齐（加 dex 后）
"$BT/zipalign" -f 4 "$BUILD/app.aligned.apk" "$BUILD/app.aligned2.apk"
mv "$BUILD/app.aligned2.apk" "$BUILD/app.aligned.apk"

echo "==> [7/7] apksigner 签名"
if [ ! -f "$KEYSTORE" ]; then
  echo "  生成开发 keystore: $KEYSTORE"
  keytool -genkeypair -v -keystore "$KEYSTORE" -alias "$KS_ALIAS" \
    -keyalg RSA -keysize 2048 -validity 10950 \
    -storepass "$KS_PASS" -keypass "$KS_PASS" \
    -dname "CN=GhostLock X200 App, OU=Local, O=GhostLock, C=CN"
fi
java -jar "$BT/lib/apksigner.jar" sign \
  --ks "$KEYSTORE" --ks-pass "pass:$KS_PASS" --key-pass "pass:$KS_PASS" \
  --out "$OUT_APK" "$BUILD/app.aligned.apk"

echo "==> 产物"
ls -la "$OUT_APK"
echo "  sha256: $(sha256sum "$OUT_APK" | cut -c1-64)"
echo "BUILD_APP_OK"
