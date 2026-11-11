#include "localization.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string_view>
#include <unordered_map>

namespace atrium {

namespace {

struct Entry {
    const wchar_t* zh;
    const wchar_t* en;
};

// Chinese source text -> English. This is the single place the native
// interface's English wording lives; call sites name the Chinese string.
const Entry kEnglish[] = {
    // main.cpp: the bar's menu, app menus and startup messages.
    {L"没有找到 Atrium.Settings.exe。请确认安装目录中的 Settings 文件夹完整。",
     L"Atrium.Settings.exe was not found. Make sure the Settings folder in the installation directory is complete."},
    {L"外观设置…", L"Appearance Settings…"},
    {L"显示桌面", L"Show Desktop"},
    {L"任务管理器", L"Task Manager"},
    {L"添加小组件…", L"Add Widgets…"},
    {L"任务栏", L"Taskbar"},
    {L"桌面小组件", L"Desktop Widgets"},
    {L"台前调度", L"Stage Manager"},
    {L"退出 Atrium", L"Quit Atrium"},
    {L"打开", L"Open"},
    {L"打开新窗口", L"Open New Window"},
    {L"打开文件所在的位置", L"Open File Location"},
    {L"从 Atrium 取消固定", L"Unpin from Atrium"},
    {L"固定到 Atrium", L"Pin to Atrium"},
    {L"关闭所有窗口", L"Close All Windows"},
    {L"关闭窗口", L"Close Window"},
    {L"关闭程序", L"Close Program"},
    {L"卸载 Atrium？个人设置和桌面小组件布局将会保留。",
     L"Uninstall Atrium? Your personal settings and desktop widget layout will be kept."},
    {L"另一位置的 Atrium 正在运行。请先退出该实例，再启动本目录中的版本。",
     L"Atrium is running from another location. Quit that instance, then start the version in this folder."},
    // settings.cpp
    {L"设置已保存，但未能修改 Windows 开机启动。请重新切换开机启动后再保存。",
     L"Settings were saved, but Windows startup could not be changed. Toggle Start with Windows again, then save."},
    {L"有线网络", L"Ethernet"},
    {L"没有无线网卡", L"No Wi-Fi adapter"},
    {L"已关闭", L"Off"},
    {L"已连接", L"Connected"},
    {L"未连接", L"Not connected"},
    {L"不可用", L"Unavailable"},
    {L" 台设备已连接", L" devices connected"},
    {L"已打开", L"On"},
    {L"蓝牙", L"Bluetooth"},
    {L"已充满", L"Fully charged"},
    {L"正在充电", L"Charging"},
    {L"剩余 ", L""},
    {L" 小时", L" hours left"},
    {L" 分钟", L" minutes left"},
    {L"使用电池", L"On battery"},
    {L"电源", L"Power"},
    {L"已接通电源", L"Plugged in"},
    {L"投屏", L"Cast"},
    {L"无线显示器", L"Wireless display"},
    {L"已打开 · 最近使用的应用在右侧", L"On · Recent apps on the right"},
    {L"已打开 · 最近使用的应用在左侧", L"On · Recent apps on the left"},
    {L"显示器", L"Display"},
    {L"不支持调节", L"Not adjustable"},
    {L"没有播放设备", L"No output device"},
    {L"静音", L"Muted"},
    {L"声音", L"Sound"},
    {L"Wi-Fi 已关闭", L"Wi-Fi is off"},
    {L"没有找到 Wi-Fi 网络", L"No Wi-Fi networks found"},
    {L"正在搜索网络…", L"Searching for networks…"},
    {L"断开", L"Disconnect"},
    {L"输入密码…", L"Enter password…"},
    {L"连接", L"Connect"},
    {L"网络和 Internet 设置", L"Network & Internet settings"},
    {L"更多 Wi-Fi 网络", L"More Wi-Fi networks"},
    {L"这台电脑没有蓝牙", L"This PC has no Bluetooth"},
    {L"蓝牙已关闭", L"Bluetooth is off"},
    {L"没有已配对的设备", L"No paired devices"},
    {L"正在读取设备…", L"Reading devices…"},
    {L"蓝牙设备 ", L"Bluetooth device "},
    {L"蓝牙和其他设备设置", L"Bluetooth & devices settings"},
    {L"声音输出", L"Sound output"},
    {L"正在读取…", L"Reading…"},
    {L"音量合成器", L"Volume mixer"},
    {L"没有应用在播放声音", L"No apps are playing sound"},
    {L"更多音量设置", L"More sound settings"},
    {L"音量合成器设置", L"Volume mixer settings"},
    {L"正在搜索无线显示器…", L"Searching for wireless displays…"},
    {L"没有找到无线显示器", L"No wireless displays found"},
    {L"无线显示器 ", L"Wireless display "},
    {L"正在连接…", L"Connecting…"},
    {L"投影模式", L"Projection mode"},
    {L"只连接了一个显示器", L"Only one display is connected"},
    {L"仅电脑屏幕", L"PC screen only"},
    {L"复制", L"Duplicate"},
    {L"扩展", L"Extend"},
    {L"仅第二屏幕", L"Second screen only"},
    {L"投影到此电脑", L"Project to this PC"},
    {L"系统投屏面板", L"System cast panel"},
    {L"用户", L"User"},
    {L"搜索应用和文件夹", L"Search apps and folders"},
    {L"返回", L"Back"},
    {L"全部应用", L"All apps"},
    {L"还没有固定的应用。右键单击搜索结果或“全部应用”里的应用来固定", L"Nothing is pinned yet. Right-click an app in the search results or All apps to pin it."},
    {L"没有找到应用", L"No apps found"},
    {L"没有应用", L"No apps"},
    {L"已隐藏", L"Hidden"},
    {L" 个应用", L" apps"},
    {L"Enter 打开", L"Enter to open"},
    {L"睡眠", L"Sleep"},
    {L"重启", L"Restart"},
    {L"关机", L"Shut down"},
    {L"锁定", L"Lock"},
    {L"新建文件夹", L"New folder"},
    {L"固定到任务栏", L"Pin to taskbar"},
    {L"固定到开始菜单", L"Pin to Start"},
    {L"移出文件夹", L"Remove from folder"},
    {L"从开始菜单移除", L"Remove from Start"},
    {L"取消隐藏", L"Unhide"},
    {L"隐藏", L"Hide"},
    {L"重命名", L"Rename"},
    {L"解散文件夹", L"Ungroup folder"},
    {L"小", L"Small"},
    {L"中", L"Medium"},
    {L"大", L"Large"},
    {L"显示隐藏的应用", L"Show hidden apps"},
    {L"恢复为 Windows 开始菜单的固定项", L"Restore Windows Start pins"},
    {L"刚刚", L"Just now"},
    {L" 分钟前", L" min ago"},
    {L" 小时前", L" h ago"},
    {L" 条", L" messages"},
    {L"内容已隐藏", L"Content hidden"},
    {L"通知  ·  ", L"Notifications  ·  "},
    {L"没有新通知", L"No new notifications"},
    {L"全部清除", L"Clear All"},
    {L"收起", L"Collapse"},
    {L"日期和时间设置", L"Date & time settings"},
    {L"今天", L"Today"},
    {L"没有托盘图标", L"No tray icons"},
    {L"读取不到系统托盘", L"The system tray is unavailable"},
    {L"托盘图标", L"Tray icon"},
    {L"托盘", L"Tray"},
    {L" 个图标", L" icons"},
    {L"系统托盘", L"System tray"},
    {L"窗口", L"Window"},
    {L"选择照片文件夹", L"Choose a photo folder"},
    {L"选择照片文件夹…", L"Choose a photo folder…"},
    {L"换一张", L"Next photo"},
    {L"清除已完成", L"Clear completed"},
    {L"移除小组件", L"Remove widget"},
    {L"本地", L"Local"},
    {L"明天", L"Tomorrow"},
    {L"昨天", L"Yesterday"},
    {L"小时", L" h"},
    // widget_faces.cpp: the world clock's cities (stored by their Chinese names).
    {L"北京", L"Beijing"},
    {L"上海", L"Shanghai"},
    {L"香港", L"Hong Kong"},
    {L"台北", L"Taipei"},
    {L"东京", L"Tokyo"},
    {L"首尔", L"Seoul"},
    {L"新加坡", L"Singapore"},
    {L"曼谷", L"Bangkok"},
    {L"新德里", L"New Delhi"},
    {L"迪拜", L"Dubai"},
    {L"莫斯科", L"Moscow"},
    {L"伊斯坦布尔", L"Istanbul"},
    {L"开罗", L"Cairo"},
    {L"约翰内斯堡", L"Johannesburg"},
    {L"柏林", L"Berlin"},
    {L"巴黎", L"Paris"},
    {L"伦敦", L"London"},
    {L"圣保罗", L"São Paulo"},
    {L"纽约", L"New York"},
    {L"多伦多", L"Toronto"},
    {L"芝加哥", L"Chicago"},
    {L"丹佛", L"Denver"},
    {L"洛杉矶", L"Los Angeles"},
    {L"温哥华", L"Vancouver"},
    {L"安克雷奇", L"Anchorage"},
    {L"檀香山", L"Honolulu"},
    {L"悉尼", L"Sydney"},
    {L"墨尔本", L"Melbourne"},
    {L"奥克兰", L"Auckland"},
    // desktop_widgets.cpp: the world clock's city menus.
    {L"更改哪个城市", L"Change which city"},
    {L"更改城市…", L"Choose city…"},
    {L"把「%s」换成", L"Change “%s” to"},
    // main.cpp: the app menu's forced close.
    {L"强制关闭“%s”？未保存的内容会丢失", L"Force close “%s”? Unsaved content will be lost"},
    {L"强制关闭", L"Force close"},
    {L"取消", L"Cancel"},
    {L"%.0f GB 可用", L"%.0f GB free"},
    {L"处理器", L"CPU"},
    {L"内存", L"Memory"},
    {L"磁盘", L"Disk"},
    {L"系统", L"System"},
    {L"没有已连接的设备", L"No connected devices"},
    {L"备忘录", L"Notes"},
    {L"点按以记录…", L"Tap to write…"},
    {L"提醒事项", L"Reminders"},
    {L"新提醒", L"New Reminder"},
    {L"点按以添加提醒", L"Tap to add a reminder"},
    {L"照片", L"Photos"},
    {L"右键选择照片文件夹", L"Right-click to choose a photo folder"},
    {L"天气", L"Weather"},
    {L"正在获取天气…", L"Fetching weather…"},
    {L"最高 ", L"H "},
    {L"  最低 ", L"  L "},
    {L"日出", L"Sunrise"},
    {L"日落", L"Sunset"},
    {L"晴", L"Clear"},
    {L"大部晴朗", L"Mostly clear"},
    {L"局部多云", L"Partly cloudy"},
    {L"阴", L"Overcast"},
    {L"雾", L"Fog"},
    {L"毛毛雨", L"Drizzle"},
    {L"冻雨", L"Freezing rain"},
    {L"小雨", L"Light rain"},
    {L"中雨", L"Moderate rain"},
    {L"大雨", L"Heavy rain"},
    {L"小雪", L"Light snow"},
    {L"中雪", L"Moderate snow"},
    {L"大雪", L"Heavy snow"},
    {L"雪粒", L"Snow grains"},
    {L"阵雨", L"Rain showers"},
    {L"强阵雨", L"Violent rain showers"},
    {L"阵雪", L"Snow showers"},
    {L"雷阵雨", L"Thunderstorm"},
    {L"雷阵雨伴冰雹", L"Thunderstorm with hail"},
    {L"多云", L"Cloudy"},
    {L"无法确定位置", L"Location unavailable"},
    {L"无法获取天气", L"Weather unavailable"},
    {L"现在", L"Now"},
    {L"元旦", L"New Year's Day"},
    {L"劳动节", L"Labour Day"},
    {L"国庆节", L"National Day"},
    {L"春节", L"Spring Festival"},
    {L"元宵节", L"Lantern Festival"},
    {L"端午节", L"Dragon Boat Festival"},
    {L"七夕", L"Qixi Festival"},
    {L"中秋节", L"Mid-Autumn Festival"},
    {L"重阳节", L"Double Ninth Festival"},
    {L"腊八节", L"Laba Festival"},
    {L"除夕", L"New Year's Eve"},
    {L"系统声音", L"System Sounds"},
    {L"应用", L"Apps"},
    {L"更新已准备好", L"Update ready"},
    {L" 已下载完成，可在关于中选择重启更新。", L" has been downloaded. Restart from About to install it."},
    {L"微信", L"WeChat"},
    {L"飞书", L"Feishu"},
    {L"开始", L"Start"},
    {L" 条未读通知", L" unread notifications"},
    {L"通知", L"Notifications"},
    {L"未连接网络", L"Not connected"},
    {L" · 信号 ", L" · signal "},
    {L"\n电池 ", L"\nBattery "},
    {L" · 已接通电源", L" · plugged in"},
    {L"\n音量 ", L"\nVolume "},
    {L" 个窗口", L" windows"},
};

const std::unordered_map<std::wstring_view, const wchar_t*>& English()
{
    static const auto map = [] {
        std::unordered_map<std::wstring_view, const wchar_t*> m;
        m.reserve(std::size(kEnglish));
        for (const Entry& e : kEnglish)
            m.emplace(std::wstring_view(e.zh), e.en);
        return m;
    }();
    return map;
}

std::atomic<bool> g_english = false;

bool SystemPrefersEnglish()
{
    const LANGID id = GetUserDefaultUILanguage();
    return PRIMARYLANGID(id) != LANG_CHINESE;
}

} // namespace

bool SetLanguage(const std::wstring& setting)
{
    const bool before = g_english.load(std::memory_order_relaxed);
    if (setting == L"en")
        g_english.store(true, std::memory_order_relaxed);
    else if (setting == L"zh")
        g_english.store(false, std::memory_order_relaxed);
    else
        g_english.store(SystemPrefersEnglish(), std::memory_order_relaxed);
    return before != g_english.load(std::memory_order_relaxed);
}

bool LanguageIsEnglish() { return g_english.load(std::memory_order_relaxed); }

const wchar_t* Tr(const wchar_t* chinese)
{
    if (!chinese || !g_english.load(std::memory_order_relaxed))
        return chinese;
    const auto& map = English();
    auto it = map.find(std::wstring_view(chinese));
    return it == map.end() ? chinese : it->second;
}

std::wstring TrFormat(const wchar_t* chinese, ...)
{
    const wchar_t* format = Tr(chinese);
    va_list args;
    va_start(args, chinese);
    const int length = _vscwprintf(format, args);
    va_end(args);
    if (length <= 0)
        return std::wstring(format);
    std::wstring text(size_t(length), L'\0');
    va_start(args, chinese);
    vswprintf_s(text.data(), text.size() + 1, format, args);
    va_end(args);
    return text;
}

} // namespace atrium
