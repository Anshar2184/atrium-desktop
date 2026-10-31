# DECISIONS · 当前有效决策

这是项目唯一的有效决策表。其他文档与本表冲突时以本表为准；被取代的方案在本表末尾和原文中标注 **已取代**，仅作历史记录。证据编号指向 [VERIFICATION](VERIFICATION.md)。

状态：**有效**（按此实施）· **实验**（可做开关功能，不阻塞主线）· **延后**（本阶段不做）· **待证**（方向已定，关键门槛未过）

更新：2026-10-31，v0.2 后。

| ID | 决策 | 状态 | 证据 |
| --- | --- | --- | --- |
| D01 | Core：C++20 / Win32 / C++/WinRT；D3D11 + DirectComposition；DirectWrite | 有效 | 技术路线；MSVC 14.44 已可构建原生代码 |
| D02 | 渲染设备建在**实际输出画面的适配器**上（混合显卡笔记本通常为核显）。已实现：`QueryDisplayConfig` 取该显示器扫描输出适配器的 LUID，`EnumAdapterByLuid` 建设备；不写系统 GPU 偏好 | 有效 | V1.10、V1.13、V13 |
| D03 | 背景来源：WGC 整屏捕获 + `WDA_EXCLUDEFROMCAPTURE`；DXGI 只做对照不进生产 | 有效 | V1.1、V1.10 |
| D04 | 自回声处理：用当前背景与**最近一次实际呈现的背景**做 GPU 精确比较（分块），只在不同时重绘；另设 2 秒最大陈旧时间；产品中比较结果异步回读（v0.1 仍为同步回读，每帧工作 p95 1.68 ms，待改） | 有效 | V1.7、V13 |
| D05 | 材质：一套 SDF + HLSL 管线，Liquid / Frosted 为参数预设 | 有效 | V1.4 |
| D06 | 截图：默认模式下系统截图不含玻璃，提供内置截图合成 | 有效 | V1.9 |
| D07 | 方案 C（逐窗口捕获重建背景，普通截图可见）：动态重建已成立，但必须自行合成其他窗口的 DWM 阴影，并在新窗口首帧到达前兜底 | 实验 | V2.x |
| D08 | 可视表面 = 可点穿的分层显示窗口（DComp）+ 带形状区域的隐形输入窗口；不用 `HTTRANSPARENT` | 有效 | V3 |
| D09 | 接管事务（协议 v2）：Core 建控制块（心跳、事务号、Guard 就绪、撤销标志）→ 等 Guard 就绪握手，失败则不接管 → 每一步在接管锁内先检查撤销、再写恢复状态文件（用户自动隐藏、可见性、工作区、AppBar、阶段、Core PID 与启动时间）、再执行：切自动隐藏 → 注册 AppBar → 隐藏任务栏 | 有效（单屏） | V4.9 |
| D10 | 恢复：**原生** `Atrium.Guard.exe`（静态 CRT，Per-Monitor V2 DPI，只依赖系统 DLL）等待 Core 进程句柄 + 心跳；卡死 3 秒判定后先撤销接管权、再结束 Core；恢复先撤销、在锁外等 Core 交还 AppBar，再持锁执行；只有核实任务栏可见、自动隐藏、工作区一致且 Core 不再持有 AppBar 才写 `restored`，否则记 `restore-pending` 留待重试；拿不到锁返回可重试错误；Guard 丢失时 Core 先释放 AppBar 再调用 `--restore`；下次启动按残留状态恢复 | 有效（单屏；Explorer 真实重启未测） | V4.9 |
| D11 | 系统入口：开始/搜索/快捷设置/通知中心通过公开快捷键调用，弹出位置由 Windows 决定 | 有效 | V4.7 |
| D12 | 托盘：Core 内自写**转发式桥接**。拷贝消息负载后由每个目标一个转发线程投递，桥接线程最多等 500 ms；失败和超时一律对应用返回 FALSE（超时后目标可能仍处理该消息，结果不确定）；不改 Explorer 任务栏 Z 序；广播 `TaskbarCreated` 时跳过 Explorer 自己的窗口；少数拿不到的旧图标提供“打开系统溢出区”兜底 | 待证（Explorer 重启与真实应用兼容矩阵未测） | V5.x |
| D13 | 任务列表：EnumWindows + WinEvent（合并 30 ms），AUMID 优先、其次程序路径分组；图标统一用 `IShellItemImageFactory`（.lnk / exe / `shell:AppsFolder\<AUMID>`）后台加载；点击先试 `SetForegroundWindow`，被拒绝时用 `SwitchToThisWindow`（不发送任何按键）；DWM 缩略图待做 | 有效 | V6、V14 |
| D14 | 桌面组件：公开顶层窗口 + `EVENT_SYSTEM_FOREGROUND` / `MINIMIZESTART` / `MINIMIZEEND` 状态机。桌面模式 = 所有应用窗口已最小化且前台不是应用窗口；不嵌入 WorkerW / Progman | 有效（单屏） | V7.x |
| D15 | 通知中心不进首版；包身份是否能让实时事件成立尚未对照验证 | 延后 | V8.x |
| D16 | 首个 C++ 纵切默认**不**自动接管系统任务栏（共存模式）；完整替换须在恢复、托盘门槛在稳定版系统上通过后由用户开启 | 有效 | 审查建议 |
| D17 | 许可证 MIT。可参考 Apache-2.0 / MIT 项目并保留声明；GPL / AGPL 项目只研究行为，不复制代码 | 有效 | LICENSE |
| D18 | 设置中心：C# / WinUI 3，**.NET 10**（LTS 至 2028-11；.NET 8/9 于 2026-11-10 停止支持），Windows App SDK 2.6.4 只引用 WinUI 组件，未打包、自包含，按需启动 | 有效 | V13 |
| D19 | 设置模型与协议：扁平 JSON `%LOCALAPPDATA%\Atrium\settings.json`；命名管道 `Atrium.Core.<会话>`，预览只改内存、保存原子写入、客户端断开撤销预览，见 [SETTINGS-PROTOCOL](SETTINGS-PROTOCOL.md) | 有效 | V13 |
| D20 | 分发：v0.1 为便携 zip（`tools/build.ps1`：Core + Guard + Settings），CI 产出构件；安装器、开机启动、签名与升级延后到 M4 | 有效 | V13 |
| D21 | 预设：液态参照 macOS Liquid Glass——几乎不染色、中等模糊、强折射（30 DIP）、宽边缘（24 DIP）、明显色散（0.30）、锐利边缘高光与轻微鲜艳度提升；磨砂为强模糊、弱折射。数值以 `settings.cpp` 为准 | 有效 | V14 |

