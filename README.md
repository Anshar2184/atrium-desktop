<div align="center">

# Atrium Desktop

**面向 Windows 11 的原生玻璃桌面环境**

实时折射的悬浮任务栏 · 统一材质的桌面小组件 · 保留 Explorer，崩溃可恢复

[![build](https://github.com/wynter-yaxley/atrium-desktop/actions/workflows/build.yml/badge.svg)](https://github.com/wynter-yaxley/atrium-desktop/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform: Windows 11](https://img.shields.io/badge/platform-Windows%2011-0078D6)](#当前状态)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](native/)
[![WinUI 3 settings](https://img.shields.io/badge/settings-WinUI%203%20%C2%B7%20.NET%2010-512BD4?logo=dotnet&logoColor=white)](settings/)
[![Status](https://img.shields.io/badge/status-prototype-orange)](docs/VERIFICATION.md)

简体中文 | [English](README_EN.md)

</div>

> [!NOTE]
> **v0.1 开发版。** 可以运行的第一个切片：一条与系统任务栏共存的 C++ 玻璃栏（时钟与日期）和一个可实时调节外观的设置中心。它不会隐藏或替换系统任务栏；完整替换还要等恢复与托盘在稳定版 Windows 11 上通过验证。

<p align="center">
  <img src="docs/images/v01-bar-liquid.png" alt="液态玻璃预设" width="80%"><br>
  <img src="docs/images/v01-bar-frosted.png" alt="磨砂玻璃预设" width="80%"><br>
  <sub>v0.1 的 Atrium.Core 实机画面：液态与磨砂两种预设（背景为测试图案）</sub>
</p>

<p align="center">
  <img src="docs/images/v01-settings.png" alt="设置中心" width="60%"><br>
  <sub>设置中心：拖动滑块时玻璃栏实时预览，保存后才写入设置</sub>
</p>

## 它要做什么

Atrium 重新设计你每天看到的那一层：一条悬浮的玻璃任务栏和一套桌面小组件，共享同一种材质和设计语言。Explorer 和 Windows Shell 保持不动，开始菜单、搜索、通知中心和快捷设置照常使用。

- **一套材质引擎**：大圆角与胶囊形状、边缘折射、适度色散、GPU 模糊、柔和高光；液态和毛玻璃只是同一管线的参数预设。
- **原生、轻量**：C++20、Win32、Direct3D 11、DirectComposition、DirectWrite；不用浏览器内核，不装服务，不需要管理员权限。
- **能长期日常使用**：独立的原生守护进程在崩溃、卡死时恢复系统任务栏；不向 Explorer 或 DWM 注入代码，不修改系统文件。

## 快速开始

1. 从 [Actions](https://github.com/wynter-yaxley/atrium-desktop/actions/workflows/build.yml) 最新一次成功构建下载 `Atrium-win-x64` 构件，或按下文自行构建。
2. 解压后运行 `Atrium.Core.exe`。屏幕底部出现玻璃栏，托盘区出现 Atrium 图标。
3. 双击托盘图标（或右键 →“外观设置…”，或右键玻璃栏）打开设置中心，调整透明度、模糊、折射、圆角、尺寸等。
4. 托盘右键 →“退出 Atrium”即可完全退出；不修改注册表，不安装服务，设置保存在 `%LOCALAPPDATA%\Atrium`。

## 当前状态

验证都在一台 Windows 11 26300 预览版、核显 + RTX 4060 的笔记本上执行。完整数据见 [docs/VERIFICATION.md](docs/VERIFICATION.md)，当前有效决策见 [docs/DECISIONS.md](docs/DECISIONS.md)。

| 方向 | 结论 |
|---|---|
| v0.1 玻璃栏与设置中心 | ✅ 共存模式；背景每帧变化时每帧工作 p95 1.7 ms、内存 126 MB；设置端到端 9 项与协议健壮性 7 项通过 |
| 实时玻璃 | ✅ WGC 捕获 + 排除自身 + D3D11；核显每帧 p95 2.4 ms，画面滞后 p95 1 帧 |
| 自回声 | ✅ 与最近一次呈现的背景精确比较；缓慢渐变、1 像素光标、低对比滚动文字都不会漏画 |
| 点击穿透 | ✅ 可点穿的显示窗口 + 带形状的输入窗口，阴影和透明角都能点到下方窗口 |
| 任务栏接管与恢复 | ✅ 接管协议 v2 + 原生守护进程（含真实 AppBar）：强杀、卡死、启动停顿、双进程同时终止、锁超时、恢复未生效等 15 个故障场景都恢复到用户原设置（单屏；Explorer 真实重启未测） |
| 系统入口 | ✅ 系统任务栏隐藏后，开始、搜索、快捷设置、通知中心仍可正常使用 |
| 第三方托盘 | ⚠️ 转发式桥接的正常与失败路径都已验证；Explorer 重启、真实应用兼容性待测 |
| 桌面小组件 | ✅ 事件驱动状态机，Win+D、显示桌面按钮、桌面图标等 13 个场景通过（单屏） |
| 截图里的玻璃 | ⚠️ 默认用内置截图；逐窗口重建背景的方案动态上可行，但还需要自己合成窗口阴影 |
| 通知中心 | ⏸ 延后，不进首版 |

**还没有验证**：多显示器、HDR、高刷新率、Explorer 重启、睡眠唤醒、长时间稳定性，以及稳定版 Windows 11。

## 仓库结构

```text
native/core    Atrium.Core：玻璃栏、托盘、设置管道（C++20、D3D11、DirectComposition）
native/guard   原生任务栏恢复守护进程 Atrium.Guard（C++20，静态 CRT）
settings/      设置中心 Atrium.Settings（C# / WinUI 3）
tools/         build.ps1 打包脚本、图标生成
tests/         端到端与协议测试脚本
docs/          架构、有效决策、验证报告、设置协议、风险与路线图
poc/Probe      验证探测：捕获、玻璃渲染、输入、任务栏、托盘、桌面组件、恢复事务
poc/TrayProbe  ManagedShell 对照探测
poc/results    探测原始结果（JSON）
```

建议阅读顺序：[DECISIONS](docs/DECISIONS.md) → [VERIFICATION](docs/VERIFICATION.md) → [ARCHITECTURE](docs/ARCHITECTURE.md) → [ROADMAP](docs/ROADMAP.md)。

## 构建

需要 Windows 10 19041 及以上（推荐 Windows 11）、.NET 10 SDK，以及带 C++ 工作负载和 Windows SDK 的 Visual Studio 或 Build Tools（CMake 3.25+）。

```powershell
# 原生 Core、Guard 与设置中心，输出便携目录 dist/Atrium（加 -Zip 同时打包）
./tools/build.ps1

# 测试（会短暂启动 Atrium 并操作设置窗口，结束后恢复设置文件）
./tests/e2e-settings.ps1 -Package
./tests/pipe-protocol.ps1

# 验证探测程序
dotnet build poc/Atrium.Poc.sln -c Release
```

> [!WARNING]
> 部分探测会隐藏任务栏、模拟按键和点击、临时修改任务栏自动隐藏或桌面图标设置。它们都会等你空闲时才执行，结束后恢复原状；运行前请先阅读 [poc/README.md](poc/README.md)。

## 参与贡献

现在最有价值的是：设计评审，以及在其他硬件和系统上运行探测得到的结果，尤其是稳定版 Windows 11、多显示器和 HDR。详见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可证

[MIT](LICENSE)。视觉灵感来自 Apple 的 Liquid Glass；本项目与 Apple、Microsoft 无关。
