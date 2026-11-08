# 质量审计修复与验收记录

日期：2026-11-07。基线为 [QUALITY-AUDIT-2026-11-07.md](QUALITY-AUDIT-2026-11-07.md) 与接手时的未提交工作区。保留原有改动，没有重置分支、推送、发布或修改 README。

## 交付状态

A01–A07、A09–A15 已实现。A08 已交付可运行原型与对比结果，原型未满足材质及输入约束，**未接入产品、未标记修复完成**，见 [A08 方案记录](G3-CLIPPING-PROPOSAL-2026-11-07.md)。

开发包：dist/Development/Atrium。启动身份同时由本地构建标记和 development.txt 保护；完整开发包保留 Uninstall.ps1，身份不依赖删除卸载脚本。

本轮仅用户明确确认后，将已经指向旧开发包的真实 Atrium 自启动值恢复为已核实存在的正式路径 D:\Download App\Atrium\Atrium.exe。没有修改正式安装文件；注册与失败注入测试均使用独立替身或隔离目录。

## 分批变更、证据和限制

| 批次 / 问题 | 实现及主要文件 | 验证 | 验证边界 |
| --- | --- | --- | --- |
| 1 · A01 身份 | native/CMakeLists.txt、settings.cpp、main.cpp、app_registration.cpp、CoreClient.cs、App.xaml.cs、csproj、tools/build.ps1：本地默认开发身份；正式打包显式关闭。开发版不写卸载注册、快捷方式或 Run；设置连接核对完整核心路径，另一位置实例明确提示；自启动开关在开发设置中说明并禁用 | 实际 settings/registration 源码在独立 HKCU 下测试开发/分发两种编译身份；开发保存不改模拟正式 Run、无快捷方式或卸载接管；分发注册写入隔离位置 | 未执行正式安装、卸载；真实 Run 仅按用户确认正常恢复，不做失败试验 |
| 1 · A02 退出 | system_chrome.h/.cpp、main.cpp：RestoreForExit 先不可逆关闭再次隐藏入口，再恢复系统界面，随后清理后台；WM_CLOSE、WM_ENDSESSION 和消息循环结束都覆盖 | 真实 SystemChrome 接独立 Shell 替身；查询完成门闩未释放时已恢复任务栏，后续 Enforce/SetTaskbarHidden(true) 不重藏；原开发核心退出已观察 | 未卡死真实 COM、Shell 服务或驱动；没有新增看门狗或每线程强杀 |
| 1 · A06 保存语义 | settings.cpp、settings_server.cpp、CoreClient.cs、MainWindow.xaml.cs：文件写入成功即接受 saved/current；自启动部分失败单独返回 warning。只在设置值改变或安装归属变更时改 Run；停用时不删除其他分发目录的启动项 | 独立 HKCU 注入自启动拒绝，磁盘重读仍与已接受保存快照一致并有 warning；客户端真实 ok/warning 回复确认保存；没有无回复成功路径 | 本次采用明确部分成功语义，不是跨文件/注册表事务；自启动失败仍需用户重试该设置 |
| 2 · A03 查询与写入 | control_center.h/.cpp：Radio/Audio/Wi-Fi 各一在途读取与一份合并待办；关闭/写入使旧读取失效。无线开关、输出选择有各自单个顺序写入工作者，最新意图合并，慢读取不阻塞写入；亮度只在接受有效结果后确认已读 | 真实控制中心源码接设备替身：延迟旧查询，201 次开关意图与 200 次重复读取，Radio 与 Audio 的读取/写入峰值分别为 1/1，最终 off/C；写入在被阻塞的读取释放前已执行；Wi-Fi 200 次扫描加关闭/重开只执行两次扫描、峰值 1 | 未切换用户真实无线、电台、音频输出；没有改变计时器、动画、布局或滑块事件节奏 |
| 2 · A04 投屏发现 | projection.cpp：通知目标与代次在同一锁规则下读取；watcher Start 完成后才发布；失效实例自行撤销五种订阅并 Stop，旧异常不得改新代次 | 实际 DisplayFinder 接 WinRT watcher 替身，延迟 Start 后 Stop、旧异常晚于新 Start，以及 100 轮 Start/Stop；所有旧 watcher 与订阅退役，新 searching 保留 | 无真实投屏连接；此测试验证生命周期与同步，不覆盖设备驱动兼容 |
| 2 · A05 IPC | CoreClient.cs、MainWindow.xaml.cs：5 秒总请求期限覆盖队列/写/读；断开取消所有排队请求；窗口关闭取消。失联提交不自动转成离线成功，状态不再把失联误报为核心已退出 | 唯一测试管道实际连接后不回复，4 个请求约 5019 ms 全部释放且无假保存；窗口生命周期取消约 0 ms；重新连接可提交并读出 warning。配置仍是一个在途预览与最新待办 | UI 暂未注入真实核心卡死；协议无回复可能已落盘，所以提示未确认并要求重新连接核对 |
| 3 · A07 ROI 边缘 | glass_renderer.h/.cpp、backdrop_edge.hlsl、CMake：GPU 复制当前边缘条带并用夹取延拓填充屏外区域；只画必要边缘。64 px 原点与 mip 相位保留 | 实际 GPU/WARP，白色与高对比图源，15 个常规/四边/四角/全屏外位置，4,423,680 个像素误差 0/255；原 MaterialProbe 的 mip0–6 漂移仍为 0.000/255，含预留释放 | 仅单屏离屏 GPU 验证；需要一张可复用的局部 ROI scratch，在边界首次分配，非全屏纹理，也非零显存代价 |
| 3 · A09 圆角缓存 | continuous_corners.h、surface.cpp：失效随共享设备对象最后释放；单个浮层隐藏、重建或析构不再清同设备缓存；仍为 64 项 / 4 MiB | 实际隐藏 Surface 使用真实 D2D 共享设备，25 次创建/Reset/析构只复用同一个 5,625 字节掩码；最后设备释放清除对应域 | 没有扩大缓存预算；不把 mask 预算当作总合成显存 |
| 3 · A10 图标缓存 | task_model.cpp、app_catalog.h/.cpp、start_menu.h/.cpp：清退出未固定身份及名称；历史尺寸按 16 MiB / 512 项逐出，保留当前尺寸、可见/固定及在途工作集；requested 在完成后释放；失败不存空图标，限定重试并在目录刷新后恢复机会 | 实际缓存逻辑接像素替身：Task 80 身份×9 尺寸，平台最多 16,777,216 像素字节、138 暖项；Catalog 200 身份×4 尺寸，137 项/16,777,216 字节；可见/固定保留、退出清理、失败后第二次有效请求恢复 | 预算限制可逐出历史项；受保护工作集本身超预算时保留它，未宣称进程总内存恒定。没有每次关应用清空全部图标 |
| 4 · A08 | 产品行为保持，源码/设计文档更正连续性声明；独立原型见专页 | 实际 WinUI 对比揭示 Acrylic 背景变白与候选输入命中失败；排除该路线 | 严格裁剪问题仍未修复，不能以有限折线或视觉接近验收 |
| 5 · A11 换照片 | MainWindow.Photos/Hero/Preview/Design.cs、MainWindow.xaml.cs：选择代次贯穿每次冻结 await、提交与回写；快照等待绑定目标照片代次，旧 Ready 不能释放新快照。固定色样使用现有 LatestWorker，最终回写检查照片代次；关闭停止工作 | 实际 WinUI 宿主链接产品源码，旧照片 Lazy 解码保持阻塞，连续选择纸张→几何→层叠后才放行；25 个图像缓冲全部对应最终照片，与实际方法重新渲染的完整缓冲哈希一致，无旧图残留、无悬挂快照；普通 UI 流程也通过。原 420 ms 淡出保持 | 注入仅在隔离宿主的旧解码边界；检查实际图像缓冲、代次和转场树，不冒充多显卡或所有系统条件验证 |
| 5 · A12 更新状态损坏 | ReleaseInstaller.cs：后台只读取一次已知任务身份；catch 独立写可读错误，不重复读取失败文件；区分不存在与 JSON null/损坏；保留未知 BITS 状态及半包 | 实际后台代码在专用临时更新目录验证截断 JSON、null、文件独占锁：可读失败结果、坏状态保留，恢复后可再次执行；没有调用未知 BITS 任务的取消 | 未对用户真实更新目录故障注入；不可写错误目录由应用日志补充记录 |
| 5 · A13 更新回滚 | tools/Update.ps1、main.cpp、settings_server.h/.cpp、App.xaml.cs、CoreClient.cs、MainWindow.xaml.cs：目录交换后等待核心 UI/管道与设置窗口/真实 state 握手；确认前保留一份旧包，失败停止本次新进程并恢复；恢复失败也留旧包；保留开发身份文件 | 全部真实文件交换、独立可执行演示包和命名事件：成功、第二次交换失败、核心启动失败、设置启动失败、无信号超时、恢复交换再失败，共 6 场景在 PowerShell 7 与产品实际使用的 Windows PowerShell 5.1 均通过；设置与 ready 状态按预期保留；成功只清本次临时目录 | 故障注入对象为隔离演示包；用户选择核心保持关闭时只验证设置启动，保留该运行偏好。未升级正式安装 |
| 5 · A14 更新卡片 | update_checker.h/.cpp、notifications.h/.cpp、main.cpp：横幅确认与 ready 卡片分离；UI 同步接受后才写提醒标记；重启静默恢复一张可操作卡片，安装后清理 | 实际 UpdateChecker/Notifications + 独立 ready 目录：首次一张待弹横幅、重建通知中心后一张静默卡片、重复完成无计数增长、点击动作有效、ready 移除后卡片消失 | 非持久化所有通知；未扩展聊天或通知兼容策略 |
| 6 · A15 文档 | DESIGN.md、SETTINGS-PROTOCOL.md、MainWindow.Design.cs、shader 与旧 VERIFICATION 注释：目标与实现分开，说明双侧握把、EaseOut 改向和帧内分配边界；修复无效材料文档路径 | 本地链接、源码对应说明、差异空白检查；保留动画与胶囊比例 | 性能预算仍是目标；不宣称所有参数、融合拓扑或外部窗口均已数学 G3 |

