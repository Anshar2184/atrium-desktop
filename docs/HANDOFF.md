# 交接文档（2026-11-01，v0.5 之后）

给接手的新对话：先读本文，再读 [DECISIONS](DECISIONS.md)（当前有效决策 D01–D27，D23/D24/D26 为 v0.5 版本）和 [VERIFICATION](VERIFICATION.md) 第 16–18 节。保留已有实现，不推倒重做。

## 1. 项目现状

- 仓库：`wynter-yaxley/atrium-desktop`（MIT），本机路径 `D:\Projects\atrium-desktop`（WSL 下 `/mnt/d/Projects/atrium-desktop`），分支 `main`（最新提交以 `git log` 为准）。
- 版本 0.5.0（`native/CMakeLists.txt`、`native/core/core.rc`、`settings/Atrium.Settings/Atrium.Settings.csproj` 三处同步）。
- 形态：与系统任务栏**共存**的玻璃任务栏（不隐藏、不替换系统任务栏；接管与 Guard 恢复代码存在但默认关闭，D16）。
- 已完成：
  - v0.3：任务栏结构 [Windows 徽标][固定+运行应用][时钟][通知铃铛]，溢出入口，拖动排序，文件拖放；自绘开始菜单；Atrium 自己的通知；WinUI 3 设置中心。
  - v0.4：层级守护（D25）、弹窗与任务栏统一玻璃材质和圆角（D26）、程序坞式悬停放大 / 启动弹跳 / 按压波纹 / 弹窗动画（D27）、开始菜单重做、`magnify` 设置项与滑块。
  - v0.5：弹窗玻璃与栏参数完全相同、一个弹窗多块玻璃、墨色按背景切换（D26）；开始菜单改为启动器：面板（搜索 + **Windows 开始菜单的固定项**，7×3 分页）+ 与栏同形的胶囊状态条 + 电源玻璃卡片，取消全部应用（D23）；通知每张卡片一块玻璃、向上堆叠、向上展开（D24）。
- 用户正在日常使用 `dist\Atrium\Atrium.Core.exe`（v0.4）。

## 2. 代码地图（native/core）

| 文件 | 内容 |
| --- | --- |
| `bar.cpp/.h` | 玻璃栏：布局（`Relayout`）、输入窗口与命中测试、拖动、层级守护（`GuardStacking`、`ForegroundIsFullscreen`）、渲染线程（`RenderSession`，同时绘制弹窗玻璃层 `renderPanels`）、`LogState` 测试钩子 |
| `render/bar_painter.*` | 栏内容绘制与动画：放大映射 `MapX`（升余弦密度积分）、弹跳、波纹、小圆点、Windows 徽标 `DrawLogo` |
| `render/glass_renderer.*` | 玻璃着色器管线；`PopVisual` DComp 弹出动画；参数 `GlassParams`（128 字节，含形状矩形与波纹） |
| `render/shaders/glass.hlsl` | 玻璃着色器（与 `GlassParams` 一一对应，改一处必须改另一处） |
| `render/glass_panel.*` | 弹窗玻璃层的描述（`GlassPanel`）、`GlassHost` 接口、`PanelToneFor` |
| `ui/surface.*` | 弹窗基础：上层 D2D 内容（WARP）+ 下层玻璃（栏的渲染线程）；阴影边距、动画帧、波纹、`Theme` |
| `ui/start_menu.*` | 开始菜单（面板、状态胶囊、电源卡片三块玻璃，`Surface::SetShapes`） |
| `ui/notifications.*` | 通知弹出与列表（每张卡片一块玻璃，向上堆叠） |
| `main.cpp` | 进程入口、托盘、菜单、`--command` 测试命令（`RunCommand`） |
| `shell/*` | 任务模型、应用目录（含 Windows 开始菜单固定项：`AppCatalog::StartPins`，`Export-StartLayout`）、系统状态、通知监听（实验，默认关） |
| `clock.h` | 两线程共用的动画时钟 `ClockMs()` |

设置中心：`settings/Atrium.Settings`（C# / WinUI 3 / .NET 10）。设置键见 [SETTINGS-PROTOCOL](SETTINGS-PROTOCOL.md)，C++ `settings.h` 与 C# `BarSettings.cs` 必须同步。

## 3. 构建、打包、测试、推送

所有操作在用户电脑（devbox “pc”，项目目录 `/mnt/d/Projects`）上执行。

