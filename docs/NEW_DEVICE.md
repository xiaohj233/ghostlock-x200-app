# 新机型接入手册（device 替换流程）

本仓库与 PC 版（ghostlock-x200-root）解耦，但换机型时**构建期联动**是完整闭环：
PC 版负责"按机型出资产与偏移"，本仓库负责"按机型组 APK"。App 运行时逻辑
（runner/glt/w2host）不做任何机型判断——机型只体现在**编译期硬编码的偏移表**
与 **payload 资产目录**。

```
PC 版（ghostlock-x200-root）                   本仓库（ghostlock-x200-app）
─────────────────────────────                  ─────────────────────────────
boot.img / kallsyms / BTF
   │ offsets_auto.py feasibility        ┌─> tools/gen_device_header.py
   │ offline / winoffs-image / header   │      (win_offs.json + device.json
   │ derive-pselect / package           │       → runner/devices_<id>.h)
   └──> devices/<id>/ ──────────────────┘
         ├ target.h  ──> build_glt_esync.sh -t <id> ──> prebuilt/glt_esync
         ├ device.json/win_offs.json ──> (本仓库归集)
         └ manifest.json（verify-device）
                                                      1) tools/make_payload.sh <pc_root> <id>
                                                      2) runner/build_runner.sh <ndk> <id>
                                                      3) tools/build_app.sh <sdk> <id>
                                                      └──> app/build/GhostLockX200-*.apk
```

## 0. 分工与机型无关资产

| 资产 | 是否按机型 | 说明 |
|---|---|---|
| glt_esync | **按机型** | PC 侧 `build_glt_esync.sh -t <id>` 用 `devices/<id>/target.h` 编译；**必须 r19c(clang8)**，编译器版本决定竞速时序 |
| w2host | 否 | 采样窗口/候选范围运行时 W2_* 注入；本仓库 `exploit/` 内置 ring64MB 修复版 |
| ksud | 否 | 官方 KernelSU userspace |
| kernelsu.ko | **按内核 release** | 官方 GKI ko + PC 侧 `modules/kernelsu/patch_vermagic.py` 重打 vermagic；runner patcher 再清 `__versions` |
| clear_vr_tag.ko / permissive_restore.ko | 同 6.6 分支可用 | 偏移由 runner 运行期 patch（ABS 重定位）；跨分支需 PC 侧重编 |
| runner | **按机型（编译期）** | 偏移表 `devices_<id>.h` 硬编码（本仓库） |
| 前端 / Java 壳 | 否 | `assets/device_id` 由 build_app.sh 写入，MainActivity 按它解压 payload |

## 1. PC 侧：生成机型模块

在 ghostlock-x200-root 仓库（Kali 优先）：

```bash
# 1a. 可行性预检（可选但建议）
python3 tools/offset_tools/offsets_auto.py feasibility <boot.img> \
    --kernel-release "<目标内核 release>" --soc dimensity

# 1b. 离线恢复 kallsyms + BTF（只要 boot.img）
python3 tools/offset_tools/offsets_auto.py offline <boot.img> \
    --out-dir /tmp/off --branch auto

# 1c. 镜像固有偏移（physmap + sym_offs + w2host 窗口）
python3 tools/offset_tools/offsets_auto.py winoffs-image <boot.img> \
    /tmp/off/kallsyms.txt /tmp/win_offs.json --task-cred-off 0x820

# 1d. 编译期全量偏移头（target.h，供 glt 编译）
python3 tools/offset_tools/offsets_auto.py header /tmp/off/kallsyms.txt \
    /tmp/off/vmlinux.btf -o /tmp/target.h

# 1e. pselect 栈布局（waiter word shift）
python3 tools/offset_tools/offsets_auto.py derive-pselect <boot.img> \
    --kallsyms /tmp/off/kallsyms.txt --btf /tmp/off/vmlinux.btf -o /tmp/pselect.json

# 1f. 整合为可分享机型模块 devices/<id>/
python3 tools/offset_tools/offsets_auto.py package <boot.img> \
    --out devices/<id> --device-id <id> --soc dimensity --family <family>

# 1g. 编译按机型的 glt（必须 r19c）
bash exploit/build/build_glt_esync.sh -o prebuilt -n <ndk-r19c-llvm-bin> -t <id>
cp prebuilt/glt_esync devices/<id>/prebuilt/glt_esync

# 1h. kernelsu.ko vermagic 重打到目标 release
python3 modules/kernelsu/patch_vermagic.py <官方 GKI kernelsu.ko> "<目标 release>"

# 1i. 完整性校验
python3 tools/offset_tools/offsets_auto.py verify-device <id>
```