## 已验收拉伸回归

- WidgetResizeProbe：8 类小组件、100/125/150/200% 缩放、544 个中间帧，轮廓 alpha 最大差 1/255。
- 180 帧连续伸缩与左握把反向：宿主固定、对侧固定、旧确认过滤、最新尺寸提交、缓冲复用与释放通过。
- 同轮 WARP 绘制平均 1.558 ms、最大 4.949 ms；同时有编译负载，不能与旧审计耗时直接比较或宣称性能提升，也不表示整机帧率。
- MaterialProbe：mip0–6 均为 0.000/255 漂移，屏外白色 ROI 不再保留洋红。

## 构建、实际启动和系统归属

原生 Release、C# Release、开发分发打包均已通过。本机缺少的 .NET SDK 使用 [Microsoft 官方安装脚本](https://learn.microsoft.com/en-us/dotnet/core/install/windows) 安装在 Git 忽略的 devshots/quality-audit-fixes/dotnet，版本 10.0.403；没有改目标框架或安装到系统目录。

开发包实际验证：核心（本轮 PID 9736）通过真实命名事件确认 UI/管道就绪；设置中心（本轮 PID 42044）在窗口加载且收到同目录核心真实 state 后确认，验证完毕后已正常关闭，开发核心保持运行。实际窗口显示“正在运行”；流线→纸张→几何→层叠后，主预览、放大镜和三张材质色样最终均为层叠，保存按钮仍禁用，没有改写配置。系统页实际开机启动开关禁用，关于页保留版本显示、检查结果与“检查更新”按钮。真实 Run 保持用户批准的正式路径，正式卸载注册字段及快捷方式文件的哈希与本轮验证前一致。

