# ProcessNetMonitor - TrafficMonitor 插件

TrafficMonitor 的插件，显示每个进程的网络速度。

## 效果预览

![任务栏显示](docs/screenshot-main.jpg)

![悬浮信息窗口](docs/screenshot-popup.jpg)

![详情窗口](docs/screenshot-detail.jpg)

## 功能

- **任务栏**：两行显示，Upload 和 Download 各一行
  ```
  U:mihomo-alp. 5.6KB/s
  D:mihomo-alp. 1.4KB/s
  ```
- **悬浮信息窗口**：鼠标悬停主悬浮窗时弹出带进程图标的详情窗口
  - 进程图标 + 名称 + 上传/下载速度
  - 深色/浅色主题跟随系统设置
  - 上传/下载各显示 Top 5 进程
  - 空闲进程按 EMA（指数移动平均）排序，展示近期活跃进程
  - 圆角窗口，DWM 阴影
- **详情窗口**（点击插件区域打开）：
  - 实时流量：可排序表格，显示进程名 / 类别 / 下载速度 / 上传速度 / 连接数
  - 历史流量：按时间段（24小时/3天/7天/30天）统计每进程总流量和平均速度
  - 展开行：点击进程名展开显示 PID 和完整路径
  - 右键菜单：定位文件 / 文件属性 / 结束进程 / 查看连接
  - 历史数据持久化：关闭重启后历史数据不丢失
- **Tooltip**：保留 TM 默认文本 tooltip 作为备用
- **不显示系统速度**：系统速度由 TrafficMonitor 本身提供，插件只显示进程级信息

## 编译环境

- **编译器**：MSVC (Visual Studio 2022 BuildTools)
- **SDK**：Windows SDK 10.0.26100.0
- **C++ 标准**：C++17
- **依赖库**：iphlpapi.lib, ws2_32.lib, gdi32.lib, user32.lib, shell32.lib, dwmapi.lib, advapi32.lib

## 编译部署

```bat
build.bat
```

**注意**：编译前需先关闭 TrafficMonitor，否则 DLL 无法覆盖。build.bat 会自动尝试 `taskkill`，如果 TM 以管理员权限运行则需要手动退出。

## 文件结构

```
ProcessNetMonitor/
├── build.bat                    # 编译+部署脚本
├── plugin/
│   ├── src/
│   │   ├── PluginInterface.h    # TM 插件接口定义
│   │   ├── capture.h / .cpp     # 网络抓包（GetIfTable2）
│   │   ├── plugin_main.h / .cpp # 插件主体（2个item：Up/Down）
│   │   ├── tooltip_popup.h/.cpp # 富文本悬浮信息窗口
│   │   ├── detail_window.h/.cpp # 火绒风格详情窗口
│   ├── ProcessNetMonitor.dll    # 编译输出
│   └── Makefile
├── TrafficMonitor/
│   ├── TrafficMonitor/          # TM 主程序
│   │   ├── plugins/             # 插件DLL放这里
│   │   └── skins/
│   └── plugins/                 # 备份DLL位置
└── README.md
```

## TM 配置

1. 打开 TM → 右键 → 选项 → 插件
2. 勾选 "Up" 和 "Down" 两个显示项目
3. 在任务栏设置中将两项都添加到任务栏显示

**推荐设置**：选项 → 主窗口设置 → 取消勾选「显示鼠标提示」，否则主悬浮窗鼠标悬停时会同时出现两个信息窗口（TM 自带的文本提示 + 本插件的富文本弹窗）

## 版本历史

### v1.4.0 (2026-07-06)
- 历史流量功能完善：数据持久化，关闭重启后不丢失
- 历史数据改用墙钟时间戳（不再依赖系统运行时长），重启电脑后数据依然有效
- 存储格式改为增量记录（每次采样的实际传输量），彻底解决 TM 重启后累计值归零导致数据清零的问题
- 展开行状态修复：历史标签页切换时间范围或数据刷新时，已展开的行不再自动折叠
- 自动保存间隔从 60 秒缩短到 30 秒
- 数据文件版本升级至 v4，自动兼容旧格式（v2/v3）

### v1.3.0 (2026-07-05)
- 新增「详情窗口」：点击插件区域打开火绒风格的全功能流量监控窗口
- 可排序表格：程序名称 / 程序类别 / 下载速度 / 上传速度 / 连接数
- 进程图标 + 进程名 + .exe 自动去除
- 展开行：显示进程 ID 和完整路径
- 右键菜单：定位文件 / 文件属性 / 结束进程
- 标题栏可拖动，Esc 关闭
- 实时/历史标签页（UI 骨架，历史功能待实现）
- 连接数统计：从 TCP 连接表实时统计每进程活跃连接数
- 深色/浅色主题跟随系统

### v1.2.0 (2026-07-05)
- 新增 Rich Tooltip Popup：鼠标悬停主悬浮窗时弹出带进程图标的详情窗口
- 深色/浅色主题跟随系统设置
- 上传/下载各显示 Top 5 进程（不够时用历史进程补位）
- EMA 指数移动平均排序（alpha=0.3，~30 秒衰减窗口）
- 进程图标缓存（SHGetFileInfo + ExtractAssociatedIcon）
- 分层窗口 + DWM 圆角 + SetWindowRgn 裁黑角
- 智能 hover 检测：GetAncestor(GA_ROOT) + 任务栏位置判断，区分主悬浮窗和任务栏窗口
- 自适应高度（根据实际进程数调整）
- hover 检测频率优化：显示时 80ms，隐藏时 300ms

### v1.1.0 (2026-07-05)
- 从单 item (CustomDraw) 改为双 item（Up/Down 独立显示）
- 去掉任务栏系统速度（TM 自带）
- 去掉任务栏标签文字
- 恢复 tooltip 完整进程列表

### v1.0.0
- 初始版本，单 item CustomDraw 模式
- 支持按进程统计网络速度（Upload/Download Top 5）
