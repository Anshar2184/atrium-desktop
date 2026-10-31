# SOURCES · 证据与检索边界

检索日期：2026-10-31，Asia/Macau。GitHub 通过已连接的 GitHub 工具读取仓库 metadata、默认分支目录、README、LICENSE、指定源码和开放 Issues；Microsoft/Qt 文档在线读取。没有下载/执行这些项目，没有本机 GPU 或 Shell PoC，也没有完整代码审计。

文档中的 API 事实来自官方契约；参考项目只证明它采用过某种技术路径。架构、参数、进程拆分和验收阈值是本项目提案。对未公开实现的“可行”绝不等于 Windows 官方保证。

## 官方资料

| 编号 | 资料 | 本设计采用的证据 |
| --- | --- | --- |
| D01 | [CreateSwapChainForComposition](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition) | D3D11 device / D3D12 queue 输入区别；composition swap chain、flip sequential 与 scaling 约束 |
| D02 | [Screen capture](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture) | WGC 帧池、捕获提示、HDR FP16 注意事项 |
| D03 | [Using the Visual Layer with Win32](https://learn.microsoft.com/en-us/windows/uwp/composition/using-the-visual-layer-with-win32) | Win32 中 Windows.UI.Composition 和 C++/WinRT 互操作 |
| D04 | [DuplicateOutput](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-duplicateoutput) | device 与输出适配器归属要求、失败条件 |
| D05 | [SetWindowDisplayAffinity](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity) | 当前进程顶层 HWND、WDA_EXCLUDEFROMCAPTURE 语义、不是安全隔离保证 |
| D06 | [Desktop Duplication API](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api) | dirty/move 更新顺序、旋转/指针、基础图像格式 |
| D07 | [Application Desktop Toolbars](https://learn.microsoft.com/en-us/windows/win32/shell/application-desktop-toolbars) | AppBar 注册、保留区与自动隐藏协作，不是 Shell 替换 API |
| D08 | [DirectComposition architecture](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components) | 应用 visual tree 与系统合成职责 |
| D09 | [Notification listener](https://learn.microsoft.com/en-us/windows/apps/develop/notifications/app-notifications/notification-listener) | capability、用户授权、读取/删除、事件后状态同步 |
| D10 | [App notifications overview](https://learn.microsoft.com/en-us/windows/apps/develop/notifications/app-notifications/) | App SDK 发送/管理本应用通知；与跨应用监听职责不同 |
| D11 | [Package identity with external location](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/grant-identity-to-nonpackaged-apps) | 稀疏/外部位置包身份及 manifest/注册步骤，不等同全功能权限 |
| D12 | [Shell_NotifyIcon](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyicona) | 客户端通知区消息，不是枚举全局图标服务 |
| D13 | [SetParent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setparent) | child/pop-up 样式与跨 DPI 父级风险 |
| D14 | [SHQueryUserNotificationState](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shqueryusernotificationstate) | 判断是否适合打扰用户的公开信号 |
| D15 | [SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput) | 输入注入受 UIPI 和现有键状态约束 |
| D16 | [DWM thumbnail overview](https://learn.microsoft.com/en-us/windows/win32/dwm/thumbnail-ovw) | live thumbnail 到目标 HWND，不是任意 shader 纹理 |
| D17 | [CreateForMonitor](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createformonitor) | Win32 公开 monitor capture item interop |
| D18 | [IsBorderRequired](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired) | 关闭边框前的用户同意要求 |
| D19 | [SetWinEventHook](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook) | OUTOFCONTEXT 不映射回调 DLL 到目标进程 |
| D20 | [High DPI desktop development](https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows) | per-monitor DPI 场景与动态缩放 |
| D21 | [Qt licensing](https://doc.qt.io/qt-6/licensing.html) | 许可选项与不同模块许可差异 |

部分 Microsoft 页面包含通用登录提示，但工具仍返回正文；以上结论依据返回的 API 正文。不存在用登录提示推断 API 不可用的判断。未成功读取的旧通知 overview URL 已改用 D10；未成功读取的 CreateBackdropBrush 页面不作为已验证能力依据。

## 参考仓库与已读源码

链接使用检索当日默认分支，可能随上游变化。本次只读调研没有生成可重复构建的 vendor lock；正式复用前必须固定 commit、版本与许可证清单。后文根文件 blob 标识可辅助核对本次样本，但不等于仓库提交版本。

### R01 · RetroBar（master）

[仓库/README](https://github.com/dremin/RetroBar)、[Apache-2.0 LICENSE](https://github.com/dremin/RetroBar/blob/master/LICENSE)、[Taskbar.xaml.cs](https://github.com/dremin/RetroBar/blob/master/RetroBar/Taskbar.xaml.cs)。读取任务栏生命周期、开始菜单监视、尺寸/模糊和托盘位置调用；没有运行完整应用。

开放 Issues 样本：[1497 全屏](https://github.com/dremin/RetroBar/issues/1497)、[1509 状态图标](https://github.com/dremin/RetroBar/issues/1509)、[1489 启动/原栏缺失报告](https://github.com/dremin/RetroBar/issues/1489)。最后一项用户环境含定制 Windows，不能当标准 Win11 已复现故障。

### R02 · ManagedShell（master）

[README](https://github.com/cairoshell/ManagedShell/blob/master/README.md)、[LICENSE](https://github.com/cairoshell/ManagedShell/blob/master/LICENSE)、[ExplorerHelper.cs](https://github.com/cairoshell/ManagedShell/blob/master/src/ManagedShell.AppBar/ExplorerHelper.cs)、[ExplorerTrayService.cs](https://github.com/cairoshell/ManagedShell/blob/master/src/ManagedShell.WindowsTray/ExplorerTrayService.cs)、[TrayService.cs](https://github.com/cairoshell/ManagedShell/blob/master/src/ManagedShell.WindowsTray/TrayService.cs)。

ExplorerHelper 可见状态保存/自动隐藏/定时校验；ExplorerTrayService 可见 ITrayNotify 和 VirtualAllocEx/ReadProcessMemory 旧路径。后者不等于已发现大范围代码注入，但明确不是仅使用公开托盘枚举接口。

Issues：[145 自动隐藏冲突](https://github.com/cairoshell/ManagedShell/issues/145)、[142 状态同步](https://github.com/cairoshell/ManagedShell/issues/142)、[97 多屏保留区](https://github.com/cairoshell/ManagedShell/issues/97)、[114 初始化异常](https://github.com/cairoshell/ManagedShell/issues/114)。

### R03 · Seelen-UI（master）

[README](https://github.com/eythaann/Seelen-UI/blob/master/README.md)、[AGPL LICENSE](https://github.com/eythaann/Seelen-UI/blob/master/LICENSE)、[hook_dll lib.rs](https://github.com/eythaann/Seelen-UI/blob/master/src/hook_dll/src/lib.rs)、[system_tray infrastructure.rs](https://github.com/eythaann/Seelen-UI/blob/master/src/background/modules/system_tray/infrastructure.rs)、[notifications infrastructure.rs](https://github.com/eythaann/Seelen-UI/blob/master/src/background/modules/notifications/infrastructure.rs)、[wpn_service.rs](https://github.com/eythaann/Seelen-UI/blob/master/src/background/modules/notifications/wpn_service.rs)。

源码存在窗口消息 hook 处理、通知数据库读取与激活回退。这里只说明被检查路径，不声称每项功能始终靠注入，也不复制它的实现。

Issues：[1881 通知动作](https://github.com/eythaann/Seelen-UI/issues/1881)、[1874 休眠崩溃](https://github.com/eythaann/Seelen-UI/issues/1874)、[1953 CPU/预览](https://github.com/eythaann/Seelen-UI/issues/1953)。

### R04 · VMonoShiki/ImGui（master）

[README](https://github.com/VMonoShiki/ImGui/blob/master/README.md)、[根 LICENSE.txt](https://github.com/VMonoShiki/ImGui/blob/master/LICENSE.txt)、[第三方说明](https://github.com/VMonoShiki/ImGui/blob/master/examples/example_win32_directx11/THIRD-PARTY-NOTICES.md)、[backdrop.cpp](https://github.com/VMonoShiki/ImGui/blob/master/examples/example_win32_directx11/src/glass/backdrop.cpp)、[shaders.h](https://github.com/VMonoShiki/ImGui/blob/master/examples/example_win32_directx11/src/glass/shaders.h)。可见排除、自桌面采样、模糊和色散路径；未验证多适配器和长期稳定性。

### R05 · JetComX/WindowsLiquidGlass（main）

[README](https://github.com/JetComX/WindowsLiquidGlass/blob/main/README.md)、[LICENSE](https://github.com/JetComX/WindowsLiquidGlass/blob/main/LICENSE)、[LiquidGlass.cpp](https://github.com/JetComX/WindowsLiquidGlass/blob/main/LiquidGlass.cpp)、[LiquidGlassShaders.h](https://github.com/JetComX/WindowsLiquidGlass/blob/main/LiquidGlassShaders.h)。

折射 shader 有真实纹理位移；背景来源与桌面采样是不同问题。此结论限于已读公开 renderer 核心，不声称仓库每个示例永远没有捕获能力。根目录 API 当日返回 blob：LiquidGlass.cpp `f6a63612838d1600cad32dae8b3613c8ce2886c5`，LiquidGlassShaders.h `ed9f8571c748b436d7d41cb24f702839a18e2021`。

### R06 · han1548772930/liquid-glass（main）

[README](https://github.com/han1548772930/liquid-glass/blob/main/README.md)、[Cargo.toml](https://github.com/han1548772930/liquid-glass/blob/main/Cargo.toml)、[main.rs](https://github.com/han1548772930/liquid-glass/blob/main/src/main.rs)、[根目录](https://github.com/han1548772930/liquid-glass)。Cargo.toml 无 license 字段，根目录无 LICENSE，metadata 无许可证。示例 HDR 为 README 陈述，本次没有完整读取/运行 frosted_liquid 示例，故不把它列为已验证 HDR 实现。

### R07 · hicccc77/electron-liquid-glass（main）

[README](https://github.com/hicccc77/electron-liquid-glass/blob/main/README.md)、[MIT LICENSE](https://github.com/hicccc77/electron-liquid-glass/blob/main/LICENSE)、[capture.cc](https://github.com/hicccc77/electron-liquid-glass/blob/main/src/capture.cc)、[renderer.cc](https://github.com/hicccc77/electron-liquid-glass/blob/main/src/renderer.cc)。可见 DXGI 和 D3D11 位移采样；README 说明 DComp、capture exclusion 和单首个 panel 的显示器策略。对其调度与全链路性能未进行完整审计，尤其不沿用作者 ~6 ms 的测量作为 Atrium 数据。

### R08 · Rainmeter（master）

[README](https://github.com/rainmeter/rainmeter/blob/master/README.md)、[GPL LICENSE](https://github.com/rainmeter/rainmeter/blob/master/LICENSE)、[System.cpp](https://github.com/rainmeter/rainmeter/blob/master/Library/System.cpp)、[Skin.cpp](https://github.com/rainmeter/rainmeter/blob/master/Library/Skin.cpp)。前者桌面结构段明确记录 24H2 前后差异；大文件为定向检查相关段落，并非逐行全库审计。

Issues：[496 布局跨分辨率](https://github.com/rainmeter/rainmeter/issues/496)、[365 游戏模式](https://github.com/rainmeter/rainmeter/issues/365)。

### R09 · WindowsAppSDK-Samples（main）

[README](https://github.com/microsoft/WindowsAppSDK-Samples/blob/main/README.md)、[MIT LICENSE](https://github.com/microsoft/WindowsAppSDK-Samples/blob/main/LICENSE)、[SceneGraph README](https://github.com/microsoft/WindowsAppSDK-Samples/blob/main/Samples/SceneGraph/README.md)、[MainWindow.xaml.cs](https://github.com/microsoft/WindowsAppSDK-Samples/blob/main/Samples/SceneGraph/SampleGalleryApp/MainWindow.xaml.cs)、[SampleGalleryApp.csproj](https://github.com/microsoft/WindowsAppSDK-Samples/blob/main/Samples/SceneGraph/SampleGalleryApp/SampleGalleryApp.csproj)。检查 WinUI 项目结构和互操作入口，没有编译所有样例。

Issues：[558 SDK 包版本](https://github.com/microsoft/WindowsAppSDK-Samples/issues/558)、[552 多版本 SDK](https://github.com/microsoft/WindowsAppSDK-Samples/issues/552)、[584 Widget 状态](https://github.com/microsoft/WindowsAppSDK-Samples/issues/584)。

## Issues 与置信度限制

本次查询的是各仓库开放 Issues 默认返回页（包含 PR，分析时排除 PR），不是全历史。四个玻璃项目 R04/R05/R06/R07 返回空列表；不排除曾关闭的 Issues、其他讨论渠道或未报告缺陷。相关错误是否已在某个 release 修复，没有据此作确定判断。正式依赖决策必须补查版本对应的开放/关闭问题和维护情况。

源码/文档审查确认的技术路径不计为 PoC Passed。所有工程性能、部署成功率、Windows 兼容性仍以 ROADMAP 的实际结果为准。