## 2. 本仓库侧：三步组装

```bash
# 2a. 自动生成 app 侧偏移头（PC 版 win_offs.json + device.json → devices_<id>.h）
python3 tools/gen_device_header.py <ghostlock-x200-root 路径> <id>

# 2b. 登记机型（唯一手工步骤）：runner/devices.h 追加一行
#     #elif defined(DEVICE_<ID>)            ← id 大写化、连字符转下划线
#       #include "devices_<id>.h"

# 2c. 归集 payload（拷贝 PC 版 6 产物 + 机型身份 device.json + MANIFEST）
bash tools/make_payload.sh <ghostlock-x200-root 路径> <id>

# 2d. 编译 runner（NDK r21e；-DDEVICE_<ID> 选偏移表）
bash runner/build_runner.sh /root/android-ndk-r21e <id>

# 2e. 打 APK（构建前自动执行一致性校验，见下）
bash tools/build_app.sh /root/androidsdk <id>
```

## 3. 字段映射（PC 版 json → app 侧 header）

`tools/gen_device_header.py` 自动完成，映射如下：

| app header 宏 | PC 版来源 |
|---|---|
| `DEV_KERNEL_RELEASE` | device.json.kernel_release |
| `DEV_PHYSMAP_BASE` | device.json.physmap_base（回退 win_offs.json） |
| `DEV_SELINUX_STATE_OFF` | win_offs.json.sym_offs.selinux_state（回退 offsets.json selinux_enforcing_p0 − physmap） |
| `DEV_KPTR_RESTRICT_OFF` | win_offs.json.sym_offs.kptr_restrict |
| `DEV_ZEROES_BUF_OFF` | win_offs.json.sym_offs.zeroes_buf |
| `DEV_CAP_OFF` | offsets.json.struct_offsets.cred_cap_permitted（缺省 56） |
| `DEV_W2_IP_LO / HI / CRED_IPS` | win_offs.json.w2host.{win_lo,win_hi,cred_ip_offs}（回退 offsets.json.w2host） |
| `DEV_W2_CAND_MIN_ADD` | device.json.w2_cand_min − physmap_base（缺省 0x40000000） |
| `DEV_VR_TAG_A/B/TP` | 模块 vr_offsets.json（缺省 6/44/1024） |
| `DEV_GL_SLOT_SIZE / BAD` | 固定 0x10 / 1（设计值，跨机型） |
| `DEV_CAPSYM_WIN` | 固定 0x4000000（offsets_auto.py 筛选规则） |

## 4. 构建期一致性防线

`tools/build_app.sh` 在 [1/7] 拒绝错配：

1. `payload/<id>/MANIFEST.sha256` 必须存在（未跑 make_payload.sh 则 ERR）；
2. `runner/devices_<id>.h` 必须存在（未生成则 ERR）；
3. `build/last_device_id` 必须等于 `<id>`（runner 不是该机型编译的则 ERR）；
4. `payload/<id>/device.json.kernel_release` 必须等于 header 的 `DEV_KERNEL_RELEASE`
   （device.json 缺失仅 WARN；存在但内核串不同则 ERR——典型错配：换了 payload
   没换 header，或反之）。

## 5. 真机验证清单

```bash
# 安装（vivo 弹窗流程）与授权 Shizuku 后点「启动 Root」：
#   STAGE1 Permissive → STAGE2 kptr=0 → STAGE3 kbase → STAGE4 cred 泄漏
#   → STAGE5 CAPSROOT → STAGE6 三 ko insmod + ksud 落位 → STAGE7 su uid=0
adb shell su -c id          # 期望 uid=0 (root) context=u:r:ksu:s0
adb shell cat /proc/modules | grep -E 'kernelsu|permissive_restore|clear_vr_tag'  # 期望 O
adb shell getenforce        # 期望 Permissive
```

注意：跨内核分支（非 6.6）时 clear_vr_tag/permissive_restore 需在 PC 侧按新
kallsyms 重编；kernel_release 串必须与设备 `uname -r` 完全一致（build_app 校验
的是 header 与 payload 一致，真机 gate 校验的是 header 与设备一致）。
