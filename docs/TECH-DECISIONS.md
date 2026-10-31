# TECH-DECISIONS

本文件所有“采用”均为架构提案；没有任何参考库已经安装或成为仓库依赖。信心高表示 API 契约/源码明确，中表示路径合理但组合待测，低表示依赖私有行为或缺乏验证。事实与推断分开。来源索引见 [SOURCES](SOURCES.md)。

## 1. 决策记录

| ID | 推荐决策 | 取舍、替代与重开条件 | 证据 / 信心 |
| --- | --- | --- | --- |
| T01 | Core 为 C++20 / Win32 / C++/WinRT | HWND、GPU、WinRT 可直接互操作；成本是内存/COM 与自绘无障碍开发。Rust 可行，但不为当前需求额外引入语言和绑定维护 | D01、D03、R04–R07；高 |
| T02 | D3D11，feature level 11_0 为首版目标 | capture 与 D2D 互操作直接；D3D12 的显式同步、descriptor、资源状态对小面积合成收益不明确。只有实测 CPU 提交瓶颈才重开 | D01、D02、D04；高 |
| T03 | Core 仅采用 DirectComposition 宿主 | 自己绘制纹理，DComp 管 visual、clip、opacity 和 HWND 合成。它不是玻璃引擎，也不提供任意背景 SRV | D01、D08；高 |
| T04 | **已取代 → D03**（WGC 生产、DXGI 仅对照）：WGC 为首选候选，DXGI Duplication 做同接口 A/B 验证 | WGC 便于 FP16/HDR，Win32 monitor interop；边框授权、帧队列和实际功耗需测。DXGI 提供明确脏矩形但输出/适配器约束更强。只在证据证明必要时保留第二生产后端 | D02–D06；中 |
| T05 | 一套 SDF 形状和 HLSL 管线，两种材质参数集 | Liquid 是实时纹理的屏幕空间近似光学折射，不是物理光线追踪。不能以堆色散替代视觉验证 | R04–R07 源码；高（方法），中（最终视觉） |
| T06 | **已取代 → D12**（自写转发式桥接，不用 ManagedShell）：不把 ManagedShell 默认装入原生 Core | 它是 C#/WPF 库，托盘涉及私有 COM/窗口协议。若 P04 通过可用独立 .NET 桥接复用；C++/CLI 会耦合运行时与崩溃域，不推荐 | R01、R02；高（边界），中低（兼容） |
| T07 | **已取代 → D15**（通知中心延后）：默认通知补充面板，原生 Toast 保留 | App SDK 的发送能力不是全局拦截器。监听模式不能无损代理任意动作 | D09、D10、R03；高 |
| T08 | **已取代 → D14**（只用公开窗口 + 事件状态机）：桌面组件公开 top-level 模式先行，WorkerW 实验隔离 | 公开 Z-order 不保证 Win+D 持续可见；嵌入满足桌面层更接近但私有且受 DPI/自捕获限制 | D13、R08；高（风险），中低（嵌入） |
| T09 | Settings 选 C# / WinUI 3 | 原生控件、键盘/无障碍效率较好；引入 .NET + App SDK，但按需启动不计入空闲常驻。若团队坚持单语言可改 C++/WinRT WinUI 3，接受 XAML 工具链成本 | R09、D03；中高 |
| T10 | CMake 管 native；MSBuild/NuGet 管 WinUI 和包 | 不硬把 WinUI XAML 编译塞进 CMake；顶层脚本编排二者。vcpkg manifest 固定 baseline，依赖尽少 | R09 构建与 Issues；高 |
| T11 | **已取代 → D15**（延后）：签名 MSIX 包为身份验证主线 | 身份使通知权限和部署可测试，但不等于所有 capability 已获批；先用本地测试签名验证。external-location identity package 为传统安装器后备 | D09、D11；中，P05 决定 |
| T12 | **已实现 → D10**：原生 Guard 常驻，先恢复后重启 | 多一个很小的进程换取 Core 崩溃后恢复；无 Windows 服务，无管理员运行。双进程一起终止不保证即时恢复 | R01/R02 的隐藏行为 + 自主故障域设计；高（必要性），中（时限待测） |

> 下表是 2026-10-31 首次评审时的选型记录。之后的有效决策以 [DECISIONS](DECISIONS.md) 为准，被取代的行已在行首标注。

## 2. 渲染与合成 API 不是同一层的选项

