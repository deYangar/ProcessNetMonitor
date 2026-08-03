# YOLO Plan: 破解 AppNetworkCounter 的 ETW 机制，实现准确的进程级流量监控

## 约束
- 工作目录: C:\Users\Yang\.openclaw\workspace\projects\ProcessNetMonitor
- Git 分支: master (clean at 401c9b6)
- 环境: Win11 25H2 + mihomo TUN 代理; TM 以管理员运行(插件有 admin 上下文)

## 任务清单
- [x] 1. AppNetworkCounter 二进制静态分析（API + GUID）→ 纯标准 ETW，无魔法
- [x] 2. ETW 会话侦查（logman 全量列表 + provider/keywords）
- [x] 3. etw_diag 诊断模块：双会话消费 + TDH 解析 + 原始字节 dump → 证明 size 字段有数据
- [x] 4. 集成进插件 + 编译 + 部署（诊断版）
- [x] 5. 对比实验：per-PID 字节数完整拿到（mihomo 126MB 等），推翻"TUN 下 size=0"
- [x] 6. 正式 ETW 后端 etw_capture：attach/start 自动切换 + TDH 分类 + 速度输出 + legacy 合并
- [x] 7a. x64 编译部署（TM 01:17 运行中，无崩溃）
- [ ] 7b. 咩咩同屏实测对比 ANC 数字
- [ ] 8. x86 编译、清理诊断代码、提交、发版（等咩咩确认）

## 验收标准
- 插件按进程显示准确字节数（与 ANC 交叉验证）
- TUN 模式下 mihomo/浏览器流量都归属正确进程
- 现有 UI/详情窗/历史记录不回归

## 关键结论
见 memory/2026-08-04.md：size 在 offset 4 一直有效，ANC 无魔法，昨天是自己消费者的 bug。