```bash
# 原生（Core）
cd /mnt/d/Projects/atrium-desktop/native
/mnt/d/Microsoft/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe --build build/win-x64 --config Release --target atrium_core

# 设置中心
cd ../settings/Atrium.Settings && ~/.local/bin/dn build -c Release

# 打包（dist/Atrium + dist/Atrium-<版本>-win-x64.zip）
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "\$env:PATH = 'D:\Microsoft\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;' + \$env:LOCALAPPDATA + '\atrium-tools\dotnet;' + \$env:PATH; \$env:DOTNET_ROOT = \$env:LOCALAPPDATA + '\atrium-tools\dotnet'; & 'D:\Projects\atrium-desktop\tools\build.ps1' -Zip"

# 推送（WSL 直连会超时：用 Windows git + 本地代理 + gh 凭据）
G="/mnt/c/Program Files/Git/cmd/git.exe"; GH='C:/Users/Lenovo/AppData/Local/atrium-tools/gh/bin/gh.exe'
"$G" -c http.proxy=http://127.0.0.1:10808 -c credential.helper= -c "credential.helper=!'$GH' auth git-credential" push origin main

# CI 状态
HTTPS_PROXY=http://127.0.0.1:10808 /mnt/c/Users/Lenovo/AppData/Local/atrium-tools/gh/bin/gh.exe run list -L 1 --json headSha,status,conclusion
```

测试（`tests/`，用 **Windows PowerShell 5** 的完整路径运行；WSL 里的 `powershell.exe` 实际是 PowerShell 7，WinForms 的 `Add-Type` 会失败）：

```bash
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/e2e-stacking.ps1 [-Core <exe>]
```

| 脚本 | 项数 | 内容 |
| --- | --- | --- |
| `e2e-stacking.ps1` | 5 | 置顶遮挡后恢复、全屏隐藏/恢复、最大化保留、让位于升起的系统任务栏 |
| `e2e-motion.ps1` | 7 | 放大、回落、弹跳、波纹、开始菜单与通知的玻璃层和圆角 |
| `e2e-bar.ps1` | 11 | 点击、最小化、轮换、时钟、拖动排序、文件拖放、溢出 |
| `e2e-start.ps1` | 13 | 开始菜单（真实鼠标键盘，电源为演练模式）：尺寸、胶囊与栏同形、Windows 固定项、翻页、搜索、固定到程序坞 |
| `e2e-notify.ps1` | 15 | 通知队列、合并、超时、展开、向上堆叠、关闭、动作、全屏抑制、隐藏正文 |
| `e2e-settings.ps1` | 14 | 设置中心（UI 自动化）；`-Package` 测打包版 |
| `pipe-protocol.ps1` | 7 | 设置管道协议（无鼠标） |

