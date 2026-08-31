#!/bin/bash
# 真机安装 + vivo 弹窗自动化（发布前冒烟用）
# 用法: bash tools/install.sh [apk路径]  默认 app/build/GhostLockX200-v0.1.0.apk
# 注意: adb 是 Windows 程序，APK 路径必须转 D:\ 反斜杠格式
set -u
ADB='/mnt/c/Program Files/platform-tools/adb.exe'
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APK="${1:-$ROOT/app/build/GhostLockX200-v0.1.0.apk}"
# /mnt/d/... -> D:\... （Windows adb 不认 /mnt 路径）
APK_WIN=$(echo "$APK" | sed 's|^/mnt/\([a-zA-Z]\)/|\1:/|; s|/|\\|g')
PKG='com.ghostlock.x200.app'

center() { # 从 uiautomator bounds 提取中心点
  echo "$1" | grep -oE '[0-9]+,[0-9]+' | head -1 | tr ',' ' '
}
tap() { "$ADB" shell input tap "$1" "$2"; }

echo "==> 安装 $APK_WIN"
("$ADB" install -r -g "$APK_WIN" >/tmp/install_ghost.log 2>&1 &)
# 轮询 vivo PackageInterceptActivity 弹窗（最长 120s）
for i in $(seq 1 60); do
  focus=$("$ADB" shell dumpsys window 2>/dev/null | grep mCurrentFocus | tr -d '\r')
  if echo "$focus" | grep -qi 'packageinteractor\|packageinstaller'; then
    echo "    弹窗出现 ($((i*2))s): $focus"; break
  fi
  sleep 2
done
# 勾选风险确认 checkbox（循环到 checked=true，最多 5 次）
for i in $(seq 1 5); do
  "$ADB" shell uiautomator dump /sdcard/ui.xml >/dev/null 2>&1
  xml=$("$ADB" shell cat /sdcard/ui.xml 2>/dev/null | tr -d '\r')
  line=$(echo "$xml" | grep -o '<node[^>]*deleted_file_state_cb[^>]*>' | head -1)
  if echo "$line" | grep -q 'checked="true"'; then echo "    checkbox 已勾选"; break; fi
  b=$(echo "$line" | grep -o 'bounds="\[[0-9]*,[0-9]*\]\[[0-9]*,[0-9]*\]"' | head -1)
  if [ -n "$b" ]; then
    set -- $(center "$b"); tap "$1" "$2"; echo "    tap checkbox ($1,$2)"; sleep 1
  else
    echo "    checkbox 未找到，继续等"; sleep 3
  fi
done
# 点「继续安装」（最多 5 次轮询）
for i in $(seq 1 5); do
  "$ADB" shell uiautomator dump /sdcard/ui.xml >/dev/null 2>&1
  xml=$("$ADB" shell cat /sdcard/ui.xml 2>/dev/null | tr -d '\r')
  line=$(echo "$xml" | grep -o '<node[^>]*vbutton_title[^>]*>' | head -1)
  if [ -n "$line" ]; then
    b=$(echo "$line" | grep -o 'bounds="\[[0-9]*,[0-9]*\]\[[0-9]*,[0-9]*\]"' | head -1)
    set -- $(center "$b"); tap "$1" "$2"
    echo "    已点继续安装 ($1,$2)"; break
  fi
  sleep 2
done
# 等装完（lastUpdateTime 变化）
for i in $(seq 1 30); do
  lt=$("$ADB" shell dumpsys package $PKG 2>/dev/null | grep lastUpdateTime | tr -d '\r')
  sleep 3
  if "$ADB" shell pm path $PKG >/dev/null 2>&1; then
    echo "==> 安装完成: $lt"
    break
  fi
done
# vHidden 防护（重启后 vivo 隐藏 app）
"$ADB" shell pm enable --user 0 $PKG >/dev/null 2>&1
echo "==> done"