| 技术 | 能力与限制 | 本项目角色 |
| --- | --- | --- |
| D3D11 | 纹理/着色器/光栅化；驱动隐式管理更多资源同步。WGC 帧为 D3D11 surface | 首选像素渲染 |
| D3D12 | 更强提交控制与显式资源管理；WGC 接入往往还需 D3D11 互操作，跨 API 同步增加复杂性 | 暂不采用；不是画质升级开关 |
| DirectComposition | COM visual tree、交换链与 DWM 合成；无任意背景纹理访问承诺 | Core 唯一合成宿主 |
| Windows.UI.Composition | OS WinRT 视觉层，可 Win32 托管、动画/brush/effect 丰富，需要 DispatcherQueue 等初始化 | P02 对照；仅当动画/布局收益明确才整体替换宿主 |
| Microsoft.UI.Composition | App SDK 对应视觉层，与 WinUI 3 配套；部署版本、对象体系不同于 OS 命名空间 | Settings 内部使用；不与 Core 的 visual 对象直接混接 |

组合是 D3D11 **加** DirectComposition，不是二选一。Windows Composition 能持有 D3D 渲染 surface，不意味着可以把任意桌面 backdrop 导出成 HLSL SRV。不要假设 Acrylic/Mica、BackdropBrush 或 HostBackdropBrush 自动提供可编程折射输入；Mica 也不能作为真实窗口背景折射的替代证明。P02 要对照 alpha、clip、帧延迟、失效恢复，而不是仅成功显示一个矩形。[官方交换链契约](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition)、[Win32 Composition](https://learn.microsoft.com/en-us/windows/uwp/composition/using-the-visual-layer-with-win32)。

## 3. 背景来源比较

| 来源 | 优点 | 关键代价 | 结论 |
| --- | --- | --- | --- |
| WGC monitor capture | WinRT 帧池，公开 monitor interop，HDR FP16 路径 | 系统捕获指示及无边框授权；缓冲过多增加时延；不能假定所有目标 SDK 都有相同脏区域能力；跨 GPU 可能隐式复制 | P01 首测，按功能探测新属性 |
| DXGI Desktop Duplication | GPU surface、dirty/move rect；适合 ROI 更新 | device 必须属于输出适配器；AccessLost、旋转、指针、受保护内容、远程与虚拟输出都需处理；基础接口 BGRA 与 DuplicateOutput1 格式协商要区分 | P01 对照，不预设一定比 WGC 快 |
| 壁纸纹理 | 不需连续屏幕捕获，低功耗、易录屏 | 没有前景应用、视频、图标或动态壁纸真实合成；需复现各屏 wallpaper placement | 明确标记“壁纸玻璃”的降级 |

WGC/DXGI 通常取得完整输出，裁 ROI 降低的是后续拷贝/滤波量，不能宣传操作系统只捕获了玻璃那一小块。捕获排除为窗口级设置，不是“只对 Atrium 自己的捕获隐藏”；仅排除某个调用方而保留普通录屏没有本方案可依赖的公开契约。详见 D02–D06。

## 4. 设置中心比较

| 候选 | 开发效率 / 视觉 | 开销 / 集成 | 判断 |
| --- | --- | --- | --- |
| C# WinUI 3 | 表单、绑定、可访问性与 Windows 视觉一致；自定义玻璃预览交给 Core | CLR 和 App SDK 安装/冷启动成本；MSBuild/NuGet 与 CMake 分离 | 推荐，关闭后完全退出 |
| C++ WinUI 3 | 同一控件体系，少一种业务语言 | XAML、WinRT 生成、生命周期代码较繁；仍不是纯 CMake 工程 | 团队语言偏好驱动的替代 |
| Qt Widgets / Qt Quick | 工具成熟、跨平台；Quick 擅长动态 UI | 引入 Qt 部署与样式协调；Qt Quick 再带渲染体系，透明预览仍需复用 Core；模块许可不同 | 无跨平台需求，收益不足 |
| 自绘 Win32 UI | 自由度高、运行时小 | 输入法、焦点、文本选择、读屏、表单布局都要补齐 | 只用于紧急恢复小窗口，不做完整 Settings |

不对上述方案凭直觉报内存数字；P08 记录冷启动、私有工作集、CPU 空闲与完整功能开发成本。Qt 许可按所选模块核对，[Qt 官方许可说明](https://doc.qt.io/qt-6/licensing.html)。

## 5. 九个参考项目的使用结论

本次读取默认分支 README、根目录/许可证、相关实现与当前开放 Issues 列表。Issues 是报告而非已复现缺陷；未做完整安全、历史或依赖审计。源码链接定位见 SOURCES。

| 项目 | 实际看到的实现 | 许可证与复用判断 |
| --- | --- | --- |
| RetroBar | C#/WPF + ManagedShell；Taskbar.xaml.cs 管任务栏、缩略图/自动隐藏集成；README 明确开始菜单与通知区域功能 | Apache-2.0；研究任务栏生命周期，可有选择借鉴，保留声明。不直接拿整套旧式 UI 当核心 |
| ManagedShell | ExplorerHelper 保存状态、设置自动隐藏、隐藏主副任务栏并 100 ms 检查；ExplorerTrayService 用 ITrayNotify 和旧 toolbar 进程内存读取路径；TrayService 模拟窗口协议 | Apache-2.0；唯一优先试验的托盘兼容依赖。不是官方兼容层，且必须 audit 实际启用路径和副作用 |
| Seelen-UI | Rust/Tauri；hook_dll 处理窗口消息；notifications/wpn_service.rs 读 wpndatabase.db；动作处理源码注明私有 broker 约束 | AGPL-3.0；本方案只研究行为与故障案例，不复制代码；产品未确定接受其许可，且数据库/注入路径不符合架构原则 |
| Rainmeter | Library/System.cpp 明确记录 Win11 24H2 前后 Progman/WorkerW 层级变化；Skin.cpp 有桌面/Z-order 生命周期 | GPL-2.0 根许可证，文件另有声明；只研究行为，不复制实现至未决定许可证的 Atrium。不是玻璃引擎依赖 |
| VMonoShiki/ImGui | backdrop.cpp 有 DXGI 和 WDA 排除，README 是 source release；Dual Kawase + 自定义 HLSL | 根 MIT，应用及字体/FreeType 另有声明；研究 GPU 管线，不能视为成熟 UI SDK，不引入 ImGui 作为最终无障碍 UI |
| JetComX/WindowsLiquidGlass | LiquidGlassShaders.h 有 SDF 位移、RGB 色散；LiquidGlass.cpp 的背景入口为颜色/图片和内部 RT；已查核心不能证明全桌面实时捕获 | Apache-2.0；可研究着色器，不作为现成实时桌面玻璃库。README 的多实例/ROI 路线图也提示工程缺口 |
| han1548772930/liquid-glass | Rust main.rs 的 DXGI、SDF 和 RGB 采样明确；示例宣称 HDR，未运行验证；main 同一 device 枚举输出不等于跨适配器已解决 | 根目录未发现 LICENSE，metadata 无许可标识，README 无授权条款；仅研究公开描述与原理，不复制代码/位移资源，需先取得明确授权 |
| hicccc77/electron-liquid-glass | native capture.cc/renderer.cc/panel 构成 DXGI+D3D11+DComp；确有位移采样；README ~6 ms 为作者特定实验，且多屏跟随第一个可见 panel | MIT；研究调度/ROI；不带入 Electron。capture.cc 显式偏好 BGRA，不把“支持 Windows”解释成完整 HDR/多 GPU 支持 |
| WindowsAppSDK-Samples | SceneGraph、Notifications、Windowing、Mica、部署示例；样例自身明确包含早期原型 | MIT；可直接参考官方互操作/打包样例，不整体依赖 samples，不把 Windows Widgets 示例视为任意桌面小组件宿主 |

生产可依赖候选只有 Windows SDK/系统 API、稳定版 App SDK/.NET、少量固定版本基础库，（**已取代**：原文还包括“经 P04 验证后的 ManagedShell”，P04 后改为自写桥接，见 D12）。其他玻璃项目均不是首版运行时依赖。MIT/Apache 也要保留版权及适用 NOTICE、核查资产与传递依赖；GPL/AGPL 并非“禁止使用”，项目许可证已定为 MIT（D17），此处是复用边界，不作法律结论。

## 6. Issues 转化成测试，而非口碑评分

- [ManagedShell #145](https://github.com/cairoshell/ManagedShell/issues/145)：自动隐藏栏冲突 → P03 检验同边 AppBar 竞争。
- [ManagedShell #142](https://github.com/cairoshell/ManagedShell/issues/142)：任务状态不同步 → P04 覆盖窗口风格变化、关闭与重用句柄。
- [RetroBar #1497](https://github.com/dremin/RetroBar/issues/1497)、[#1509](https://github.com/dremin/RetroBar/issues/1509)：全屏与系统状态图标 → 不把第三方托盘等同于系统状态实现。
- [Seelen #1881](https://github.com/eythaann/Seelen-UI/issues/1881)、[#1874](https://github.com/eythaann/Seelen-UI/issues/1874)、[#1953](https://github.com/eythaann/Seelen-UI/issues/1953)：通知激活、休眠崩溃、频繁预览更新 → 分别进入 P05、P07、P09。
- [Rainmeter #496](https://github.com/rainmeter/rainmeter/issues/496)：外屏布局漂移 → 持久化相对位置与 DIP。
- [AppSDK Samples #558](https://github.com/microsoft/WindowsAppSDK-Samples/issues/558)、[#552](https://github.com/microsoft/WindowsAppSDK-Samples/issues/552)：SDK 版本不一致 → 精确锁定工具链，不依赖 README 中宽泛最低版本。

四个玻璃演示库的本次开放 Issues 接口均返回空数组；这不能推导出没有 bug、有充分用户验证或生产成熟。正式选定依赖时固定 commit/package version，补做关闭 Issues、release 和传递许可证审计。
