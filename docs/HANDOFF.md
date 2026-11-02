# 交接文档（2026-11-02，v0.9.1 之后）

给接手的新对话：**先读本文**，再读 [DECISIONS](DECISIONS.md)（当前有效决策 D01–D36）和 [VERIFICATION](VERIFICATION.md) 第 20–28 节（v0.7 以后的验证记录）。保留已有实现，不推倒重做。开工前先 `git log -5`、`git status`，并确认用户的 Atrium 正在运行。

## 0. 一句话现状

与系统任务栏共存的 C++ 液态玻璃任务栏，带自绘开始菜单、通知、托盘面板、控制中心、日历、锁屏封面、macOS 式桌面小组件；截图与录屏已能拍到 Atrium（v0.9.1）。最新提交 `2d93795`，版本 **0.9.1**，用户正在日常使用打包版 `dist\Atrium\Atrium.Core.exe`（0.9.1）。

## 1. 用户与协作方式（重要）

- 回复用户一律用**中文**；用户会直接在 Windows 上看效果并反馈视觉/交互细节，要求“与 macOS 对齐”、动画要连贯不闪。
- 用户说“先讨论 / 不要修改”时只讨论方案、给推荐，不动代码。
- 用户已授权：自主提交、推送到 `origin main`；可以调用 Codex（见 §6）。
- 删除、覆盖用户文件，改系统设置，安装软件（如虚拟机），真实锁屏，拨动 Wi-Fi/蓝牙开关，切换投影模式/输出设备之类会影响用户的操作——先问（或像 v0.8.2 那样“切过去立刻切回”并告知）。
- 已知偏好：
  - 任务栏上不要高饱和的绿色、不要拥挤的复合图标（v0.8 的 iPhone 圆环被否决，现为单色 Wi-Fi 扇形）。
  - 想要的是“原生锁屏”，不是盖在桌面上的封面（v0.8 的封面被评价为“脱裤子放屁”）；锁屏技术预研报告已交，**用户尚未决定是否做独立 PoC**（见 §8）。
- **教训（2026-11-02）**：中途强制结束测试运行器，导致用户 `settings.json` 被测试内容覆盖且无法找回（固定应用与外观设置丢失，已恢复为 `{}` + 重新导入 3 个 Windows 固定项，已向用户道歉）。**永远不要中途结束 e2e 测试或 run-tests.ps1**；它们在 `finally` 里还原用户文件。

## 2. 已完成功能（按版本）

- v0.3–v0.5：栏结构 [Windows 徽标][固定+运行应用][托盘][状态][时钟][通知]，溢出、拖动排序、文件拖放；开始菜单（搜索 + Windows 开始菜单固定项 7×3 分页 + 胶囊状态条 + 电源卡片，D23）；Atrium 通知（每卡一块玻璃，D24）；WinUI 3 设置中心（D18）；层级守护（D25）；统一材质与圆角（D26）；程序坞放大/弹跳/波纹（D27）。
- v0.6–v0.7.2：自绘托盘面板（D28，UIA 读系统托盘并转发点击）；Windows 通知默认镜像为卡片（D24）；微信（D29，实验）与飞书（D33）来源；时钟打开自绘日历（D31，农历）；弹窗统一对齐规则（D32，`PopupRect`）；点通知卡片转发发送者的托盘点击（QQ 打开已登录会话）。
- v0.8：通知动效重做（原地淡出、堆叠滚轮翻看、原地展开）；弹窗按区域取墨（D26）；macOS 式控制中心（D30：Wi-Fi、蓝牙、电池、投屏、亮度、声音）；锁屏封面 + 液态玻璃时钟（D34，交给 Windows 锁屏）。
- v0.8.1：状态图标改为单色 Wi-Fi 扇形（只表示连接，电量/音量只在控制中心）；音量滑块可从喇叭图标处拖动。
- v0.8.2：控制中心“声音输出”页（播放设备列表，`IPolicyConfig` 切默认设备；音量合成器）；自绘投屏页（`ProjectionManager` 搜无线显示器、`StartProjectingAsync` 投影，失败交 Win+K；Win+P 四种投影模式 `SetDisplayConfig`）。
- v0.8.3：控制中心换页不闪：窗口固定为最高页高度、玻璃贴底并渐变高度；主页几块玻璃与页面卡片之间用着色器的“融合形状”过渡（最多 6 个各角圆角不同的圆角矩形平滑并集，再渐变到整块卡片）；窗口区域静止时从玻璃顶部开始。
- v0.9：**macOS 式桌面小组件**（D35，`ui/desktop_widgets.*`）：时钟（小：表盘；中：世界时钟北京/东京/伦敦/纽约，夜间深色表盘）、日历（小/中，农历）、电池；桌面在前台全彩、应用在前台单色玻璃；网格拖动吸附；右键改尺寸/移除/添加；栏右键菜单可添加；布局 `%LOCALAPPDATA%\Atrium\widgets.json`，开关设置 `widgets`；农历抽成 `shell/lunar.*`。
- v0.9.1：**截图兼容**（D36）：玻璃背景改用 DXGI 桌面复制（DDA），Atrium 窗口只对 DDA 排除（`WCA_EXCLUDED_FROM_DDA`），系统截图与 WGC/GDI 录屏能拍到 Atrium；DDA 不可用时先恢复全排除再回 WGC；双显卡笔记本自动写入按应用图形偏好“节能”（下次启动生效）；“隐藏通知正文”时卡片仍排除于捕获。修了 DDA 取帧线程占锁导致的渲染卡顿。

