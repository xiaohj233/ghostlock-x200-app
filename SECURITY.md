# Security Policy

## 安全说明

GhostLock X200 是**内核漏洞利用工具**，用于在用户自有设备上获取 root 权限进行安全研究。请遵守以下边界：

- 仅在你**自己拥有**的设备上使用
- 不用于任何未授权设备、他人设备或生产环境
- 工具本身可能触发内核 panic（设备自动重启恢复）；存在极端情况下不可恢复的风险，使用者自行承担

## 报告漏洞

本项目不收集任何用户数据，不需要隐私报告通道。若你发现本仓库代码（runner / app / exploit）存在安全问题，请：

- **不要公开披露**可利用的内核漏洞细节（0-day）
- 通过 GitHub Security Advisory（私有披露）或仓库 Issues 联系维护者
- 附上：影响的阶段/文件、复现条件、影响范围

## 支持的设备

- vivo X200（PD2415），内核 `6.6.89-android15-8-gb57af212129c-abogki457297774-4k`
- 其他机型需要重新提取偏移（见 ghostlock-x200-root 的 `offsets_auto.py package` 流程）并重编 payload
