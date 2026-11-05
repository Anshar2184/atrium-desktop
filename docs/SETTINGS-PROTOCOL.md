# SETTINGS-PROTOCOL · 设置协议

`Atrium.Settings.exe`（C# / WinUI 3，按需启动）通过命名管道调整 `Atrium.exe` 的外观（D18）。两边共享同一份设置模型：`native/core/settings.h` 与 `settings/Atrium.Settings/BarSettings.cs`，键名、范围、预设必须同步修改。

## 设置文件

`%LOCALAPPDATA%\Atrium\settings.json`，UTF-8，一个扁平 JSON 对象，原子替换写入。缺少的键取默认值，超出范围的值被截断，格式错误的文件整体回退到默认值（Core 日志会记录）。

| 键 | 类型 | 范围 | 默认（液态，参照 macOS Liquid Glass） | 说明 |
| --- | --- | --- | --- | --- |
| `version` | number | 1–2 | 2 | 架构版本。2（v0.6）：读取版本 1 的文件时忽略其中的 `notifyListener`（版本 1 保存的是旧默认值“关”），即迁移为开启；没有 `version` 的输入（管道的部分更新）不迁移 |
| `preset` | string | `liquid` / `frosted` / `custom` | `liquid` | 仅用于界面显示 |
| `transparency` | number | 0–1 | 0.95 | 1 = 完全不染色；着色混合 = 1 − transparency |
| `blur` | number | 0–1 | 0.50 | 模糊强度：主要决定 mip 级别（1 + 4.5 × blur），低值时与清晰采样混合 |
| `refraction` | number | 0–60 DIP | 30 | 边缘最大采样偏移 |
| `edge` | number | 1–80 DIP | 24 | 发生折射的边缘宽度 |
| `dispersion` | number | 0–0.5 | 0.30 | 通道分离比例 |
| `highlight` | number | 0–1 | 0.80 | 边缘高光与玻璃下内容的鲜艳度 |
| `shadow` | number | 0–1 | 0.30 | 投影浓度 |
| `tint` | string | `#RRGGBB` | `#FFFFFF` | 染色 |
| `cornerRadius` | number | 0–200 DIP | 30 | 绘制时不超过高度的一半 |
| `width` | number | 200–4000 DIP | 1200 | 最大宽度，不超过工作区；`autoWidth` 关闭时为固定宽度 |
| `height` | number | 32–160 DIP | 60 | |
| `margin` | number | 0–400 DIP | 10 | 与工作区底边的距离 |
| `showClock` | bool | | true | 时钟与日期 |
| `showTray` | bool | | true | 系统托盘入口 |
| `showStatus` | bool | | true | 状态环与控制中心入口 |
| `showNotifications` | bool | | true | 通知入口；关闭入口不关闭通知来源 |
| `textColor` | string | `auto` / `light` / `dark` | `auto` | 文字颜色；`auto` 按玻璃下方的实际亮度选择 |
| `iconSize` | number | 16–64 DIP | 30 | 任务栏图标边长 |
| `magnify` | number | 1–2 | 1.7 | 悬停放大：指针下图标的缩放倍数，相邻图标按波形放大；1 为关闭 |
| `autoWidth` | bool | | true | 宽度随内容伸缩，最大为 `width` |
| `showRunning` | bool | | true | 显示未固定但正在运行的应用 |
| `notifyTimeout` | number | 3–30 s | 6 | 通知弹出后停留的时间（指针停在卡片上时不计时） |
| `notifyHideBody` | bool | | false | 隐藏通知正文；同时把通知窗口排除在截图和屏幕共享之外 |
| `notifyListener` | bool | | true | 通过 UserNotificationListener 把 Windows 通知显示为 Atrium 卡片（系统横幅仍会出现，除非用户在 Windows 里关闭该应用的横幅） |
| `notifyWeChat` | bool | | true | 实验：读取微信窗口的会话列表（UI 自动化，只读），非免打扰会话有新的未读消息时显示卡片 |
| `notifyFeishu` | bool | | true | 读取飞书自己的消息弹窗里的文字（UI 自动化，只读），同时显示为 Atrium 卡片 |
| `widgets` | bool | | true | 桌面小组件；布局由 Core 存于 `widgets.json`（D35） |
| `widgetsOnRight` | bool | | false | 小组件逻辑网格从右侧排布；切换时镜像现有列，不改写 `widgets.json` |
| `widgetBackdrop` | string | `desktop` / `wallpaper` | `desktop` | 小组件玻璃的背景：实时抓取桌面窗口，或直接读取壁纸图片（省电）；都不含上层窗口（D38） |
| `hideTaskbar` | bool | | false | Atrium 运行时隐藏系统任务栏（并设为自动隐藏），关闭或退出时恢复（D37） |
| `hideBanners` | bool | | false | 打开“请勿打扰”（仅闹钟）去掉系统横幅，通知仍显示为 Atrium 卡片；关闭或退出时恢复（D37） |
| `startOnLogin` | bool | | true | 当前用户开机自启；与本次启动或关闭任务栏独立，保存时同步 Windows 开机项 |
| `pins` | array | | 见下 | 固定的应用，按顺序显示 |

