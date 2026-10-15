# GhostLock X200 App — 架构设计

本仓库是一个 Android App，可在 vivo X200（PD2415，内核
6.6.89-android15-8-gb57af212129c-abogki457297774-4k）上完成 KernelSU root。
root 逻辑复用 ghostlock-x200-root（PC 版工具链，v1.3.5）已验证的提权链二进制：
glt_esync（permissive 写原语）、w2host（cred 泄漏）、ksud（KernelSU userspace）、
三个内核模块（clear_vr_tag / permissive_restore / kernelsu）。App 只承担 payload
解压、执行编排与状态展示，不重新实现提权逻辑。

与主项目解耦：主项目仅作为「资产供应商」，其二进制拷贝进 `payload/` 即可，本工程
的构建不调用主项目的任何构建脚本。

版本：v1.0。真机验证于 2026-08-30（Shizuku shell 域，ALL STAGES PASS）。

---

## 1. 模块边界

| 层 | 构成 | 职责 |
|---|---|---|
| app 层 | Java 单 Activity + WebView 前端 | Shizuku 授权引导、payload 解压、日志/状态展示 |
| runner 层 | libghostrunner.so（native 静态 PIE） | STAGE1–7 编排、门禁、ko patcher；执行于 shell 域 |
| payload 层 | assets/payload/<device_id>/ | 提权二进制（可替换） |
| tools/ | host 侧脚本 | payload 归集、runner/APK 构建 |

### 1.1 非目标
- 不支持 b57 之外的内核（须新 payload + offsets.json，不硬编码）。
- 不做持久 root（BL 未解锁，kprobe 临时提权重启失效，为物理约束）。
- 不改动主项目文件（只读拷贝其产物）。
- 不替代 KernelSU manager（ksud 落位后 su 授权由用户管理）。

---

## 2. 偏移模型

### 2.1 物理地址族（预存）
主链写目标全部使用 direct map（physmap）物理地址，不随 KASLR 变化。以下偏移在
真实设备 root 后测得，跨多次开机一致：

```
se_p0        = physmap_base + selinux_state_off       # STAGE1 写目标
w0           = physmap_base + kptr_restrict_off - 8   # STAGE2 写目标
GL_LOCK_BASE = physmap_base + zeroes_buf_off          # GL 槽位宿主（STAGE6 入参）
```

- `physmap_base = 0xffffff8000000000`
- `selinux_state_off = 0x2346ee8`、`kptr_restrict_off = 0x210bd20`、`zeroes_buf_off = 0x2342820`
- `cap_off = 56`（task_struct → cred 偏移）
- w2host 采样窗口：`cred_ip_offs = {0xcbc0c, 0xcbc10, 0xcbc14, 0xcbc18, 0xcbc1c, 0xcc5b0, 0xcc5d4}`、`win_lo = 0xcba0c`、`win_hi = 0xcc7d4`
- `W2_CAND_MIN = physmap_base + 0x40000000`

### 2.2 运行时求值项
| 项 | 来源 |
|---|---|
| kbase（_text 虚拟基址） | STAGE2 成功（kptr_restrict=0）后读 `/proc/kallsyms` 的 `_text` 行 |
| MemTotal | `/proc/meminfo`；用于 `W2_DIRECT_END = physmap_base + MemTotal` |
| capsym | STAGE2 后读 `/proc/kallsyms` 筛选；按 §6 的 `DEV_CAPSYM_WIN` 规则取候选 |

capsym（`init_completion.__key` 一类地址的低 32 位用作 cap 位掩码）随 KASLR 变化，
故不预存。筛选规则：遍历 kallsyms 中 `.*__key` 的 b/B 类型符号，取
`kbase <= addr < kbase + DEV_CAPSYM_WIN` 且低 16 位 bit 满足 cap 位要求的候选，
无候选时回退 kbase。

### 2.3 ko 重打
`patch_ko_all.py` 将内核符号绝对 VA 写入 ko 的 symtab `st_value`（这些符号非内核导出，
insmod 无法自行解析），绝对 VA = kbase + 符号相对偏移。runner 内置 `patcher.c`：
遍历 ELF symtab，UNDEF 符号置 `st_value += kbase`、`st_shndx = SHN_ABS`，并清空
`__versions` 段 CRC。其输出与 `patch_ko_all.py` 逐字节一致（clear_vr_tag：4 符号；
kernelsu：218 符号）。