启动检查曾发现桌面工具把显式开发路径解析成注册的 Atrium.Desktop 并启动正式核心；已关闭刚启动的那个正式实例，随后改用经进程路径校验的精确可执行路径启动开发包。没有替换正式文件。后续验证使用精确路径及命名就绪事件。

源代码基线副本、差异和所有临时探针均在 devshots/quality-audit-fixes，不进入 CMake 产品目标或发布包。旧开发包备份为同目录 previous-development。

## 换图集成回归与附带兼容修复

隔离 WinUI 宿主直接链接实际 MainWindow、Photo、渲染与 XAML 源码。仅替换旧照片的 Lazy 解码边界为门闩，保持旧解码阻塞到最新选择已提交后 1.2 秒；没有复制一套渲染器作为测试。覆盖主预览底图与玻璃、放大镜、三张材质色样、通知前后两张卡片、小组件地图、14 张图库卡片和设计页两张材质对比图，共 25 个缓冲。最终结果与产品方法重新渲染最新照片的结果一致，旧图像素没有回写，冻结字典及视觉树中的转场快照均已清空。去掉逐区排查步骤后，正常页面加载流程再次通过。结果在 devshots/quality-audit-fixes/photo-probe-run-matched/photo-audit-result.json。

这次完整遍历还实际复现并修复了两处原有兼容问题：

