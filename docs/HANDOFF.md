# 交接文档（2026-11-01，v0.7 之后）

给接手的新对话：先读本文，再读 [DECISIONS](DECISIONS.md)（当前有效决策 D01–D34）和 [VERIFICATION](VERIFICATION.md) 第 17–23 节。保留已有实现，不推倒重做。

## 1. 项目现状

- 仓库：`wynter-yaxley/atrium-desktop`（MIT），本机路径 `D:\Projects\atrium-desktop`（WSL 下 `/mnt/d/Projects/atrium-desktop`），分支 `main`（最新提交以 `git log` 为准）。
- 版本 0.8.0（`native/CMakeLists.txt`、`native/core/core.rc`、`settings/Atrium.Settings/Atrium.Settings.csproj` 三处同步）。
- 形态：与系统任务栏**共存**的玻璃任务栏（不隐藏、不替换系统任务栏；接管与 Guard 恢复代码存在但默认关闭，D16）。
- 已完成：
  - v0.3：任务栏结构 [Windows 徽标][固定+运行应用][时钟][通知铃铛]，溢出入口，拖动排序，文件拖放；自绘开始菜单；Atrium 自己的通知；WinUI 3 设置中心。
  - v0.4：层级守护（D25）、弹窗与任务栏统一玻璃材质和圆角（D26）、程序坞式悬停放大 / 启动弹跳 / 按压波纹 / 弹窗动画（D27）、开始菜单重做、`magnify` 设置项与滑块。
  - v0.6：自绘托盘面板（D28，UI 自动化读取系统托盘按钮并转发点击）、状态图标与控制中心（D30：Wi-Fi、音量、电池）、Windows 通知默认显示为卡片并同步更新（D24）、微信会话列表来源（D29，实验）、开始菜单固定项展开修复、浅色背景墨色阈值、栏对系统任务栏的让位规则（D25）。
  - v0.8：通知动效重做（关闭在原处淡出、堆叠上滚轮翻动、堆叠原地展开成列表、弹窗帧跟随合成器）；弹窗按区域取墨（D26）；托盘转发点击按程序找按钮（修复飞书卡片打开 Docker）；iPhone 式状态环与 macOS 式控制中心（蓝牙、亮度、投屏，D30）；macOS 式锁屏封面，液态玻璃时钟（距离场形状），交给 Windows 锁屏（D34）。
  - v0.7.2：点通知卡片时若发送者在托盘里，转发它的托盘点击（QQ 打开未读会话，不再打开登录页）；飞书来源（D33，读取飞书自己的消息弹窗，`notifyFeishu`）。
  - v0.7.1：关闭 / 打开应用时栏宽与图标平滑过渡（D27，栏窗口固定为最大宽度）；所有弹窗按统一规则对齐栏的左右边（D32，`PopupRect`）；托盘面板按可见边距重新排版、图标 20 DIP（D28）。
  - v0.7：托盘面板缩小、图标按 Windows 的 `UIOrderList` 顺序与记录对齐（D28，修复图标错位）；时钟打开自绘日历（D31，月历 + 农历）；其他面板开着时通知卡片等待，不再重叠（D24）。
  - v0.5：弹窗玻璃与栏参数完全相同、一个弹窗多块玻璃、墨色按背景切换（D26）；开始菜单改为启动器：面板（搜索 + **Windows 开始菜单的固定项**，7×3 分页）+ 与栏同形的胶囊状态条 + 电源玻璃卡片，取消全部应用（D23）；通知每张卡片一块玻璃、向上堆叠、向上展开（D24）。
