<div align="center">

# Atrium Desktop

**面向 Windows 11 的原生玻璃桌面环境**

实时折射的悬浮任务栏 · 统一材质的桌面小组件 · 保留 Explorer，崩溃可恢复

[![build](https://github.com/wynter-yaxley/atrium-desktop/actions/workflows/build.yml/badge.svg)](https://github.com/wynter-yaxley/atrium-desktop/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform: Windows 11](https://img.shields.io/badge/platform-Windows%2011-0078D6)](#当前状态)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](native/)
[![.NET 8 probes](https://img.shields.io/badge/probes-.NET%208-512BD4?logo=dotnet&logoColor=white)](poc/)
[![Status](https://img.shields.io/badge/status-prototype-orange)](docs/VERIFICATION.md)

简体中文 | [English](README_EN.md)

</div>

> [!NOTE]
> **项目处于原型阶段，还没有可安装的应用。** 首轮受控原型已经完成，部分高风险路径仍待验证。仓库里是架构文档、验证报告、验证用的探测程序，以及第一个原生组件（任务栏恢复守护进程）。

<p align="center">
  <img src="docs/images/glass-liquid.png" alt="液态玻璃材质" width="80%"><br>
  <img src="docs/images/glass-frosted.png" alt="毛玻璃材质" width="80%"><br>
  <sub>同一套 D3D11 材质的液态与毛玻璃预设，由 <code>poc/Probe</code> 在合成背景上渲染</sub>
</p>

## 它要做什么

Atrium 重新设计你每天看到的那一层：一条悬浮的玻璃任务栏和一套桌面小组件，共享同一种材质和设计语言。Explorer 和 Windows Shell 保持不动，开始菜单、搜索、通知中心和快捷设置照常使用。

- **一套材质引擎**：大圆角与胶囊形状、边缘折射、适度色散、GPU 模糊、柔和高光；液态和毛玻璃只是同一管线的参数预设。
- **原生、轻量**：C++20、Win32、Direct3D 11、DirectComposition、DirectWrite；不用浏览器内核，不装服务，不需要管理员权限。
- **能长期日常使用**：独立的原生守护进程在崩溃、卡死时恢复系统任务栏；不向 Explorer 或 DWM 注入代码，不修改系统文件。

## 当前状态

两轮验证都在一台 Windows 11 26300 预览版、核显 + RTX 4060 的笔记本上执行。完整数据见 [docs/VERIFICATION.md](docs/VERIFICATION.md)，当前有效决策见 [docs/DECISIONS.md](docs/DECISIONS.md)。

| 方向 | 结论 |
|---|---|
| 实时玻璃 | ✅ WGC 捕获 + 排除自身 + D3D11；核显每帧 p95 2.4 ms，画面滞后 p95 1 帧 |
| 自回声 | ✅ 与最近一次呈现的背景精确比较；缓慢渐变、1 像素光标、低对比滚动文字都不会漏画 |
| 点击穿透 | ✅ 可点穿的显示窗口 + 带形状的输入窗口，阴影和透明角都能点到下方窗口 |
| 任务栏接管与恢复 | ✅ 接管事务 + 原生守护进程：强杀、卡死、启动中途失败、双进程同时终止都能恢复到用户原设置 |
| 系统入口 | ✅ 系统任务栏隐藏后，开始、搜索、快捷设置、通知中心仍可正常使用 |
| 第三方托盘 | ⚠️ 转发式桥接的正常与失败路径都已验证；Explorer 重启、真实应用兼容性待测 |
| 桌面小组件 | ✅ 事件驱动状态机，Win+D、显示桌面按钮、桌面图标等 13 个场景通过（单屏） |
| 截图里的玻璃 | ⚠️ 默认用内置截图；逐窗口重建背景的方案动态上可行，但还需要自己合成窗口阴影 |
| 通知中心 | ⏸ 延后，不进首版 |

**还没有验证**：多显示器、HDR、高刷新率、Explorer 重启、睡眠唤醒、长时间稳定性，以及稳定版 Windows 11。

## 仓库结构

```text
docs/          架构、有效决策、验证报告、风险与路线图
native/guard   原生任务栏恢复守护进程 Atrium.Guard（C++20，静态 CRT）
poc/Probe      验证探测：捕获、玻璃渲染、输入、任务栏、托盘、桌面组件、恢复事务
poc/TrayProbe  ManagedShell 对照探测
poc/results    探测原始结果（JSON）
```

建议阅读顺序：[DECISIONS](docs/DECISIONS.md) → [VERIFICATION](docs/VERIFICATION.md) → [ARCHITECTURE](docs/ARCHITECTURE.md) → [ROADMAP](docs/ROADMAP.md)。

## 构建

需要 Windows 10 19041 及以上（推荐 Windows 11）、.NET 8 SDK，以及带 C++ 工作负载的 Visual Studio 2022 或 Build Tools。

```powershell
# 探测程序
dotnet build poc/Atrium.Poc.sln -c Release

# 原生守护进程
cmake --preset win-x64 -S native
cmake --build native/build/win-x64 --config Release
```

> [!WARNING]
> 部分探测会隐藏任务栏、模拟按键和点击、临时修改任务栏自动隐藏或桌面图标设置。它们都会等你空闲时才执行，结束后恢复原状；运行前请先阅读 [poc/README.md](poc/README.md)。

## 参与贡献

现在最有价值的是：设计评审，以及在其他硬件和系统上运行探测得到的结果，尤其是稳定版 Windows 11、多显示器和 HDR。详见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可证

[MIT](LICENSE)。视觉灵感来自 Apple 的 Liquid Glass；本项目与 Apple、Microsoft 无关。
