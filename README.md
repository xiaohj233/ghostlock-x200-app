<!--
SPDX-FileCopyrightText: 2026 xiaohj233
SPDX-License-Identifier: Apache-2.0
-->
# GhostLock X200 (App)

当前发行版：**1.1.0**。[下载发行版](https://github.com/xiaohj233/ghostlock-x200-app/releases/tag/v1.1.0) · [更新日志](CHANGELOG.md)

针对 **vivo X200**、基于 GhostLock（CVE-2026-43499）内核漏洞的**设备端一键 root App**：不刷机、不解锁、不清数据，运行后获得**临时 root**，重启后失效，重新打开 App 一键再 root。

这是 [ghostlock-x200-root](https://github.com/xiaohj233/ghostlock-x200-root)（PC 版工具链）配套的手机端入口，共用同一套提权链。通过 [Shizuku](https://github.com/RikkaApps/Shizuku)（支持无线调试自举）授权后即可使用，**无需电脑 adb**。

> **法律与安全声明（请务必先阅读）**
>
> 本仓库仅用于**安全研究与教育目的**，并且**只允许在你自己拥有、或已获得明确
> 书面授权的设备上使用**。未经授权对他人设备使用本工具，可能违反当地法律法规。
> 使用本工具可能导致设备异常重启、数据损坏、系统异常、保修失效等后果，请自行
> 评估风险。**作者不对任何直接或间接损害承担责任，亦不对任何滥用行为负责。**
> 继续阅读或使用本仓库，即表示你已理解并同意自行承担全部风险与责任。

## 使用

1. 安装并启动 [Shizuku](https://github.com/RikkaApps/Shizuku)（Android 16 需 GitHub 版 13.6.0+）
2. 打开本 App，按引导授予 Shell 权限
3. 点击「启动 Root」，等待各阶段完成（约 1-2 分钟）
   支持自动识别已安装的 KernelSU / SukiSU / ReSukiSU，并使用该版本的管理器加载程序；
   多个管理器时选择其中一个。详情见 [管理器加载与测试](docs/MANAGER_LOADING.md)。
4. 验证：终端执行 `su -c id`，应输出 `uid=0(root)`
5. 重启后 root 失效，重复步骤 1-3

注意：**开机后请先等约 4 分钟再运行**；root 过程中可能触发内核重启（可自动恢复，无变砖风险）。

1.1.0 已知问题：首次完成后，第 9 步可能等待较久并显示验证未通过，但实际功能可能已经可用。
当前仍保留约 120 秒验证上限，后续将优化结果检测与提示；若实际功能已正常，请先检查管理器状态，避免重复执行完整流程。

## 构建

需要 [ghostlock-x200-root](https://github.com/xiaohj233/ghostlock-x200-root)（v1.3.5）提供提权链资产，以及 NDK r21e、Android SDK build-tools 36 / android-35、JDK 11。

```bash
bash runner/build_runner.sh                                # 编译 runner
bash tools/make_payload.sh <ghostlock-x200-root 路径>      # 组装提权链资产
bash tools/build_app.sh /root/androidsdk                   # 产出 APK
```

前端产物（`app/src/main/assets/www`）已随仓库分发；重建二进制源文件（runner）前需先编译，详见 `docs/DESIGN.md` 与 `docs/NEW_DEVICE.md`。

> 双仓库依赖：本仓库的 `tools/make_payload.sh` 编译提权链资产时需要
> [ghostlock-x200-root](https://github.com/xiaohj233/ghostlock-x200-root) 的
> `prebuilt/`（glt_esync / ksud / w2host）与 `modules/*.ko`。必须先 clone 主项目，
> 再以主项目根目录作为参数运行 `make_payload.sh`，否则 `build_app.sh` 会在第一步报
> `ERR: payload 缺失`。

底层原理、设备适配范围等细节见 PC 版仓库文档：[架构](https://github.com/xiaohj233/ghostlock-x200-root/tree/v1.3.5/docs/ARCHITECTURE.zh-CN.md)、[使用说明](https://github.com/xiaohj233/ghostlock-x200-root/tree/v1.3.5/docs/USAGE.zh-CN.md)；本仓库细节见 `docs/DESIGN.md`。

## 目录结构

```
app/      Android App（Java 壳 + Shizuku 集成）
runner/   提权链编排器（C）
exploit/  提权链原生程序源码
tools/    构建脚本
vendor/   Shizuku API 依赖（AAR，构建时自解压）
```

## 许可证与来源

采用 Apache-2.0（[LICENSE](LICENSE)，版权署名 **xiaohj233**）。提权链资产与源码源自 [ghostlock-x200-root](https://github.com/xiaohj233/ghostlock-x200-root)（含 IonStack/KernelSU 等第三方代码，完整来源见 [NOTICE](NOTICE)）。

## 致谢

- [NebuSec / CyberMeowfia](https://github.com/NebuSec/CyberMeowfia) — IonStack（CVE-2026-43499）
- [tiann/KernelSU](https://github.com/tiann/KernelSU) — KernelSU
- [RikkaApps/Shizuku](https://github.com/RikkaApps/Shizuku) — shell 域执行
- [Furry-Xiyi/WinUIonWeb](https://github.com/Furry-Xiyi/WinUIonWeb) — WinUI 前端组件
