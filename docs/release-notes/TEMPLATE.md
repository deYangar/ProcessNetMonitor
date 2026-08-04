# Release Note 模板（ProcessNetMonitor）

> **规则**：本项目每个版本的 Release Note 都必须包含下面三个标准段落
> （版本说明 / 安装前提 / 已知说明），版本号替换为实际版本。
> 顶部再写本版本特有的「新增 / 修复 / 变更」，然后接这三段。

---

# ProcessNetMonitor v{version} — <一句话主题>

<本版本特有的新增 / 修复 / 变更说明>

## 版本说明（2 个产物）

| 文件 | 说明 |
|---|---|
| `ProcessNetMonitor.dll` | x64（64 位 TrafficMonitor 使用） |
| `ProcessNetMonitor_x86.dll` | x86（32 位 TrafficMonitor 使用） |

选择与你的 TrafficMonitor 主程序位数一致的 DLL，放入 TM 的 `plugins/` 目录（或插件管理界面安装）。

## 安装前提

1. Windows 10/11（推荐 64 位）
2. TrafficMonitor 主程序（插件版）
3. 插件以管理员权限运行时可启用 ETW 内核采集（更准）；普通权限自动回退传统采集并提示

## 已知说明

- TUN 代理模式下，走代理的应用流量归属代理进程（如 mihomo），应用侧 TUN 重复流量不显示（与 AppNetworkCounter 一致）
- 代理进程与应用之间的虚拟转发流量（TUN 用户态）无法通过内核 ETW 观测，不显示
- 与 AppNetworkCounter 等 ETW 工具共用时：先启动的一方占用 NT Kernel Logger；后启动的 TM 进入附加模式（悬浮窗有黄色警告提示）
- ETW 占用者检测为尽力而为（Windows 无官方 API，普通权限下为已知工具名单识别）
- 历史流量如有旧版本残留的虚高数据，请在详情窗口点击「清除历史」重置