## 3. 代码地图（native/core）

| 文件 | 内容 |
| --- | --- |
| `main.cpp` | 进程入口、`g_app`、托盘与栏右键菜单（含“添加桌面小组件”）、`ApplySettings`、`--command` 测试命令 |
| `bar.cpp/.h` | 栏：布局、输入、层级守护、渲染线程 `RenderSession`（背景来源选择 DDA/WGC、`ShowOwnWindowsToCapture`、弹窗玻璃 `renderPanels`） |
| `render/capture.*` | `BackdropSource` 接口；`MonitorCapture`（WGC）、`DuplicationCapture`（DDA，取帧线程 `AcquireNextFrame(0)` + 5 ms 轮询，交出帧不整屏复制）；`SetCaptureExclusion`；`PreferPowerSavingGpu` |
| `render/gpu.*` | 选扫描输出的 GPU；同一 GPU 被列两次时选列出该屏输出的那一项 |
| `render/glass_renderer.*`、`shaders/glass.hlsl` | 玻璃着色器；`GlassParams` **336 字节**（含 6 个融合部件 `parts`、`partRadii`、`melt`、`partMix`），与 HLSL cbuffer 一一对应 |
| `render/glass_panel.*` | `GlassPanel` / `GlassShape`（含 `parts`）/ `GlassHost`、墨色网格 `InkGrid` |
| `render/bar_painter.*` | 栏内容绘制与动画；状态图标 `DrawStatus`（单色 Wi-Fi） |
| `ui/surface.*` | 弹窗基础（D2D 内容 + 栏渲染线程画的玻璃）；`ShapeDip`（可带 `parts` 融合）；`SetDesktopLayer`（小组件：不置顶）；`SetCaptureVisible` / `SetPrivate`；`Canvas::Capsule/Arc/Image/...` |
| `ui/desktop_widgets.*` | 桌面小组件（布局、层级状态机 `Restack`、拖动、菜单、绘制） |
| `ui/control_center.*` | 控制中心（Main/Wifi/Bluetooth/Sound/Cast 五页，换页融合动画） |
| `ui/start_menu.*`、`ui/notifications.*`、`ui/tray_panel.*`、`ui/calendar_panel.*`、`ui/lock_screen.*` | 开始菜单、通知、托盘面板、日历、锁屏封面 |
| `shell/audio_devices.*` | 播放设备、`IPolicyConfig` 设默认、按程序的音量会话 |
| `shell/projection.*` | 无线显示器 `DisplayFinder`、`ProjectTo`、投影模式 |
| `shell/lunar.*` | `ChineseCalendar`（ICU 农历、节日）、`DaysIn`、`Weekday` |
| `shell/radios.*`、`shell/wifi.*`、`shell/system_status.*` | 无线电/蓝牙/亮度；WLAN；电池、音量、网络 |
| `shell/tray_model.*`、`shell/task_model.*`、`shell/app_catalog.*` | 托盘、任务、应用目录 |
| `shell/notification_listener.*`、`shell/wechat_source.*`、`shell/feishu_source.*` | 通知来源 |
| `settings.*` | 设置读写（`ReadTextFile` / `WriteTextFile` 原子写） |

设置中心：`settings/Atrium.Settings`（C# / WinUI 3 / .NET 10）。设置键见 [SETTINGS-PROTOCOL](SETTINGS-PROTOCOL.md)；C++ `settings.h` 与 C# `BarSettings.cs` 必须同步（C# 写回时会丢掉它不认识的键，所以小组件布局单独存 `widgets.json`）。

