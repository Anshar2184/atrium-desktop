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
> **v0.9 开发版。** 一条与系统任务栏共存的 C++ 玻璃任务栏：Windows 徽标打开 Atrium 开始菜单，固定与运行中的应用（可拖动排序），自绘的托盘面板，一个单色的 Wi-Fi 状态图标（点开是 macOS 式的控制中心：Wi-Fi、蓝牙、电池、投屏与投影模式、亮度、声音与输出设备、各应用音量），时钟（点开是 Atrium 日历，带农历），以及显示 Windows 通知和微信消息的 Atrium 通知，macOS 式的锁屏（液态玻璃时钟，按键后交给 Windows 锁屏），加上可实时预览的设置中心。所有弹出窗口都是与任务栏完全相同的液态玻璃；悬停时图标像 macOS 程序坞一样波浪放大。它不会隐藏或替换系统任务栏；完整替换还要等恢复与托盘在稳定版 Windows 11 上通过验证。

<p align="center">
  <img src="docs/images/v04-magnify.png" alt="v0.4 任务栏：悬停放大" width="80%"><br>
  <sub>指针下的图标放大，两侧依次变小，玻璃栏随之伸展；运行中的应用下方是小圆点（背景为测试图案）</sub>
</p>

<p align="center">
  <img src="docs/images/v06-control.png" alt="Atrium 控制中心" width="60%"><br>
  <sub>栏右侧：托盘入口、Wi-Fi 与电量合成的状态图标（电量数字写在电池里）、时钟、通知。点状态图标打开控制中心：附近的 Wi-Fi（已保存的网络直接连接）、音量滑块、电池（截图隐藏了网络名称）</sub>
</p>

<p align="center">
  <img src="docs/images/v05-start.png" alt="Atrium 开始菜单" width="52%">
  <img src="docs/images/v05-center.png" alt="Atrium 通知" width="39%"><br>
  <sub>开始菜单：搜索框和 Windows 开始菜单的固定项（每页 7×3），下方是与任务栏同高、同为胶囊形的状态条；通知：每张卡片一块玻璃，向上堆叠、向上展开。材质与任务栏完全相同（截图用 Windows 自带应用作固定项，隐藏了账户与网络名称）</sub>
</p>

<p align="center">
  <img src="docs/images/v03-settings.png" alt="设置中心" width="70%"><br>
  <sub>设置中心：侧边栏与卡片，顶部是按真实参数在 CPU 上渲染的玻璃样张，右上角放大镜显示边缘的折射与色散</sub>
</p>

<details>
<summary>v0.4 截图</summary>

<p align="center">
  <img src="docs/images/v04-start.png" alt="Atrium 开始菜单 v0.4" width="52%">
  <img src="docs/images/v04-center.png" alt="Atrium 通知 v0.4" width="44%"><br>
  <sub>v0.4：开始菜单含按首字母分组的全部应用，通知向下堆叠，玻璃比任务栏更模糊、染色更深</sub>
</p>

</details>

<details>
<summary>v0.3 截图</summary>

<p align="center">
  <img src="docs/images/v03-bar.png" alt="v0.3 任务栏" width="80%"><br>
  <sub>徽标（开始菜单）· 固定与运行中的应用 · 时钟 · 通知；图标位于与栏圆角同心的底板中央（背景为测试图案）</sub>
</p>

<p align="center">
  <img src="docs/images/v03-start.png" alt="Atrium 开始菜单" width="48%">
  <img src="docs/images/v03-notify.png" alt="Atrium 通知" width="48%"><br>
  <sub>开始菜单：固定的应用、全部应用、搜索、电源，底部是网络、音量、电池和系统托盘入口；通知：收拢堆叠，点击展开</sub>
</p>

</details>

<details>
<summary>v0.2 截图</summary>

<p align="center">
  <img src="docs/images/v02-bar-liquid.png" alt="液态玻璃预设" width="80%"><br>
  <img src="docs/images/v02-bar-frosted.png" alt="磨砂玻璃预设" width="80%"><br>
  <img src="docs/images/v02-bar-light.png" alt="浅色背景下自动切换为深色文字" width="80%"><br>
  <sub>实机画面：液态（参照 macOS Liquid Glass 的强折射与色散）、磨砂，以及浅色背景下自动切换的深色文字（背景为测试图案）</sub>