- 用户正在日常使用 `dist\Atrium\Atrium.Core.exe`（v0.7）。

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
| `ui/tray_panel.*`、`shell/tray_model.*` | 托盘面板；托盘模型（工作线程 UI 自动化读取 `NotifyItemIcon`，Invoke / ShowContextMenu 转发，`NotifyIconSettings` 取图：`UIOrderList` 顺序对齐 + `Shell_NotifyIconGetRect` 判断图标是否存在；每次列表变化在日志里逐个记下按钮对应的程序） |
| `ui/lock_screen.*` | 锁屏封面（墙纸窗口 + 玻璃时钟距离场 + 低级键盘钩子，交给 `LockWorkStation`） |
| `shell/radios.*` | Wi-Fi / 蓝牙无线电、已配对蓝牙设备、屏幕亮度（WinRT、WMI，阻塞调用，只在工作线程） |
| `ui/calendar_panel.*` | 日历（月历玻璃 + 时间胶囊，农历用 Windows 的 ICU） |
| `ui/control_center.*`、`shell/wifi.*` | 控制中心（网络卡片、音量与电池胶囊）；WLAN API |
| `shell/feishu_source.*` | 飞书来源（找飞书的消息弹窗，只读 UI 自动化，每 0.4 s） |
| `shell/wechat_source.*` | 微信会话列表来源（只读 UI 自动化，每 2 s） |
| `main.cpp` | 进程入口、托盘、菜单、`--command` 测试命令（`RunCommand`） |
| `shell/*` | 任务模型、应用目录（含 Windows 开始菜单固定项：`AppCatalog::StartPins`，`Export-StartLayout`）、系统状态、通知监听（默认开，每秒轮询） |
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
| `e2e-motion.ps1` | 8 | 放大、回落、弹跳、波纹、开始菜单与通知的玻璃层和圆角 |
| `e2e-bar.ps1` | 11 | 点击、最小化、轮换、时钟、拖动排序、文件拖放、溢出 |
| `e2e-start.ps1` | 13 | 开始菜单（真实鼠标键盘，电源为演练模式）：尺寸、胶囊与栏同形、Windows 固定项、翻页、搜索、固定到程序坞 |
| `e2e-notify.ps1` | 18 | 通知队列、合并、超时、展开、向上堆叠、关闭、动作、全屏抑制、面板开着时卡片等待、隐藏正文、真实 Windows 通知出卡 |
| `e2e-lock.ps1` | 5 | 锁屏封面（演练模式）：显示与玻璃时钟、栏让开、按键不进前台窗口且交给锁屏、栏回来、点击同样交接 |
| `e2e-tray.ps1` | 13 | 托盘入口与状态图标、托盘面板、左右键转发到测试托盘图标、日历（打开、翻月、Esc）、控制中心、音量滑块与滚轮 |
| `e2e-settings.ps1` | 14 | 设置中心（UI 自动化）；`-Package` 测打包版 |
| `pipe-protocol.ps1` | 7 | 设置管道协议（无鼠标） |