---

## 3. 执行域

runner 由 App 通过 Shizuku 启动，执行于 shell 域。选型依据如下：

- untrusted_app 域中运行竞速链的实测结果：glt 竞速窗口不命中（写原语不落地，安全失败），
  且 App 域受限调度导致的大量 fork（mm_struct spray）触发内核 hung_task watchdog panic。
  两者为实测现象，非预期路径。
- shell 域（Shizuku 派生的 rish 进程 = uid 2000、`u:r:shell:s0`、Seccomp=0）与
  adb shell 等价，同参数跑同一 glt 二进制一轮即写原语落地（Permissive）。

配套参数约束：
- glt 由 r19c(clang 8.0.2) 交叉编译（prebuilt，3137376B）。竞速写原语的命中与时序强相关，
  编译器版本变化会改变时序，故重编维持 r19c。
- 首版暴露「手动按钮」；「开机自启」为独立开关，默认关。

---

## 4. STAGE 执行流程

runner 逐 STAGE 移植自 `root_full_permissive_restore.ps1`：

| STAGE | runner 函数 | 内容 / 判定 |
|---|---|---|
| 0 | `gate_kernel()` | 校验 `uname -r` 等于 b57 串 |
| 1 | `stage1_permissive()` | 若已 Permissive（或 enforce 可读且为 permissive）则短路跳过；否则 glt 竞速写 se_p0 |
| 2 | `stage2_kptr()` | 若 kptr_restrict 已为 0 则短路跳过；否则 leaf 形态写 w0 |
| 3 | `stage3_base()` | 读 `/proc/kallsyms` `_text` 得 kbase，校验前缀/对齐 |
| 4 | `stage4_quiesce()` / `stage4_leak()` | 负载静默门；启动 w2host perf 泄漏 cred |
| 5 | `stage5_capsroot()` | glt 写 `cred + cap_off`，`GL_INIT_CRED` = 现场 capsym，`GL_SLIDE_ONLY=1` |
| 6 | `stage6_insmod()` | patch 三 ko → insmod permissive_restore / clear_vr_tag / kernelsu；ksud 落位 `/data/adb/ksud` |
| 7 | `stage7_verify()` | `su` 返回 uid=0、三模块 /proc/modules 为 O、ksud 版本、网络 |

安全门：
- 开机 uptime < 240 s 时 STAGE1 前等待（避免开机风暴下竞速部分命中触发 panic）。
- STAGE1/2 短路：目标状态未满足时才运行写链。
- STAGE4 负载门：静默门参数来自 root.ps1 的 panic 防护逻辑。
- 竞速前与超时后清除 glt 残留进程，`exec_timeout` 以进程组为单位终止。

---

## 5. payload 资产与校验

| 资产 | 哈希（sha256 前缀） |
|---|---|
| glt_esync | 341074c4… |
| w2host | 2b8192b9… |
| ksud | 077797b6… |
| clear_vr_tag.ko | 08acf405… |
| kernelsu.ko | 187bb1bd… |
| permissive_restore.ko | 061133d0… |

`tools/make_payload.sh <ghostlock-x200-root 路径> [device_id= x200_b57]` 从主项目归集
上述产物并生成 MANIFEST.sha256。payload 目录不入库（.gitignore 忽略），镜像构建需先运行
该脚本。

---

## 6. 本机型硬编码表（`runner/devices_x200_b57.h`）

```c
#define DEV_PHYSMAP_BASE      0xffffff8000000000ULL
#define DEV_SELINUX_STATE_OFF 0x2346ee8ULL
#define DEV_KPTR_RESTRICT_OFF 0x210bd20ULL   // w0 = 0x210bd18（-8）
#define DEV_ZEROES_BUF_OFF    0x2342820ULL   // GL_LOCK_BASE
#define DEV_CAP_OFF           56
#define DEV_W2_CRED_IPS      {0xcbc0c,0xcbc10,0xcbc14,0xcbc18,0xcbc1c,0xcc5b0,0xcc5d4}
#define DEV_W2_IP_LO          0xcba0c
#define DEV_W2_IP_HI          0xcc7d4
#define DEV_W2_CAND_MIN_ADD   0x40000000ULL
#define DEV_KERNEL_RELEASE    "6.6.89-android15-8-gb57af212129c-abogki457297774-4k"
#define DEV_CAPSYM_WIN        0x4000000ULL
#define DEV_VR_TAG_A 6
#define DEV_VR_TAG_B 44
#define DEV_VR_TP    1024
#define DEV_GL_SLOT_SIZE 0x10   // slot index 1 的宿主页有毒，跳过
```