</p>

<p align="center">
  <img src="docs/images/v02-bar-hover.png" alt="悬停" width="80%"><br>
  <img src="docs/images/v02-settings-pins.png" alt="设置中心" width="60%"><br>
  <sub>悬停时图标浮起并在上方显示名称；设置中心可实时调节材质、尺寸并管理固定的应用</sub>
</p>

</details>

## 它要做什么

Atrium 重新设计你每天看到的那一层：一条悬浮的玻璃任务栏、它自己的开始菜单和通知，以及一套桌面小组件，共享同一种材质和设计语言。Explorer 和 Windows Shell 保持不动，系统的开始菜单、搜索、通知中心和快捷设置照常可用。

- **一套材质引擎**：大圆角与胶囊形状、边缘折射、适度色散、GPU 模糊、柔和高光；液态和毛玻璃只是同一管线的参数预设。
- **原生、轻量**：C++20、Win32、Direct3D 11、DirectComposition、DirectWrite；不用浏览器内核，不装服务，不需要管理员权限。
- **能长期日常使用**：与系统任务栏共存，不隐藏、不替换它；不向 Explorer 或 DWM 注入代码，不修改系统文件。

## 快速开始

1. 从 [Actions](https://github.com/wynter-yaxley/atrium-desktop/actions/workflows/build.yml) 最新一次成功构建下载 `Atrium-win-x64` 构件，或按下文自行构建。
2. 解压后运行 `Atrium.Core.exe`。屏幕底部出现玻璃任务栏，首次运行会导入你当前 Windows 任务栏的固定项。
3. 左键点击应用：打开、切到前台或最小化；中键或 Shift+点击：新窗口；右键：固定 / 取消固定、打开文件所在位置、关闭窗口。拖动图标调整顺序；把文件拖到应用上即用它打开。
4. 点击左侧的 Windows 徽标打开 Atrium 开始菜单（直接打字即可搜索应用）；点击右侧铃铛查看通知；点击时钟打开 Atrium 日历（月历与农历）。鼠标经过图标时它们会放大（设置中心“形状”页的“悬停放大”可调整或关闭）；前台应用最大化或全屏时玻璃栏滑出屏幕，鼠标移到屏幕底边时滑回。
5. 双击托盘图标（或右键 →“外观设置…”）打开设置中心，调整材质、尺寸、固定的应用和通知。
6. 托盘右键 →“退出 Atrium”即可完全退出；不修改注册表，不安装服务，设置保存在 `%LOCALAPPDATA%\Atrium`。

## 当前状态

验证都在一台 Windows 11 26300 预览版、核显 + RTX 4060 的笔记本上执行。完整数据见 [docs/VERIFICATION.md](docs/VERIFICATION.md)，当前有效决策见 [docs/DECISIONS.md](docs/DECISIONS.md)。

| 方向 | 结论 |
|---|---|
| v0.6 托盘、控制中心与通知接入 | ✅ 自绘托盘面板（读取系统托盘图标，左键 / 右键转发给应用本身）、状态图标与控制中心（Wi-Fi、音量、电池）、Windows 通知默认显示为卡片、微信消息（读取会话列表，实验）；开始菜单固定项在缺少环境变量时也完整；浅色背景下文字改为深色：托盘 8 项、开始菜单 13 项、通知 16 项及其余回归测试通过 |
| v0.5 开始菜单与通知 | ✅ 开始菜单显示 Windows 开始菜单的固定项（与任务栏的固定项分开）、只保留搜索、分页；状态条与任务栏同形；通知每张卡片一块玻璃、向上堆叠；弹出窗口与任务栏材质完全一致：开始菜单 13 项、通知 15 项、动效 7 项及其余回归测试通过 |
| v0.4 动效、统一材质与置顶 | ✅ 悬停放大、启动弹跳、按压波纹、弹出窗口共用任务栏的玻璃：7 项；被置顶窗口遮挡后恢复在最上、全屏应用时隐藏（v0.9.2 起最大化与全屏时自动隐藏）、让位于升起的系统任务栏：5 项；v0.3 的全部测试照常通过 |
| v0.3 任务栏、开始菜单与通知 | ✅ 共存模式；真实鼠标与键盘测试：任务栏 11 项、开始菜单 11 项、通知 14 项通过；设置端到端 14 项与协议 7 项通过 |
| v0.2 玻璃任务栏与设置中心 | ✅ 背景每帧变化时每次呈现 p95 0.8 ms、约单核 4% CPU、内存 144 MB |
| 实时玻璃 | ✅ 背景用 DXGI 桌面复制捕获（Atrium 窗口只对它排除），不可用时回到 WGC；D3D11；核显每帧 p95 2.4 ms，画面滞后 p95 1 帧 |
| 自回声 | ✅ 与最近一次呈现的背景精确比较；缓慢渐变、1 像素光标、低对比滚动文字都不会漏画 |
| 点击穿透 | ✅ 可点穿的显示窗口 + 带形状的输入窗口，阴影和透明角都能点到下方窗口 |
| 任务栏接管与恢复 | ⏸ v0.1 时用原型验证过接管协议与守护进程的故障恢复；当前只有共存模式，原型已移出仓库（git 历史） |
| 系统入口 | ✅ 系统任务栏隐藏后，开始、搜索、快捷设置、通知中心仍可正常使用 |
| 第三方托盘 | ⚠️ 转发式桥接的正常与失败路径都已验证；Explorer 重启、真实应用兼容性待测 |
| 桌面小组件 | ✅ v0.9.2：时钟、世界时钟、数字时钟、日历（含大号农历月历）、电池、系统监视、备忘录、提醒事项、照片；始终是透明玻璃；小 / 中 / 大三种尺寸，拖动吸附网格，右键改大小 / 移除 / 添加；位于桌面之上、所有窗口之下，Win+D 时在最上（单屏） |
| 截图与录屏 | ✅ v0.9.1：系统截图、使用 WGC / GDI 的截图和录屏能拍到任务栏、弹窗和小组件（D36）；使用 DXGI 桌面复制的录屏软件仍拍不到；双显卡笔记本首次运行后需重启一次 Atrium |
| 通知 | ✅ Atrium 自己的通知卡片（排队、合并、超时、未读、全屏抑制、隐藏正文）；⚠️ 镜像 Windows 通知为实验功能、默认关闭：未打包进程可读取并轮询，但系统横幅仍会重复出现 |

**还没有验证**：多显示器、HDR、高刷新率、Explorer 重启、睡眠唤醒、长时间稳定性，以及稳定版 Windows 11。

## 仓库结构

```text
native/core    Atrium.Core：玻璃栏、开始菜单、通知、控制中心、桌面小组件、设置管道（C++20、D3D11、DirectComposition）
settings/      设置中心 Atrium.Settings（C# / WinUI 3）
tools/         build.ps1 打包脚本、图标生成
tests/         端到端与协议测试脚本
docs/          有效决策、验证记录、设置协议、交接文档
```

建议阅读顺序：[HANDOFF](docs/HANDOFF.md) → [DECISIONS](docs/DECISIONS.md) → [VERIFICATION](docs/VERIFICATION.md)。早期的设计文档与验证探测程序（`poc/`、`native/guard`）已在 v0.9.2 移出仓库，可在 git 历史中查看。

## 构建

需要 Windows 10 19041 及以上（推荐 Windows 11）、.NET 10 SDK，以及带 C++ 工作负载和 Windows SDK 的 Visual Studio 或 Build Tools（CMake 3.25+）。

```powershell
# 原生 Core 与设置中心，输出便携目录 dist/Atrium（加 -Zip 同时打包）
./tools/build.ps1

# 测试（会短暂启动 Atrium 并操作设置窗口，结束后恢复设置文件）
./tests/e2e-settings.ps1 -Package
./tests/pipe-protocol.ps1
./tests/e2e-bar.ps1          # 以下几个会移动鼠标、点击或输入，仅在你 30 秒未操作时运行
./tests/e2e-start.ps1
./tests/e2e-notify.ps1
./tests/e2e-tray.ps1
./tests/e2e-widgets.ps1
./tests/e2e-motion.ps1
./tests/e2e-stacking.ps1
```

## 参与贡献

现在最有价值的是：设计评审，以及在其他硬件和系统上运行测试的结果，尤其是稳定版 Windows 11、多显示器和 HDR。详见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可证

[MIT](LICENSE)。视觉灵感来自 Apple 的 Liquid Glass；本项目与 Apple、Microsoft 无关。
