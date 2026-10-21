# ProcessNetMonitor 项目规则

## 构建铁律

- **任何产出 DLL 的构建，链接参数必须显式 `/OPT:REF`**（2026-10-21 咩咩拍板「以后都这样」）。
  原因：`/DEBUG`（或 `/Zi` 编译触发链接器默认 /DEBUG）下链接器默认 NOREF，
  死代码全留在产物里，实测 x64 从 678KB 膨胀到 2.02MB（3 倍）。
  build.bat / build_debug.bat / build_release.bat 三者已统一带上。
- CRT 保持 `/MT` 静态链接（插件零依赖分发原则）。`/MD` 实测可到 472KB
  但引入 vcruntime140.dll 依赖，已否决；`/OPT:ICF` 实测零增益，不加。
