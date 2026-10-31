# Atrium Desktop

**面向 Windows 11 的原生玻璃桌面环境**：用实时折射渲染的悬浮任务栏和桌面小组件，共享一套设计语言。保留 Explorer 和 Windows Shell，只替换你看到的那一层。

[English](README.md)

> **状态：可行性已验证，产品尚未开发。** 仓库目前包含架构评审文档、验证报告，以及在真实桌面上验证各项高风险 Windows 集成的探测程序，还没有可安装的应用。

## 验证结论

完整数据、替代方案和决策见 [docs/VERIFICATION.md](docs/VERIFICATION.md)。简要如下：

- **实时液态玻璃**：WGC 捕获 + 排除自身 + D3D11 着色可行，核显每帧 0.16 ms；自回声可以过滤。
- **截图可见**：用逐窗口捕获重建背景，普通系统截图也能看到玻璃。
- **点击穿透**：可点穿的显示窗口 + 带形状区域的输入窗口。
- **任务栏接管与崩溃恢复**：守护进程 128 ms 恢复系统任务栏。
- **系统入口**：任务栏隐藏后开始菜单、搜索、快捷设置、通知中心均可正常使用。
- **托盘**：自研转发式桥接可用，少数不响应重建消息的旧图标需要兜底。
- **桌面组件**：公开接口即可在 Win+D 时保持可见，不嵌入 WorkerW。

尚未验证：多显示器、HDR、高刷新率、Explorer 重启、睡眠唤醒、长时间稳定性，以及稳定版 Windows 11。

## 文档

| 文档 | 内容 |
| --- | --- |
| [ARCHITECTURE](docs/ARCHITECTURE.md) | 最小架构、进程、线程、数据流和配置 |
| [VERIFICATION](docs/VERIFICATION.md) | 可行性验证清单与结果 |
| [TECH-DECISIONS](docs/TECH-DECISIONS.md) | 选型、证据、信心程度、九个参考项目 |
| [RENDERING-DESIGN](docs/RENDERING-DESIGN.md) | 双材质、采样、GPU 管线、性能和降级 |
| [WINDOWS-INTEGRATION](docs/WINDOWS-INTEGRATION.md) | 任务栏、托盘、通知、桌面层与恢复 |
| [PROJECT-STRUCTURE](docs/PROJECT-STRUCTURE.md) | 建议目录、构建与最少接口 |
| [ROADMAP](docs/ROADMAP.md) | 独立 PoC、测试步骤、开发里程碑 |
| [RISKS](docs/RISKS.md) | 失败条件、备选和发布门槛 |
| [SOURCES](docs/SOURCES.md) | 官方文档、源码、许可证和 Issues 检索范围 |

## 运行探测程序

需要 Windows 10 19041+（推荐 Windows 11）和 .NET 8 SDK。部分探测会隐藏任务栏或模拟点击与按键，运行前请先阅读 [poc/README.md](poc/README.md)。

## 许可证

[MIT](LICENSE)。视觉灵感来自 Apple 的 Liquid Glass；本项目与 Apple、Microsoft 无关。