## 4. 构建、打包、推送

所有操作在用户电脑（devbox “pc”，`/mnt/d/Projects`）上执行。

```bash
# Core
cd /mnt/d/Projects/atrium-desktop/native
/mnt/d/Microsoft/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe --build build/win-x64 --config Release --target atrium_core

# 打包（先 --exit 用户的 Atrium；设置中心窗口开着时会拒绝打包）
PS=/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
$PS -NoProfile -Command "Start-Process 'D:\Projects\atrium-desktop\dist\Atrium\Atrium.Core.exe' -ArgumentList '--exit' -Wait"
$PS -NoProfile -ExecutionPolicy Bypass -Command "\$env:PATH = 'D:\Microsoft\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;' + \$env:LOCALAPPDATA + '\atrium-tools\dotnet;' + \$env:PATH; \$env:DOTNET_ROOT = \$env:LOCALAPPDATA + '\atrium-tools\dotnet'; & 'D:\Projects\atrium-desktop\tools\build.ps1' -Zip"
# 确认输出里有 "wrote ...zip"，再用 Start-Process 重新启动用户的 Atrium

# 推送（WSL 直连超时：Windows git + 本地代理 + gh 凭据）
G="/mnt/c/Program Files/Git/cmd/git.exe"; GH='C:/Users/Lenovo/AppData/Local/atrium-tools/gh/bin/gh.exe'
"$G" -c http.proxy=http://127.0.0.1:10808 -c credential.helper= -c "credential.helper=!'$GH' auth git-credential" push origin main
```

版本号三处同步：`native/CMakeLists.txt`、`native/core/core.rc`（4 处）、`settings/Atrium.Settings/Atrium.Settings.csproj`。每轮：改代码 → 截图核对 → 开发版测试 → 文档（DECISIONS / VERIFICATION / README / 本文）→ 升版本 → 打包 → 打包版回归 → 提交推送 → 中文汇报。

## 5. 测试

用 **Windows PowerShell 5** 完整路径运行（WSL 里的 `powershell.exe` 不行）。推荐用本地运行器（后台执行，逐个等用户空闲 >35 s，开跑前退出、结束后重启用户的 Atrium，并把用户的 `settings.json` / `widgets.json` 另存到 `%LOCALAPPDATA%\atrium-tools\user-backup\`、每个测试后还原）：

```bash
T='C:\Users\Lenovo\AppData\Local\atrium-tools'
nohup $PS -NoProfile -ExecutionPolicy Bypass -File "$T\\run-tests.ps1" -Core 'D:\Projects\atrium-desktop\dist\Atrium\Atrium.Core.exe' \
  -Tests 'e2e-settings,e2e-widgets,e2e-tray,e2e-start,e2e-notify,e2e-bar,e2e-motion,e2e-stacking,e2e-lock' \
  -Log "$T\\test-xxx.log" -Package >/dev/null 2>&1 &