## 已取代的方案

| 原方案 | 出处 | 取代为 | 原因 |
| --- | --- | --- | --- |
| ManagedShell 作为托盘依赖、独立 `Atrium.ShellBridge.exe` | ARCHITECTURE §1、TECH-DECISIONS T06、PROJECT-STRUCTURE §1/§3、WINDOWS-INTEGRATION §4 | D12 | 处理过的图标不转发 Explorer、改动 Explorer 置顶属性、Win11 上旧托盘读取路径无效（V5.1–5.5） |
| WorkerW / Progman 嵌入实验（`DesktopHostAdapter`） | TECH-DECISIONS T08、WINDOWS-INTEGRATION §6、RENDERING-DESIGN §3、ROADMAP P06 | D14 | 公开窗口状态机已覆盖 Win+D、显示桌面按钮、桌面图标等场景（V7） |
| 签名 MSIX 作为首版身份主线、右下角通知镜像面板 | TECH-DECISIONS T07/T11、WINDOWS-INTEGRATION §5、ROADMAP P05/M5 | D15 | 范围决定：首版不做通知中心 |
| WGC 与 DXGI “对照择优”、可能保留第二生产后端 | TECH-DECISIONS T04、RISKS §4、ROADMAP P01 | D03 | 双显卡下 DXGI 受限，WGC 全部达标 |
| 用“与上一帧捕获比较、阈值 6”的回声过滤 | 第一轮 VERIFICATION 1.7 | D04 | 缓慢变化会永久漏绘（审查第 2 条） |
| 桥接线程内直接 `SendMessageTimeout` 转发并在失败时返回成功 | 第一轮 `TrayHost` | D12 | 谎报成功；实测超时未生效，阻塞 3 秒（V5.9） |
| “前台变为桌面就提升组件” | 第一轮 P06 | D14 | 开着应用时点桌面会把组件提到应用上方（V7.4） |
| 设置中心用 .NET 8 | 第二轮 D18 | D18 | .NET 8 于 2026-11-10 停止支持 |
| 按 DXGI 输出所属适配器选设备 | 第一轮探测 `Gpu.ForMonitor` | D02 | Optimus 下 DXGI 把输出挂在独显上，实际扫描输出的是核显（V13） |