---

## 7. 构建链（均在本机离线执行）

| 环节 | 工具链 |
|---|---|
| runner | NDK r21e（`/root/android-ndk-r21e`），`runner/build_runner.sh`；产物 aarch64 静态 PIE |
| glt / w2host | NDK r19c（clang 8.0.2）；prebuilt 直接复用，重编仅在改源码时 |
| app Java | android.jar（android-35）+ javac `--release 11` |
| dex / 打包 | build-tools 36 d8 / zipalign / apksigner；d8 8.2.2 在 JDK25 上 NPE，须用 36 版 |
| 前端 | ui-preview（Vite + Vue3 + WinUIonWeb）→ `vite build --config vite.online2.config.mjs` → `tools/regen_www.py` |
| payload | `tools/make_payload.sh` |

```bash
bash runner/build_runner.sh
bash tools/make_payload.sh <ghostlock-x200-root 路径>
bash tools/build_app.sh /root/androidsdk
```

产物：`app/build/GhostLockX200-v1.1.0.apk`（dev key 自签）。

前端在线版从 `ui-preview/online/LiveGhostLockUI.vue` 经 vite 打包，`tools/regen_www.py`
将产物转为 WebView `file://` 可用形式（去除 `type="module"` 并改为 `defer`、重写资源
相对路径、确认无 `import.meta` / ES module 语法），写入 `app/src/main/assets/www/`。

---

## 8. 前端

前端为 WebView 加载的本地页面（`www`），样式基于 WinUIonWeb（WinUI 高清还原组件库）。
布局为 KernelSU Manager 式：状态大卡 + 2×2 功能入口（设备 / 执行阶段 / 日志 / 开机自启）
+ 安全状态条（SELinux / 内核 / Root）+ 底部全宽操作按钮与 InfoBar 汇总。

与 Java 层的通道：`GhostBridge`（addJavascriptInterface）暴露
`startRoot()`、`getEnv()`、`setBootAuto(boolean)`、`getBootAuto()`；Java 侧通过
`window.dispatchEvent(new CustomEvent(...))` 推送 `GhostLog` / `GhostStage` /
`GhostDone` / `GhostEnv` / `GhostShizuku` 事件。

---

## 9. 真机验证结果（2026-08-30，Shizuku shell 域）

一次完整运行（干净无 root 起始态）返回 ALL STAGES PASS：
STAGE1 → Permissive；STAGE2 → kptr_restrict=0；STAGE3 → kbase 解算（440405 符号）；
STAGE4 → w2host 泄漏 1 候选；STAGE5 → CAPSROOT（round 2）；STAGE6 三 ko patch + insmod +
ksud 落位 4556352 B；STAGE7 → `su` uid=0（context `u:r:ksu:s0`）、三模块为 O、
ksud 版本 32525、网络 OK。设备侧确认 SELinux=Permissive。

已达成 BIAS 态（Permissive + 三模块 Live）后重跑同一链：STAGE1/2 触发短路跳过写链，
直接走 STAGE3 起，无 panic。

---

## 10. 目录结构

```
ghostlock-x200-app/
├── README.md / LICENSE / NOTICE
├── app/                 Android 壳（Java + WinUI WebView；assets/www 随仓库分发）
├── runner/              STAGE 编排器（C）+ devices_x200_b57.h + 构建脚本
├── exploit/             glt_esync / w2host 源码副本（含修复；主项目不随本仓改动）
├── ui-preview/          WinUI 前端预览工作台（Vite + Vue3 + WinUIonWeb）
├── payload/             二进制资产（不入库，make_payload.sh 组装）
├── tools/               make_payload.sh / build_app.sh / regen_www.py
├── vendor/shizuku-api/  Shizuku API（构建时自解压）
└── docs/                DESIGN.md（本文档）
```