- ContinuousCorners.cs 原先对图库按钮及设计页动画元素调用 GetElementVisual 取得合成器，会与 XAML 的 Translation/TranslationTransition 冲突；现在从已连接的窗口根节点取得同一个合成器，焦点框仍添加到原控件。没有移除卡片 180 ms 位移动画。
- MainWindow.Design.cs 材质对比将 XAML 分界 Clip 与圆角合成 Clip 置于同一元素，布局时触发原生 E_NOINTERFACE。分界矩形现放在一个无内边距的独立 Grid 上，材质图片和圆角仍在原 Border 内，保持两者的交集。阴影、照片、材质参数和分界位置保持。

平台对 GetElementVisual 与 SetElementChildVisual 的职责划分见 [Microsoft 的 XAML / Visual Layer 互操作说明](https://learn.microsoft.com/en-us/windows/apps/develop/composition/using-the-visual-layer-with-xaml)。上述是复现异常后的兼容修复，**不是 A08 解析裁剪替代实现**。

最新开发包已重新发布 C# Release（无警告/错误）。实际设置进程 PID 32164 通过核心 state / 窗口就绪事件；设计页保持两种玻璃与阴影，分界线可拖动，图库可打开并显示键盘焦点框，保存按钮保持禁用。该设置窗口与隔离测试窗口均已关闭；开发核心 PID 9736 保持运行。

## 后续：侧边栏切换短暂变紫

实际复现于 ContinuousCorners.cs 的颜色动画：默认 Auto 插值在灰色/透明与蓝色之间经过了紫色。颜色关键帧现明确使用 CompositionColorSpace.Rgb，原侧边栏 140 ms 过渡、状态终点、圆角和焦点行为保留。该修复位于共用的颜色过渡路径，同类控件的中性色/强调色切换也使用相同规则。

独立 WinUI 宿主直接加载产品 App.xaml、NavItemStyle、ContinuousCorners 和 NavLabel，通过 RenderTargetBitmap 读取实际渲染帧，没有屏幕捕获会话。浅色、深色各验证普通→选中、按下→按下选中、选中→普通、悬停→悬停选中四种过渡，各取 32 帧；旧代码分别出现 8 / 6 个偏紫色采样，修复后均为 0，最终选中像素仍为原 #0A84FF。中间帧证据及 JSON 在 devshots/quality-audit-fixes/nav-probe-before 与 nav-probe-after；不同运行的取样时刻不完全相同，不用于比较帧耗时。

插值 API 依据见 [Microsoft ColorKeyFrameAnimation 文档](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.composition.colorkeyframeanimation?view=windows-app-sdk-2.0)。C# Release 构建通过；只更新开发分发包的 Atrium.Settings.dll / pdb，原文件备份于 nav-probe/binaries-before。设置窗口无待保存更改后正常关闭并重开，新设置进程 PID 9980 完成窗口与实际核心 state 就绪确认，核心仍为 PID 29240。正式版和系统主题设置未改动。

## 后续：窗口预览关闭与名称气泡（2026-11-08）

窗口预览原先只在任务栏继续悬停时收到窗口列表更新；指针进入预览后，已关闭或已隐藏的窗口仍可能留下卡片。现在 TaskModel 每次通知直接刷新当前预览，按 HWND 保留仍存在的卡片与缩略图，移除关闭或托盘隐藏的窗口，最后一张卡片消失时收起。关闭按钮和中键发送标准 WM_SYSCOMMAND / SC_CLOSE，只有成功排入消息队列才记录请求；应用原有的保存确认、取消或隐藏到托盘行为保留。按下目标按 HWND 跟随，列表重排或窗口消失不会误关另一张卡片；刷新不会延长原 320 ms 离开期限。

独立原生探针直接编译实际 WindowPreview / TaskModel 并连接实际 Surface，使用一次性窗口验证 10 个场景：销毁、隐藏到托盘、只处理系统关闭命令、取消关闭、多窗口仅移除目标、按下后重排、中键关闭、刷新期间离开期限、按下目标被移除、无效目标。旧代码复现失败，修复后全部通过；气泡的共用 Surface 修改完成后已重新编译并再次通过。证据位于 devshots/preview-close-fix/verification.log。未自动关闭用户正在使用的第三方窗口，不将替身验证描述为所有软件兼容性测试。

名称气泡使用现有 Surface / PopVisual 出现及消失动画，沿用 kPop 的 0.92→1 缩放、160 ms 缓出淡入，以及关闭时 130 ms 缩放 / 120 ms 淡出；没有新增缓动公式。主体位置、文字、尺寸与悬停判定保持，底部增加 5 DIP 小尖角，屏幕边缘夹取气泡主体时尖角仍指向图标。尖角接入同一玻璃距离场，因此模糊、折射与阴影沿完整轮廓绘制。无玻璃时使用相同解析边界的一份缓存掩码，避免每个动画帧重新生成掩码。

实际 Bar 输入消息与 Surface 的探针覆盖快速划过 8 个图标、450 ms 停留、连续 20 次切换、出场中断、左右屏幕边缘和窗口预览交接；均无过期气泡、重新冒出或错误锚点，重复内容更新不重播出现动画。实际离屏渲染图已检查。WARP 对比 100/125/150/200% 缩放下的 91,720 个像素，尖角关闭时新旧玻璃材质差异为 0/255；尖角开启时，按着色器的导数抗锯齿规则核对解析覆盖，alpha 最大误差 0/255。记录与图像位于 devshots/hover-label-fix；没有使用屏幕捕获来作此验证。

原生 Release 再次构建通过，WidgetResizeProbe 的 8 类 / 544 帧、180 帧伸缩与反向握把检查仍通过，轮廓 alpha 最大误差 1/255；MaterialProbe 的 mip0–6 漂移仍为 0.000/255。这里只验证对应交互和渲染回归，不据此宣称整机动画帧率达标。

最新核心已复制到 dist/Development/Atrium，构建与开发包哈希一致；核心 PID 32312 通过 UI / 管道就绪事件，设置 PID 41832 完成窗口与同目录核心实际 state 握手。设置显示“正在运行”且保存按钮禁用，验证后正常关闭，核心继续运行。未改版本号、未 git commit；正式 Run 仍为用户已批准的路径。

## 后续：数字时钟偶发省略（2026-11-08）

已使用产品实际 MiSans、Canvas 与 PaintWidget 离屏复现截图中的 11:58→“11:…”：小号数字时钟按“00:00”恰好填满 136 DIP 列宽，其他数字组合及字形取整存在不足一像素的差异，触发 DirectWrite 字符省略。此次在固定全天字号的计算中留出 1 DIP 宽度余量，文字仍在原列中居中；设置中心 WidgetFaces.cs 使用相同余量。没有按当前时间改变字号，也没有取消截断后让文字溢出。

直接运行产品 PaintWidget 后读取其实际文字格式，以同一个 DirectWrite 排版路径穷举全天 1,440 分钟，覆盖 small / medium 与 100/125/150/200% 缩放，共 11,520 次检查。旧实现 1,254 次被省略，新实现为 0，文字自然宽度均在列内。实际 11:58 的修复前后离屏绘制也已查看；记录与 PNG 位于 devshots/digital-time-fix/before.log、after.log、before-11-58.png、after-11-58.png。没有修改系统时间或使用屏幕捕获。

原生 Release 与 C# Release 构建通过，后者 0 警告 / 0 错误。开发包已更新 Atrium.exe 与设置 DLL / PDB，核心 PID 40516 通过就绪事件；设置 PID 42836 完成窗口和同目录核心 state 握手。原设置窗口保存按钮禁用后正常关闭再重开。正式安装、自启动、版本号与提交状态保持原有约束。

## 后续：设置中心小组件预览错位及来源（2026-11-08）

实际复现：画廊卡片缩窄 14 DIP 后，14 种预览全部向右偏移，200% 系统缩放下中心偏差为 14 像素。背景 ImageBrush 随新容器宽度拉伸，而内容 Canvas 仍使用上一次渲染的坐标和尺寸。旧 SizeChanged 处理会跳过不足 16 DIP 的宽度变化，因此停止缩窗后错位也不会自行恢复；异步新图未完成期间也存在同类失步。

引入点为 2026-11-03 的提交 a9d94aa90669755d1b951b68fd423234230537d1（0.9.12）：将同一 Viewbox 内的玻璃与内容改为 CPU 背景图和独立 Canvas，并增加 16 DIP 更新阈值。随后 806f845399b768e93251378a64e8b7f9116739f8 的预览分配优化沿用了这一处理。修复前 MainWindow.Widgets.cs 与本次审计接手基线的文件哈希相同；最近数字时钟修复只调整 WidgetFaces 中的字号余量。来源证据及复现数值位于 devshots/widget-layout-fix/cause-evidence.json。之前的验收覆盖了原生小组件裁剪和时钟文字，但漏掉了设置画廊小幅缩窗这一场景。

修复涉及 MainWindow.Widgets.cs、MainWindow.Preview.cs、MainWindow.xaml：每次尺寸变化都提交最新渲染请求，继续沿用已有的按目标合并待办和最多两个并行绘制；画廊的内容层使用与已完成背景图对应的固定视口，由外层 Viewbox 在等待新图期间同步缩放。桌面布局缩略图采用同样处理，新背景与新内容坐标在同一回调内更新。没有改变玻璃参数、小组件内容布局、原生桌面宿主或动画参数。

独立 WinUI 宿主链接实际 MainWindow / WidgetFaces / XAML / 玻璃渲染源码，检查全部 14 种画廊预览和现有 7 个桌面布局预览，覆盖 ±1/7/14/15.5 DIP 的小幅尺寸变化、20 次连续改向、980/1560/2320/1880 像素窗口宽度、三次换图及布局缩略图尺寸变化。在新图尚未应用和渲染完成两个阶段检查玻璃与内容边界，共 63 个阶段、756 组几何检查，最大差异 0.506 像素，来自 XAML 布局取整；全部通过。实际系统缩放为 200%，本次未修改 Windows DPI。修复前后 14 张离屏绘制均已查看，记录在 devshots/widget-layout-fix/verified-stress/layout-result.json，复现图在 before-shrink，修复图在 after-shrink / verified-stress。未使用屏幕捕获。

原生 WidgetResizeProbe 重新链接当前构建：8 类、四档缩放、544 个中间帧及 180 帧连续拉伸 / 左握把反向检查通过，轮廓 alpha 最大误差仍为 1/255。C# Release 完整发布通过，开发设置包按发布产物更新 DLL、PDB 和编译后的 XAML 资源 Atrium.Settings.pri；正式安装文件及版本号保持。

## O01–O07 的处理

只落实了 O06：add-widget 的短消息改传 kind/size 两个值，消除 SendMessageTimeout 超时后的局部字符串生命周期依赖。没有建立通用消息/任务框架。

O01、O02、O03、O04、O07 尚无本轮热点、带宽或长期日志成本测量，不据静态代码或文件数量进行性能改写。O05 没有证据表明实际尺寸动画跨越近圆/正圆边界，保留静态圆形与现有参数。可见性、布局、刷新频率、实时桌面背景、材质、动画时长/弹性均未用于换取性能。

## 尚需的验收

A08 的替代实现与用户可见对比；实际设备响应、真实更新安装、系统注销、多显示器、驱动卡死以及长期内存/日志压力仍未全覆盖。原审计的限制继续适用，不据本轮通过的局部探针宣称整个项目无问题。