# 轮询日志直到 "runner: done"；开发版测试把 -Core 换成 native\build\win-x64\Release\Atrium.Core.exe 并去掉 -Package
```

| 脚本 | 项数 | 内容 |
| --- | --- | --- |
| `e2e-settings.ps1` | 14 | 设置中心（UIA）；`-Package` 测打包版 |
| `e2e-widgets.ps1` | 9 | 小组件：读布局、层级、网格、真实鼠标拖动吸附并保存、改尺寸、添加、移除、关闭 |
| `e2e-tray.ps1` | 16 | 托盘面板与转发、日历、控制中心（蓝牙页、声音页、投屏页、音量滑块与滚轮、从喇叭拖动） |
| `e2e-start.ps1` | 13 | 开始菜单 |
| `e2e-notify.ps1` | 18 | 通知 |
| `e2e-bar.ps1` | 11 | 栏的点击、拖动、拖放、溢出 |
| `e2e-motion.ps1` | 8 | 动效与弹窗玻璃（`launch-bounce` 偶发取样过晚，见 VERIFICATION 28.5） |
| `e2e-stacking.ps1` | 5 | 层级守护 |
| `e2e-lock.ps1` | 5 | 锁屏封面（`ATRIUM_LOCK_DRYRUN=1`，**必须**） |
| `pipe-protocol.ps1` | 7 | 设置管道 |

- 测试设置文件都写 `"notifyListener":false,"notifyWeChat":false,"notifyFeishu":false,"widgets":false`（小组件测试除外）。
- 移动真实鼠标的脚本在用户不空闲时输出 `SKIP` 并退出（SKIP 不算通过）。
- 测试命令：`Atrium.Core.exe --command <verb> [arg]`。常用：`bar-state`；`start*`；`notify*`；`tray-*`；`calendar-*`；`lock-open|lock-state|lock-dismiss`；`control-open|control-state|control-volume <n>|control-click <id>`（id：`wifi-page`、`bt-page`、`sound-page`、`cast`、`back`、`out:<i>`、`amute:<i>`、`disp:<i>`、`mode:<i>`、`mute`；**`wifi`/`bt` 会真拨无线电、`mode:*` 会改投影模式、`disp:*` 会投屏，测试不要用**）；`widgets-state`、`widgets-do <add:kind:size|remove:i|size:i:size|restack|vivid:0/1>`。
- 环境变量：`ATRIUM_LOCK_DRYRUN=1`、`ATRIUM_LOCK_WALLPAPER=<图>`、`ATRIUM_START_LAYOUT=<json>`、`ATRIUM_PRIVACY=1`（截图隐藏个人信息）、`ATRIUM_WIDGETS_TEST=1`（小组件置顶、全彩/单色由 `vivid` 命令控制，截图用）、`ATRIUM_WIDGETS_LIGHT=0/1`、`ATRIUM_CAPTURE=wgc`（强制旧的 WGC 背景，对比用）。
- `--debug-capturable`：窗口始终可被截图；用 DDA 时背景实时，退回 WGC 时背景冻结在第一帧。`--debug-layout`：记录各项屏幕矩形。
- 日志：`%LOCALAPPDATA%\Atrium\logs\core.log`。渲染每分钟一行 `render: last minute ... work p50/p95/max`，改渲染/捕获后要看它有无退化（DDA 修复后基准：p50 0.6 ms、p95 12 ms）。
- 本地辅助脚本（`%LOCALAPPDATA%\atrium-tools\`，不在仓库）：`run-tests.ps1`、`idle.ps1`；截图 `cc-shot.ps1`/`cc2-shot.ps1`（控制中心各页）、`cc-anim.ps1`（换页连拍 12 帧）、`widgets-shot.ps1`（小组件全彩/单色）、`widgets-desk.ps1`（真实桌面 Win+D 层级）、`capture-shot.ps1`（非调试模式 GDI 截屏，验证截图兼容与背景实时）、`v07-shot.ps1` 等；性能 `blink.ps1` + `perf-compare.sh`；对比 `mot-compare.sh`；`switch-check.ps1`（切换输出设备并切回）；设置样例 `demo-v07.json` 等。截图拷到 `devshots/tmp/`（已被 .gitignore 忽略）再用 `remote_read_file` 看。

## 6. 调用 Codex（agent-control-bridge）

```bash
cd /mnt/c/Users/Lenovo/AppData/Local/atrium-tools/acb
N="/mnt/d/Download App/Agent Control Bridge/runtime/node.exe"
"$N" acb-call.mjs acb_codex_list '{"limit":5}'
"$N" acb-call.mjs acb_codex_send prompt.json      # {"threadId":..., "prompt":..., "wait":false}
"$N" acb-call.mjs acb_codex_read '{"threadId":"...","turns":1}'
```

atrium-desktop 的对话 `01a19124-4b29-7172-a13a-3f5b0122f0c1`（cwd 为本仓库）。只让它做只读审查/方案，并说明“不要修改文件、不要构建或运行 Atrium”。

## 7. 必须遵守的约束与坑

- 一切开发、构建、Git 都在用户电脑上用 devbox 工具执行。
- Atrium 单实例：测试/打包前 `--exit` 用户的 Atrium，之后用 `powershell.exe -Command Start-Process ...` 重启（`cmd.exe /c start` 会挂住）。
- **不要中途结束测试**（见 §1）。截图/实验前先等用户空闲 ≥30 s（`idle.ps1`），会遮住屏幕十几秒。
- 不提交含个人内容的截图；不提交仓库根目录用户的 `iPhoneDuo-原图-圆环状态栏.png`。
- 不注入 Explorer/DWM/LogonUI，不改系统通知设置，不添加根证书、不改信任策略；锁屏全替换只能在虚拟机验证（GPT 报告与用户要求）。
- 源码里 Segoe Fluent Icons 私有区字符一律写 `L"\uE721"` 转义（用 Python 改文件时注意别把它们吞掉）。
- 测试脚本 UTF-8 BOM + CRLF；用 Python 读写时用 `newline=''` 保留 CRLF。
- `GlassParams` 与 `glass.hlsl` 的 cbuffer 必须同步（`static_assert` 336 字节）。
- DDA：取帧线程绝不能在 `AcquireNextFrame` 里等待（设备多线程保护，会卡住渲染线程）；`Acquire()` 只释放上一次交出的帧。
- 双显卡：DXGI 只在进程所在 GPU 下列出屏幕；Atrium 首次运行写入 `HKCU\Software\Microsoft\DirectX\UserGpuPreferences`（本机 dist 与开发版 exe 都已写入）。
- 弹窗窗口 = 内容 + 阴影边距；`Surface::Rect()` 是内容区；控制中心窗口固定为最高页高度，命中坐标要减 `cardTop_`。
- 跨进程 `SendMessage`（`--command`）里不能发 COM 调用。
- 在 `Surface` 自己的消息处理中不能销毁该 Surface（小组件移除用 `PostMessage` 延后）。
- 本机：Windows 11 家庭中文版 26H2（26300），3200×2000 @200%，系统任务栏自动隐藏，Intel Arc（输出）+ RTX 4060；两台播放设备（Realtek、NICEHCK，默认 NICEHCK）；附近有一台无线显示器；单屏。
- 用户当前设置：`settings.json` 为 `{}`（事故后恢复的默认值，3 个固定项导入在内存中、下次保存时写入），`widgets.json` 不存在（默认 4 个小组件）。

## 8. 待办与未验证（按优先级建议）

1. **用户可能要的下一步**（等用户指示）：
   - 锁屏：预研报告 `D:\Projects\atrium-validation\2026-11-02-lockscreen-research\REPORT.md`（结论“部分可行”，推荐用户会话内 uiAccess 展示层叠在 LockApp 之上，需受信任代码签名 + Program Files；不建议 SYSTEM 安全桌面方案）。需用户同意安装 VMware/VirtualBox 后在虚拟机做 PoC。现有 v0.8 锁屏封面仍在开始菜单“锁定”里——用户不喜欢，可考虑改为直接 `LockWorkStation`（需先和用户确认）。
   - 小组件：天气（需网络与位置，先问用户）、更多 mac 组件、多显示器、拖动时的放置预览、“编辑小组件”画廊。
2. 未实机验证：控制中心投屏真正连接无线显示器、投影模式切换、Wi-Fi/蓝牙开关拨动（都会影响用户，需用户亲测）；OBS / ShareX / Windows 截图工具逐个实测（只验证了 GDI、WGC 两种底层方式）。
3. 已知限制：使用 DDA 的录屏软件拍不到 Atrium；小组件只在栏所在显示器；全屏判定只看前台窗口；多显示器整体未做；托盘不能转发双击；`e2e-motion` 的 `launch-bounce` 偶发取样过晚。
4. 通知：微信来源状态显示（未运行 / 列表不可读 / 已建立基线）尚未做（GPT round2 §5.2）；通用 Toast → QQ/飞书入站点击 → 微信 UIA 出卡的顺序见下。
5. 更早的未完成项：键盘与屏幕阅读器、窗口缩略图预览、HDR/高刷、Explorer 重启与睡眠唤醒、长时间稳定性、稳定版 Windows 11 验证、真正接管系统任务栏（D16，默认关闭）。

### 通知接入背景（仍有效）

先读仓库外本机报告：`D:\Projects\atrium-validation\2026-11-01\`（QQ/微信适配验证、通知/托盘/捕获可行性）与 `2026-11-01-round2\REPORT.md`。通用 Windows Toast 为主、特殊应用适配为辅；事件流“系统 Toast / 专用适配器 → `Notice` → 现有卡片”；复用 `notification_listener.*`、`Notice`、`WM_NOTICE`；`WM_NOTICE` 不应被误绑到系统监听开关。GitHub 参考：[Seelen UI 通知模块](https://github.com/eythaann/Seelen-UI/tree/master/src/background/modules/notifications)、[微软监听示例](https://github.com/WindowsNotifications/NotificationListenerActionCenterApp)、[wechat-toast](https://github.com/EthanAIDev/wechat-toast)。

### 仓库外资料（换机器交接需一并提供）

- `D:\Projects\atrium-validation\2026-11-01\`、`2026-11-01-round2\`（GPT 的验证报告与探针源码）
- `D:\Projects\atrium-validation\2026-11-02-lockscreen-research\REPORT.md`（锁屏预研）
- `D:\Projects\atrium-validation\2026-11-02-capture\`（截图兼容的探针结果）
- `%LOCALAPPDATA%\atrium-tools\`（本地脚本、测试日志、截图、Codex 桥接）