`pins` 的每一项：`{"name": "显示名", "path": ".lnk / .exe 路径，可空", "args": "附加参数，可空", "aumid": "AppUserModelID，可空", "multipleWindows": false}`。`multipleWindows` 可省略，浏览器与 Windows 文件管理器默认允许新窗口，聊天、音乐及其他应用默认打开已有页面；中键与“打开新窗口”遵循此设置。浏览器仅按常用程序文件名识别（Chrome、Edge、Firefox、Brave、Opera、Vivaldi、Chromium），不扫描协议注册或按显示名推断。主窗口关闭到后台时，通过应用自己的托盘入口恢复，不强制显示隐藏窗口。识别同一应用时 `aumid` 优先，其次匹配启动器与实际程序；多个固定项共用的程序路径不作模糊归组。设置文件里**没有** `pins` 键时（首次运行），Core 导入用户当前 Windows 任务栏的固定项（`%APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar\*.lnk`），`get` 返回的 `current` / `saved` 中包含导入结果；保存后写入文件，之后不再导入。

预设只改材质键（transparency、blur、refraction、edge、dispersion、highlight、shadow、tint），不改布局与内容键。预设由 `BarSettings.cs` 的 `Liquid()` / `Frosted()` 定义，液态默认值与 `native/core/settings.h` 保持一致。

## 管道

- 名称：`\\.\pipe\Atrium.Core.<会话 ID>`；消息模式；拒绝远程客户端；Core 以 `FILE_FLAG_FIRST_PIPE_INSTANCE` 创建，其他进程无法预先占用名称。
- Settings 连接后用 `GetNamedPipeServerProcessId` 核对对端进程是 `Atrium`。管道名称沿用内部标识 `Atrium.Core`，不受可执行文件改名影响。
- 同一时间只服务一个客户端。每条请求是一个 UTF-8 JSON 消息，对应一条回复。

| 请求 | 回复 | 行为 |
| --- | --- | --- |
| `{"op":"get"}` | `{"op":"state","protocol":1,"saved":{…},"current":{…},"path":"…"}` | 已保存值与当前生效值（可能含未保存预览） |
| `{"op":"preview","settings":{…}}` | `{"op":"ok"}` | 只改内存，立即重绘；可只发部分键 |
| `{"op":"commit","settings":{…}}` | `{"op":"ok"}` 或 `{"op":"error","message":…}` | 写文件成功后才更新已保存值 |
| `{"op":"cancel"}` | `{"op":"ok"}` | 恢复到已保存值 |
| `{"op":"add-widget","kind":"clock","size":"small"}` | `{"op":"ok"}` 或 `{"op":"error","message":…}` | 添加小组件并保存布局；不改变设置预览租约 |

客户端断开时若当前值与已保存值不同，Core 自动撤销预览（预览租约）。Settings 端拖动滑块时同一时间只有一个预览请求在途，返回后再发送最新状态，不积压。

Core 未运行时，Settings 直接读写设置文件，Core 下次启动生效。

## Core 命令行

| 参数 | 作用 |
| --- | --- |
| （无） | 启动；若已在运行，则打开设置窗口 |
| `--exit` | 请正在运行的实例退出，最多等 10 秒（安装、升级用） |
| `--debug-capturable` | 仅开发：不排除捕获并冻结背景，用于截取真实画面 |