- v0.6 结果见 VERIFICATION §19。测试的设置文件都显式写 `"notifyListener":false,"notifyWeChat":false,"notifyFeishu":false`，避免真实消息干扰（`e2e-notify` 的 `windows-toast` 除外）。
- 除 `pipe-protocol` 外都移动真实鼠标，脚本开头检查用户空闲 ≥30 s，否则输出 `SKIP` 并退出（**SKIP 不算通过**）。用户经常在用电脑：可以写一个循环，每次先等空闲 >35 s 再跑一个脚本，后台执行并轮询日志。
- 测试钩子：`Atrium.Core.exe --command <verb> [arg]`，verb 有 `bar-state`、`start`、`start-state`、`start-type`、`start-key`、`start-click <id>`（如 `power`、`page:1`）、`notify app|title|body|action`、`notify-state`、`notify-open`、`notify-click <id>`（如 `stack`）、`notify-clear`、`notify-quiet n`、`notify-probe`、`tray-open`、`tray-state`、`tray-find <文字>`、`tray-click` / `tray-rclick <序号或文字>`、`control-open`、`control-state`、`control-click <id>`、`control-volume <0-100>`、`calendar-open`、`calendar-state`、`calendar-click <id>`（`prev`、`next`、`today`、`day:<0-41>`）、`lock-open`、`lock-state`、`lock-dismiss [lock]`（不带 `lock` 时只收起封面，不锁定）；`control-click` 新增 `wifi`、`bt`（开关无线电，测试不要用）、`wifi-page`、`bt-page`、`back`、`cast`。环境变量 `ATRIUM_LOCK_DRYRUN=1` 让锁屏只记录交接不真锁定（**测试锁屏必须设置**），`ATRIUM_LOCK_WALLPAPER=<图片>` 替换墙纸（截图）。环境变量 `ATRIUM_START_LAYOUT=<json>` 让开始菜单读这个文件而不是导出用户的固定项（截图用）。日志在 `%LOCALAPPDATA%\Atrium\logs\core.log`；`--debug-layout` 记录各项的屏幕矩形，`--debug-capturable` 让栏和弹窗能被截图（背景冻结在第一帧）。
- 本地辅助脚本（不在仓库）：`%LOCALAPPDATA%\atrium-tools\`，其中 `v07-shot.ps1`（托盘、日历、面板开着时的通知截图，`-SettingsFile demo-v07.json`）、`v05-shot.ps1`（README 截图，带测试背景，设 `ATRIUM_PRIVACY=1` 和 `ATRIUM_START_LAYOUT=demo-start-layout.json`，即 Windows 自带应用）、`run-tests.ps1`（后台依次跑测试：每个脚本前等用户空闲 >35 s，开始前退出、结束后重启用户的 Atrium；`-Tests 'e2e-start,e2e-notify'`）、`mem3.ps1`（内存）、`idle.ps1`（空闲秒数）、`demo-pins.json`、`acb\acb-call.mjs`（见下）。

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
- 托盘：不能转发双击；按消息坐标放菜单的应用，菜单会出现在屏幕左下。微信来源实机出卡待验证（需要真实入站消息），且微信主窗口关到托盘（隐藏）时读不到会话列表，只在打开或最小化时可用；飞书的系统通知待真实样本。
- 测试中出现过一次 GPU 设备被移除（`0x887A0005`）后渲染线程停帧、Core 退出变慢；`run-tests.ps1` 的下一个脚本若在旧 Core 退出前启动，新 Core 会因单实例直接退出。
- 本机用户的 QQ 设置为“任务栏头像闪动”，会让自动隐藏的系统任务栏长时间升起：截图与测试时栏不再因此让位（D25），但系统任务栏仍可见。
- 弹窗阴影边距在窗口内，点在边距上会关闭菜单但不会穿透到下层窗口。
- 用户对“系统任务栏升起时谁在上面”的偏好未明确，目前让系统任务栏在上（D25）。

更早留下的未完成项：键盘操作与屏幕阅读器支持、窗口缩略图预览、多显示器、HDR / 高刷新率、Explorer 重启与睡眠唤醒、长时间稳定性、稳定版 Windows 11 验证；第三方托盘桥接（D12）与真正接管系统任务栏仍待证；镜像 Windows 通知为实验功能、默认关闭（D24）。

## 8. 通知接入交接更新（2026-11-01）

通知开发先读本机独立验证目录中的 [适配验证与集成报告](../../atrium-validation/2026-11-01/QQ-WECHAT-ADAPTER-VALIDATION.md)，以及 [通知、托盘和捕获可行性报告](../../atrium-validation/2026-11-01/FEASIBILITY-REPORT.md)。这些是仓库外的本机报告，Git clone 不会附带；完整路径为 `D:\Projects\atrium-validation\2026-11-01`，换机器交接时需一并提供。

- **通用 Windows Toast 为主，特殊应用适配为辅。** 获得授权且 Listener 可读的标准 Toast 共用来源、文本与状态同步，不按每个软件重写采集器；原始按钮、快捷回复、定位会话另行验收。
- **QQ**：独立探针已捕捉到新增系统 Toast，但与具体测试聊天的对应、覆盖率和点击尚未通过。先补通用通道。
- **飞书**：优先用真实入站消息验证同一 Windows 通道；尚未取得有效样本，不预先开发专用适配器，也不标记已兼容。读不到先判断通知设置和来源类型。
- **微信**：未读到系统 Toast；只读 UIA 已捕捉五次后台未读增长，均为免打扰会话。当前各应用只读到 10 个会话行，关闭到托盘、非免打扰真实出卡和稳定身份待补，不宣称全量接管。
- 默认事件流为“系统 Toast / 专用适配器 → 统一 `Notice` 模型 → 现有 Atrium 卡片”。Windows 通知中心留档是可选单向镜像；自己的镜像必须防止重复/回环。`SuppressPopup` 只控制自己生成的 Toast，不能阻止其他应用的自绘弹窗。
- GitHub 参考：[Seelen UI 通知模块](https://github.com/eythaann/Seelen-UI/tree/master/src/background/modules/notifications)、[微软监听示例](https://github.com/WindowsNotifications/NotificationListenerActionCenterApp)、[wechat-toast](https://github.com/EthanAIDev/wechat-toast)。后者仍有后台稳定性和精确跳转限制；各项目的机制、许可和证据见独立报告，不直接搬入大型依赖。

下一批顺序：通用 Toast 同步 → QQ/飞书入站与基本点击 → 微信 UIA 出卡 → 已证实的覆盖缺口。复用 `notification_listener.*`、`Notice`、`WM_NOTICE` 和现有卡片；注意 `WM_NOTICE` 当前受 `g_app.listener` 条件限制，UIA 不应被误绑到系统监听开关。只读探针结果不等于产品集成已完成。

新对话开始时先 `git log -5`、`git status` 确认状态，按用户已授权的上述方向继续；遇到确实缺失的输入再询问。
