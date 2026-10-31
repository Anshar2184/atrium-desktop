# SETTINGS-PROTOCOL · 设置协议

`Atrium.Settings.exe`（C# / WinUI 3，按需启动）通过命名管道调整 `Atrium.Core.exe` 的外观（D18）。两边共享同一份设置模型：`native/core/settings.h` 与 `settings/Atrium.Settings/BarSettings.cs`，键名、范围、预设必须同步修改。

## 设置文件

`%LOCALAPPDATA%\Atrium\settings.json`，UTF-8，一个扁平 JSON 对象，原子替换写入。缺少的键取默认值，超出范围的值被截断，格式错误的文件整体回退到默认值（Core 日志会记录）。

| 键 | 类型 | 范围 | 默认（液态） | 说明 |
| --- | --- | --- | --- | --- |
| `version` | number | 1 | 1 | 架构版本 |
| `preset` | string | `liquid` / `frosted` / `custom` | `liquid` | 仅用于界面显示 |
| `transparency` | number | 0–1 | 0.90 | 1 = 完全不染色；着色混合 = 1 − transparency |
| `blur` | number | 0–1 | 0.35 | 模糊混合比例，同时决定 mip 级别 |
| `refraction` | number | 0–60 DIP | 13 | 边缘最大采样偏移 |
| `edge` | number | 1–80 DIP | 17 | 发生折射的边缘宽度 |
| `dispersion` | number | 0–0.5 | 0.08 | 通道分离比例 |
| `highlight` | number | 0–1 | 0.55 | 边缘高光 |
| `shadow` | number | 0–1 | 0.28 | 投影浓度 |
| `tint` | string | `#RRGGBB` | `#FFFFFF` | 染色 |
| `cornerRadius` | number | 0–200 DIP | 28 | 绘制时不超过高度的一半 |
| `width` | number | 200–4000 DIP | 720 | 不超过工作区 |
| `height` | number | 32–160 DIP | 56 | |
| `margin` | number | 0–400 DIP | 10 | 与工作区底边的距离 |
| `showClock` | bool | | true | 时钟与日期 |

磨砂预设：`transparency 0.78, blur 1, refraction 0, edge 1, dispersion 0, highlight 0.30, tint #F5F5FA`。

## 管道

- 名称：`\\.\pipe\Atrium.Core.<会话 ID>`；消息模式；拒绝远程客户端；Core 以 `FILE_FLAG_FIRST_PIPE_INSTANCE` 创建，其他进程无法预先占用名称。
- Settings 连接后用 `GetNamedPipeServerProcessId` 核对对端进程是 `Atrium.Core`。
- 同一时间只服务一个客户端。每条请求是一个 UTF-8 JSON 消息，对应一条回复。

| 请求 | 回复 | 行为 |
| --- | --- | --- |
| `{"op":"get"}` | `{"op":"state","protocol":1,"saved":{…},"current":{…},"path":"…"}` | 已保存值与当前生效值（可能含未保存预览） |
| `{"op":"preview","settings":{…}}` | `{"op":"ok"}` | 只改内存，立即重绘；可只发部分键 |
| `{"op":"commit","settings":{…}}` | `{"op":"ok"}` 或 `{"op":"error","message":…}` | 写文件成功后才更新已保存值 |
| `{"op":"cancel"}` | `{"op":"ok"}` | 恢复到已保存值 |

客户端断开时若当前值与已保存值不同，Core 自动撤销预览（预览租约）。Settings 端拖动滑块时同一时间只有一个预览请求在途，返回后再发送最新状态，不积压。

Core 未运行时，Settings 直接读写设置文件，Core 下次启动生效。

## Core 命令行

| 参数 | 作用 |
| --- | --- |
| （无） | 启动；若已在运行，则打开设置窗口 |
| `--exit` | 请正在运行的实例退出，最多等 10 秒（安装、升级用） |
| `--debug-capturable` | 仅开发：不排除捕获并冻结背景，用于截取真实画面 |