- v0.5 结果见 VERIFICATION §18。
- 除 `pipe-protocol` 外都移动真实鼠标，脚本开头检查用户空闲 ≥30 s，否则输出 `SKIP` 并退出（**SKIP 不算通过**）。用户经常在用电脑：可以写一个循环，每次先等空闲 >35 s 再跑一个脚本，后台执行并轮询日志。
- 测试钩子：`Atrium.Core.exe --command <verb> [arg]`，verb 有 `bar-state`、`start`、`start-state`、`start-type`、`start-key`、`start-click <id>`（如 `power`、`page:1`）、`notify app|title|body|action`、`notify-state`、`notify-open`、`notify-click <id>`（如 `stack`）、`notify-clear`、`notify-quiet n`、`notify-probe`。环境变量 `ATRIUM_START_LAYOUT=<json>` 让开始菜单读这个文件而不是导出用户的固定项（截图用）。日志在 `%LOCALAPPDATA%\Atrium\logs\core.log`；`--debug-layout` 记录各项的屏幕矩形，`--debug-capturable` 让栏和弹窗能被截图（背景冻结在第一帧）。
- 本地辅助脚本（不在仓库）：`%LOCALAPPDATA%\atrium-tools\`，其中 `v05-shot.ps1`（README 截图，带测试背景，设 `ATRIUM_PRIVACY=1` 和 `ATRIUM_START_LAYOUT=demo-start-layout.json`，即 Windows 自带应用）、`run-tests.ps1`（后台依次跑测试：每个脚本前等用户空闲 >35 s，开始前退出、结束后重启用户的 Atrium；`-Tests 'e2e-start,e2e-notify'`）、`mem3.ps1`（内存）、`idle.ps1`（空闲秒数）、`demo-pins.json`、`acb\acb-call.mjs`（见下）。

## 4. 必须遵守的约束

- 一切开发、构建、Git 都在用户电脑上用 devbox 工具执行；回复用户用中文。
- Atrium 单实例：测试前要正常退出用户正在运行的 `dist\Atrium\Atrium.Core.exe`（`--exit`），测试后重新启动它（WSL 里用 `powershell.exe -Command Start-Process ...`；`cmd.exe /c start` 会挂住）。用户的设置中心窗口开着时，`e2e-settings` 会因单实例转交而失败，`tools/build.ps1` 也会拒绝打包（v0.5 起；之前会把 `dist\Atrium` 删到一半），需等其关闭。
- 测试会备份并还原 `%LOCALAPPDATA%\Atrium\settings.json`（用户当前**没有**这个文件，只有 `logs`，测试后保持如此）、光标与前台窗口；结束后确认没有残留测试进程。
- 不要提交含个人内容的截图（账户名、头像、Wi-Fi 名、个人文件）；截图用测试背景和 `ATRIUM_PRIVACY=1`。
- 删除、覆盖、改系统设置前先确认；用户已授权自主提交与推送。按阶段提交，每个提交可单独构建。
- 不注入 Explorer / DWM，不改用户的系统通知设置；系统入口只用公开快捷键（D11）。
- 本机环境：Windows 11 26300 预览版，3200×2000、200% 缩放，系统任务栏自动隐藏，Intel Arc 核显 + RTX 4060。

## 5. 调用 Codex（agent-control-bridge）

用户电脑上有 agent-control-bridge（`D:\Download App\Agent Control Bridge`，MCP stdio 服务），可以使用 ChatGPT 桌面应用里的 Codex 对话。本会话没有挂载它，用本地脚本调用：

```bash
cd /mnt/c/Users/Lenovo/AppData/Local/atrium-tools/acb
N="/mnt/d/Download App/Agent Control Bridge/runtime/node.exe"
"$N" acb-call.mjs acb_codex_list '{"limit":5}'
"$N" acb-call.mjs acb_codex_send prompt.json      # {"threadId":..., "prompt":..., "wait":false}
"$N" acb-call.mjs acb_codex_read '{"threadId":"...","turns":1}'
```

atrium-desktop 的对话是「制定 Atrium Desktop 技术架构」（`01a19124-4b29-7172-a13a-3f5b0122f0c1`，cwd 为本仓库）。v0.5 用它出开始菜单视觉方案和做只读代码审查；让它“不要修改文件、不要构建或运行 Atrium”，避免与本会话的改动和测试冲突。长任务用 `wait:false` 再轮询。

## 6. 容易踩的坑

- 源码里不要直接写 Segoe Fluent Icons 的私有区字符，统一写成 `L"\uE721"` 这类转义（编辑器里看不见，用脚本替换时会被当成空串）。
- 测试脚本是 UTF-8 BOM + CRLF；PowerShell 双引号字符串里的引号要用反引号转义；子进程测试窗口要先 `SetProcessDPIAware`，否则 200% 下坐标被放大一倍。
- 在跨进程 `SendMessage`（`--command`）里不能发出 COM 调用（网络列表等会失败），需要的话用定时器异步做。
- 弹窗窗口 = 内容区 + 12 DIP 阴影边距；`Surface::Rect()` 是内容区，鼠标消息已换成内容坐标；测试日志里的坐标都是屏幕像素。
- 内容层不要用 D2D 图层做透明度（WARP 会缓存，内存涨几十 MB），用 `Canvas::BeginGroup`（透明度乘进画刷）。
- 渲染线程的帧日志要写在“无变化跳过”之前，否则空闲时等不到。

## 7. 剩余限制与可做的下一步

已知限制（详见 VERIFICATION §17、§18）：
- 全屏判定只看前台窗口；多显示器整体未做。
- 悬停名称仍是系统提示框，可改成玻璃标签（在栏的放大区域上方绘制）。
- 开始菜单只读 Windows 的固定项，不能在 Atrium 里增删；固定项文件夹平铺列出；一个弹窗只有一种墨色。
- 弹窗阴影边距在窗口内，点在边距上会关闭菜单但不会穿透到下层窗口。
- 自动宽度变化时栏是瞬间变宽，没有动画。
- 用户对“系统任务栏升起时谁在上面”的偏好未明确，目前让系统任务栏在上（D25）。

更早留下的未完成项：键盘操作与屏幕阅读器支持、窗口缩略图预览、多显示器、HDR / 高刷新率、Explorer 重启与睡眠唤醒、长时间稳定性、稳定版 Windows 11 验证；第三方托盘桥接（D12）与真正接管系统任务栏仍待证；镜像 Windows 通知为实验功能、默认关闭（D24）。

新对话开始时建议：先 `git log -5`、`git status` 确认状态，向用户确认下一步要做的方向。
