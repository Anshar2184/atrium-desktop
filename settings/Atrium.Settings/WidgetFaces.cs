using System.Globalization;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace Atrium.Settings;

// A widget's face as the core paints it (native/core/ui/widget_faces.cpp): in the glass's ink, with
// macOS's few accents (orange second hand and notes, green / red battery, blue calendar), in widget
// DIP (small 164 x 164, medium 344 x 164, large 344 x 344). The glass itself is rendered under it.
internal static class WidgetFaces
{
    private const double Pad = 14; // kPad
    public static readonly Color Red = Color.FromArgb(255, 0xFF, 0x45, 0x3A), Orange = Color.FromArgb(255, 0xFF, 0x9F, 0x0A),
                                 Green = Color.FromArgb(255, 0x30, 0xD1, 0x58), Blue = Color.FromArgb(255, 0x0A, 0x84, 0xFF);
    private static readonly string[] WeekLong = { "星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日" };
    private static readonly string[] WeekShort = { "一", "二", "三", "四", "五", "六", "日" };
    private static readonly string[] Months = { "一月", "二月", "三月", "四月", "五月", "六月", "七月", "八月", "九月", "十月", "十一月", "十二月" };

    // GlassPal: the glass's inks (Theme::For) and the accents.
    private sealed record Pal(Color Text, Color Sub, Color Faint, Color Face, Color Track, Color Halo, bool Dark)
    {
        public static readonly Color Red = WidgetFaces.Red, Orange = WidgetFaces.Orange, Green = WidgetFaces.Green, Blue = WidgetFaces.Blue;

        public static Pal For(bool dark)
        {
            Color ink = dark ? Color.FromArgb(255, 0x11, 0x13, 0x17) : Color.FromArgb(255, 255, 255, 255);
            Color A(double a) => Color.FromArgb((byte)Math.Round(255 * a), ink.R, ink.G, ink.B);
            double text = dark ? 0.92 : 0.96;
            return new Pal(A(text), A(dark ? 0.66 : 0.76), A(dark ? 0.46 : 0.56), A(text * 0.10), A(text * 0.18),
                           dark ? Color.FromArgb(89, 255, 255, 255) : Color.FromArgb(82, 0, 0, 0), dark);
        }
    }

    public static (double W, double H) Size(string size) => size switch
    {
        "small" => (164, 164),
        "large" => (344, 344),
        _ => (344, 164),
    };

    public static FrameworkElement Face(string kind, string size, bool darkInk, Brush? photo = null)
    {
        var (w, h) = Size(size);
        var c = new Canvas { Width = w, Height = h, IsHitTestVisible = false };
        var p = Pal.For(darkInk);
        var now = DateTime.Now;
        switch (kind)
        {
            case "clock" when size == "small":
                ClockFace(c, w / 2, h / 2, h / 2 - 9, now, p.Face, p.Text, true, "北京", p.Sub, p);
                break;
            case "clock":
            {
                (string Name, int Offset)[] cities = { ("北京", 0), ("东京", 1), ("伦敦", -8), ("纽约", -13) };
                double colW = (w - 2 * Pad) / 4, R = 31;
                for (int i = 0; i < 4; i++)
                {
                    var t = now.AddHours(cities[i].Offset);
                    bool night = t.Hour < 6 || t.Hour >= 18;
                    double cx = Pad + colW * (i + 0.5), cy = (h - 2 * R - 42) / 2 + R - 2;
                    Color face = night ? Color.FromArgb(107, 0, 0, 0) : p.Face, ink = night ? Color.FromArgb(245, 255, 255, 255) : p.Text;
                    ClockFace(c, cx, cy, R, t, face, ink, false, null, ink, p);
                    Text(c, cities[i].Name, cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26, 12.5, p.Text, 2, 1, p);
                    int dd = t.Date.CompareTo(now.Date);
                    string when = (dd > 0 ? "明天" : dd < 0 ? "昨天" : "今天") + (cities[i].Offset >= 0 ? " +" : " ") + cities[i].Offset + "小时";
                    Text(c, when, cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42, 10.5, p.Sub, 0, 1, p);
                }
                break;
            }
            case "digital":
            {
                double leftW = size == "small" ? w - 2 * Pad : 150;
                Text(c, "本地", Pad + 2, Pad, Pad + leftW, Pad + 18, 12.5, p.Sub, 2, 0, p);
                // Match the core's fixed daily size, with one DIP of glyph-rounding headroom.
                double fit = TextWidth("00:00", 50, "TitleFont");
                double timeW = leftW - 1;
                Text(c, now.ToString("H:mm"), Pad, Pad + 22, Pad + leftW, Pad + 84, fit > timeW ? 50 * timeW / fit : 50, p.Text, 2, 1, p);
                Text(c, WeekLong[Weekday(now)], Pad + 2, h - Pad - 40, Pad + leftW, h - Pad - 20, 13, Pal.Blue, 2, 0, p);
                Text(c, $"{now.Month}月{now.Day}日", Pad + 2, h - Pad - 20, Pad + leftW, h - Pad, 13, p.Text, 0, 0, p);
                if (size == "small") break;
                double x0 = Pad + leftW + 8, rowH = (h - 2 * Pad) / 3;
                (string Name, int Offset)[] more = { ("东京", 1), ("伦敦", -8), ("纽约", -13) };
                for (int i = 0; i < 3; i++)
                {
                    double y = Pad + rowH * i;
                    var t = now.AddHours(more[i].Offset);
                    if (i > 0) Line(c, x0, y, w - Pad, y, A(p.Text, 0.12), 1);
                    int dd = t.Date.CompareTo(now.Date);
                    Text(c, more[i].Name, x0, y + 4, w - Pad - 70, y + rowH / 2 + 2, 13, p.Text, 2, 0, p);
                    Text(c, (dd > 0 ? "明天" : dd < 0 ? "昨天" : "今天") + (more[i].Offset >= 0 ? " +" : " ") + more[i].Offset + "小时",
                         x0, y + rowH / 2, w - Pad - 70, y + rowH - 4, 10.5, p.Sub, 0, 0, p);
                    Text(c, t.ToString("H:mm"), w - Pad - 80, y, w - Pad, y + rowH, 22, p.Text, 2, 2, p);
                }
                break;
            }
            case "calendar":
                Calendar(c, size, w, h, now, p);
                break;
            case "battery":
            {
                double cx = w / 2, cy = h / 2 - 12, R = 44;
                var (fill, charging, low) = BarFace.Battery();
                Arc(c, cx, cy, R, -90, 360, p.Track, 9, false);
                Color level = low ? Pal.Red : Pal.Green;
                Arc(c, cx, cy, R, -90, 360 * fill, level, 9, true);
                Glyph(c, "", cx - 22, cy - 22, cx + 22, cy + 22, 24, p.Text);
                string n = $"{Math.Round(fill * 100)}%";
                double tw = TextWidth(n, 19, "TitleFont"), bolt = charging ? 16 : 0, x0 = (w - tw - bolt) / 2;
                if (charging) Glyph(c, "", x0 - 2, cy + R + 10, x0 + bolt, cy + R + 36, 13, level);
                Text(c, n, x0 + bolt, cy + R + 10, x0 + bolt + tw + 4, cy + R + 36, 19, p.Text, 2, 0, p);
                break;
            }
            case "system":
            {
                (string Name, double V, string Detail)[] g = { ("CPU", 0.23, "处理器"), ("内存", 0.58, "9.2 / 16 GB"), ("磁盘", 0.74, "214 GB 可用") };
                Color Col(double v) => v >= 0.9 ? Pal.Red : v >= 0.7 ? Pal.Orange : Pal.Green;
                if (size == "small")
                {
                    Text(c, "系统", Pad + 2, Pad - 2, w - Pad, Pad + 16, 12.5, p.Sub, 2, 0, p);
                    double rowH = (h - 2 * Pad - 18) / 3;
                    for (int i = 0; i < 3; i++)
                    {
                        double y = Pad + 18 + rowH * i;
                        Text(c, g[i].Name, Pad + 2, y + 2, w / 2, y + 22, 13, p.Text, 2, 0, p);
                        Text(c, $"{Math.Round(g[i].V * 100)}%", w / 2, y + 2, w - Pad - 2, y + 22, 13, p.Text, 2, 2, p);
                        Fill(c, Pad + 2, y + 26, w - Pad - 2, y + 32, 3, p.Track);
                        Fill(c, Pad + 2, y + 26, Pad + 2 + Math.Max(6, (w - 2 * Pad - 4) * g[i].V), y + 32, 3, Col(g[i].V));
                    }
                    break;
                }
                double colW = (w - 2 * Pad) / 3, R = 34, cy = Pad + 6 + R;
                for (int i = 0; i < 3; i++)
                {
                    double cx = Pad + colW * (i + 0.5);
                    Arc(c, cx, cy, R, -90, 360, p.Track, 8, false);
                    Arc(c, cx, cy, R, -90, 360 * g[i].V, Col(g[i].V), 8, true);
                    Text(c, $"{Math.Round(g[i].V * 100)}%", cx - R, cy - 12, cx + R, cy + 12, 17, p.Text, 2, 1, p);
                    Text(c, g[i].Name, cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26, 13, p.Text, 2, 1, p);
                    Text(c, g[i].Detail, cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42, 10.5, p.Sub, 0, 1, p);
                }
                break;
            }
            case "notes":
                Text(c, "备忘录", Pad + 2, Pad - 2, w - Pad, Pad + 18, 13.5, Pal.Orange, 2, 0, p);
                Paragraph(c, size == "small" ? "周五前交设计评审\n换上新的壁纸" : "周五前交设计评审\n换上新的壁纸\n试试磨砂预设",
                          Pad + 2, Pad + 24, w - Pad - 2, h - Pad, 13.5, p.Text, p);
                break;
            case "reminders":
            {
                (string Text, bool Done)[] items = { ("给桌面拍一组截图", false), ("整理本周的会议记录", false), ("订周末的电影票", true) };
                Text(c, "提醒事项", Pad + 2, Pad - 2, w - Pad - 40, Pad + 18, 13.5, Pal.Orange, 2, 0, p);
                Text(c, "2", w - Pad - 48, Pad - 6, w - Pad - 2, Pad + 22, 22, Pal.Orange, 2, 2, p);
                const double RowH = 27, Top = Pad + 28;
                int slots = Math.Max(1, (int)((h - Pad - Top) / RowH));
                int shown = 0;
                foreach (var it in items)
                {
                    if (shown >= slots - 1) break;
                    double y = Top + RowH * shown, cx = Pad + 9, cy = y + RowH / 2;
                    if (it.Done)
                    {
                        Circle(c, cx, cy, 8, Pal.Orange);
                        Circle(c, cx, cy, 3.2, Color.FromArgb(250, 255, 255, 255));
                    }
                    else Arc(c, cx, cy, 7.4, -90, 360, p.Sub, 1.5, false);
                    Text(c, it.Text, Pad + 26, y, w - Pad, y + RowH, 13.5, it.Done ? p.Sub : p.Text, 0, 0, p);
                    if (it.Done) Line(c, Pad + 26, cy, Math.Min(w - Pad, Pad + 26 + TextWidth(it.Text, 13.5, "TextFont")), cy, p.Sub, 1);
                    shown++;
                }
                if (shown < slots)
                {
                    double y = Top + RowH * shown;
                    Text(c, "+", Pad - 1, y, Pad + 19, y + RowH, 17, p.Faint, 0, 1, p);
                    Text(c, "新提醒", Pad + 26, y, w - Pad, y + RowH, 13.5, p.Faint, 0, 0, p);
                }
                break;
            }
            case "photos":
                c.Children.Add(new Rectangle { Width = w, Height = h, RadiusX = 22, RadiusY = 22, Fill = photo });
                break;
        }
        return c;
    }

    // Keep a preview face while its displayed data is unchanged. This only invalidates an
    // existing render request; it does not add a clock or a polling timer to the gallery.
    public static long Stamp(string kind) => kind switch
    {
        "clock" => DateTime.Now.Ticks / TimeSpan.TicksPerSecond,
        "digital" => DateTime.Now.Ticks / TimeSpan.TicksPerMinute,
        "calendar" => DateTime.Today.Ticks,
        "battery" => BatteryStamp(),
        _ => 0,
    };

    private static long BatteryStamp()
    {
        var (fill, charging, low) = BarFace.Battery();
        return (long)Math.Round(fill * 100) | (charging ? 128L : 0) | (low ? 256L : 0);
    }

    // ---- The calendar: small, medium, and the large month with a lunar day under every date ----

    private static readonly ChineseLunisolarCalendar Lunar = new();
    private static readonly string[] LunarMonths = { "正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊" };

    private static string LunarDay(int d)
    {
        string[] digits = { "一", "二", "三", "四", "五", "六", "七", "八", "九", "十" };
        if (d == 10) return "初十";
        if (d == 20) return "二十";
        if (d == 30) return "三十";
        return (d < 10 ? "初" : d < 20 ? "十" : d < 30 ? "廿" : "三") + digits[(d - 1) % 10];
    }

    // (month, day, leap) of a date in the Chinese calendar.
    private static (int M, int D, bool Leap) ToLunar(DateTime t)
    {
        try
        {
            int y = Lunar.GetYear(t), m = Lunar.GetMonth(t), d = Lunar.GetDayOfMonth(t), leap = Lunar.GetLeapMonth(y);
            bool isLeap = leap > 0 && m == leap;
            if (leap > 0 && m >= leap) m--;
            return (m, d, isLeap);
        }
        catch (ArgumentOutOfRangeException) { return (1, 1, false); }
    }

    private static string LunarMonthDay(DateTime t)
    {
        var (m, d, leap) = ToLunar(t);
        return (leap ? "闰" : "") + LunarMonths[Math.Clamp(m, 1, 12) - 1] + "月" + LunarDay(d);
    }

    // ChineseCalendar::DayLabel: the festivals, else the day (the month's name on its first day).
    private static string DayLabel(DateTime t, out bool festival)
    {
        festival = true;
        if (t.Month == 1 && t.Day == 1) return "元旦";
        if (t.Month == 5 && t.Day == 1) return "劳动节";
        if (t.Month == 10 && t.Day == 1) return "国庆节";
        var (m, d, leap) = ToLunar(t);
        if (!leap)
        {
            (int M, int D, string Name)[] fests = { (1, 1, "春节"), (1, 15, "元宵节"), (5, 5, "端午节"), (7, 7, "七夕"), (8, 15, "中秋节"), (9, 9, "重阳节"), (12, 8, "腊八节") };
            foreach (var f in fests) if (f.M == m && f.D == d) return f.Name;
            if (m == 12 && ToLunar(t.AddDays(1)) is (1, 1, false)) return "除夕";
        }
        festival = false;
        return d == 1 ? (leap ? "闰" : "") + LunarMonths[Math.Clamp(m, 1, 12) - 1] + "月" : LunarDay(d);
    }

    private static int Weekday(DateTime t) => ((int)t.DayOfWeek + 6) % 7; // 0 = Monday

    private static void Calendar(Canvas c, string size, double w, double h, DateTime now, Pal p)
    {
        int first = ((int)CultureInfo.CurrentCulture.DateTimeFormat.FirstDayOfWeek + 6) % 7; // 0 = Monday
        var firstDay = new DateTime(now.Year, now.Month, 1);
        int lead = (Weekday(firstDay) - first + 7) % 7, days = DateTime.DaysInMonth(now.Year, now.Month);
        if (size == "large")
        {
            Text(c, Months[now.Month - 1], Pad + 4, Pad, w / 2, Pad + 22, 16, Pal.Blue, 2, 0, p);
            Text(c, $"{now.Year}年 · 农历{LunarMonthDay(now)}", w / 2 - 40, Pad, w - Pad - 4, Pad + 22, 11.5, p.Sub, 0, 2, p);
            double gx = Pad, cw = (w - 2 * Pad) / 7, y = Pad + 28;
            for (int i = 0; i < 7; i++) Text(c, WeekShort[(first + i) % 7], gx + i * cw, y, gx + (i + 1) * cw, y + 16, 10.5, p.Sub, 2, 1, p);
            y += 20;
            int rows = (lead + days + 6) / 7;
            double rh = (h - Pad - y) / Math.Max(rows, 5);
            for (int d = 1; d <= days; d++)
            {
                int cell = lead + d - 1;
                double x0 = gx + (cell % 7) * cw, y0 = y + (cell / 7) * rh, cx = x0 + cw / 2;
                bool today = d == now.Day, weekend = (first + cell % 7) % 7 >= 5;
                if (today) Fill(c, cx - 18, y0 + 1, cx + 18, y0 + rh - 1, 10, Pal.Blue);
                Color ink = today ? Color.FromArgb(250, 255, 255, 255) : weekend ? p.Sub : p.Text;
                Text(c, d.ToString(), x0, y0, x0 + cw, y0 + rh * 0.56, 14, ink, 2, 1, p, halo: !today);
                string label = DayLabel(new DateTime(now.Year, now.Month, d), out bool festival);
                Text(c, label, x0, y0 + rh * 0.48, x0 + cw, y0 + rh - 5, 9, today ? Color.FromArgb(212, 255, 255, 255) : festival ? Pal.Blue : p.Sub, 0, 1, p, halo: !today);
            }
            return;
        }
        double leftW = size == "small" ? w - 2 * Pad : 126;
        Text(c, WeekLong[Weekday(now)], Pad + 2, Pad, Pad + leftW, Pad + 20, 14, Pal.Blue, 2, 0, p);
        Text(c, now.Day.ToString(), Pad, Pad + 16, Pad + leftW, Pad + 80, 54, p.Text, 0, 0, p);
        string todayLabel = DayLabel(now, out bool fest);
        Text(c, "农历" + LunarMonthDay(now), Pad + 2, h - Pad - 40, Pad + leftW, h - Pad - 20, 12.5, p.Text, 2, 0, p);
        Text(c, fest ? todayLabel : $"{now.Year}年{Months[now.Month - 1]}", Pad + 2, h - Pad - 20, Pad + leftW, h - Pad, 12, fest ? Pal.Blue : p.Sub, 0, 0, p);
        if (size == "small") return;
        double gx2 = Pad + leftW + 6, gw = w - Pad - gx2, cw2 = gw / 7;
        Text(c, Months[now.Month - 1], gx2 + 4, Pad - 1, gx2 + gw, Pad + 17, 12.5, Pal.Blue, 2, 0, p);
        double yy = Pad + 20;
        for (int i = 0; i < 7; i++) Text(c, WeekShort[(first + i) % 7], gx2 + i * cw2, yy, gx2 + (i + 1) * cw2, yy + 14, 9.5, p.Sub, 2, 1, p);
        yy += 16;
        int rows2 = (lead + days + 6) / 7;
        double rh2 = Math.Min(17, (h - Pad - yy) / rows2);
        for (int d = 1; d <= days; d++)
        {
            int cell = lead + d - 1;
            double x0 = gx2 + (cell % 7) * cw2, y0 = yy + (cell / 7) * rh2, cx = x0 + cw2 / 2, cy = y0 + rh2 / 2;
            bool today = d == now.Day, weekend = (first + cell % 7) % 7 >= 5;
            if (today) Circle(c, cx, cy, rh2 / 2 + 0.5, Pal.Blue);
            Text(c, d.ToString(), x0, cy - rh2 / 2, x0 + cw2, cy + rh2 / 2, 10.5,
                 today ? Color.FromArgb(250, 255, 255, 255) : weekend ? p.Sub : p.Text, 2, 1, p, halo: !today);
        }
    }

    // ---- The analogue face: numbers or ticks, hands with a thin neck, the orange second hand ----

    private static void ClockFace(Canvas c, double cx, double cy, double R, DateTime t, Color face, Color ink, bool numbers, string? label, Color labelInk, Pal p)
    {
        Circle(c, cx, cy, R, face);
        for (int hr = 1; hr <= 12; hr++)
        {
            double a = (hr * 30 - 90) * Math.PI / 180;
            if (numbers)
            {
                double r = R * 0.78, fs = R * 0.21, x = cx + r * Math.Cos(a), y = cy + r * Math.Sin(a);
                Text(c, hr.ToString(), x - fs, y - fs * 0.72, x + fs, y + fs * 0.72, fs, ink, 2, 1, p);
            }
            else
            {
                double r0 = R * 0.80, r1 = R * 0.92;
                Line(c, cx + r0 * Math.Cos(a), cy + r0 * Math.Sin(a), cx + r1 * Math.Cos(a), cy + r1 * Math.Sin(a), A(ink, hr % 3 != 0 ? 0.45 : 0.9), Math.Max(1, R * 0.045));
            }
        }
        if (label != null) Text(c, label, cx - R, cy + R * 0.26, cx + R, cy + R * 0.26 + R * 0.2, R * 0.15, labelInk, 2, 1, p);
        double sec = t.Second, min = t.Minute + sec / 60, hour = t.Hour % 12 + min / 60;
        void Hand(double angle, double length, double width)
        {
            double a = (angle - 90) * Math.PI / 180, neck = R * 0.16, dx = Math.Cos(a), dy = Math.Sin(a);
            Line(c, cx, cy, cx + dx * neck, cy + dy * neck, ink, Math.Max(1.2, width * 0.36));
            Line(c, cx + dx * neck, cy + dy * neck, cx + dx * length, cy + dy * length, ink, width);
        }
        Hand(hour * 30, R * 0.52, Math.Max(2.5, R * 0.075));
        Hand(min * 6, R * 0.80, Math.Max(2.5, R * 0.075));
        Circle(c, cx, cy, Math.Max(2.2, R * 0.055), ink);
        double sa = (sec * 6 - 90) * Math.PI / 180;
        Line(c, cx - Math.Cos(sa) * R * 0.18, cy - Math.Sin(sa) * R * 0.18, cx + Math.Cos(sa) * R * 0.86, cy + Math.Sin(sa) * R * 0.86, Pal.Orange, Math.Max(1, R * 0.02));
        Circle(c, cx, cy, Math.Max(1.8, R * 0.04), Pal.Orange);
    }

    // ---- Drawing helpers (Canvas in surface.cpp) ----

    private static Color A(Color c, double a) => Color.FromArgb((byte)Math.Round(c.A * a), c.R, c.G, c.B);

    // weight: 0 regular, 1 medium, 2 semibold; align 0 left, 1 centre, 2 right; one line, centred in the rect.
    // On glass, 12.5 DIP and below a regular is set a step heavier, and every line has its halo below.
    private static void Text(Canvas c, string s, double l, double t, double r, double b, double size, Color ink, int weight, int align, Pal p, bool halo = true)
    {
        if (size <= 12.5 && weight == 0) weight = 1;
        string font = weight switch { 0 => "TextFont", 1 => "EmphasisFont", _ => "TitleFont" };
        double o = 0.6;
        for (int pass = halo ? 0 : 1; pass < 2; pass++)
        {
            var tb = new TextBlock
            {
                Text = s, FontSize = size, Foreground = new SolidColorBrush(pass == 0 ? A(p.Halo, ink.A / 255.0) : ink),
                FontFamily = (FontFamily)Application.Current.Resources[font], TextWrapping = TextWrapping.NoWrap,
                TextAlignment = align == 1 ? TextAlignment.Center : align == 2 ? TextAlignment.Right : TextAlignment.Left,
                VerticalAlignment = VerticalAlignment.Center, IsTextScaleFactorEnabled = false,
            };
            // Left-aligned lines may run past their box (DirectWrite lays them out unwrapped too); the others align in it.
            if (align != 0) tb.Width = Math.Max(1, r - l);
            Typography.SetNumeralAlignment(tb, FontNumeralAlignment.Tabular);
            // Centred in its box: MiSans' line box is 1.326 em with the figures in its middle.
            double line = size * 1.326;
            tb.Height = line;
            Canvas.SetLeft(tb, l);
            Canvas.SetTop(tb, t + (b - t - line) / 2 + (pass == 0 ? o : 0));
            c.Children.Add(tb);
        }
    }

    private static double TextWidth(string s, double size, string font)
    {
        var tb = new TextBlock { Text = s, FontSize = size, FontFamily = (FontFamily)Application.Current.Resources[font], IsTextScaleFactorEnabled = false };
        Typography.SetNumeralAlignment(tb, FontNumeralAlignment.Tabular);
        tb.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
        return tb.DesiredSize.Width;
    }

    private static void Paragraph(Canvas c, string s, double l, double t, double r, double b, double size, Color ink, Pal p)
    {
        var tb = new TextBlock
        {
            Text = s, FontSize = size, Foreground = new SolidColorBrush(ink), FontFamily = (FontFamily)Application.Current.Resources["TextFont"],
            TextWrapping = TextWrapping.Wrap, Width = r - l, MaxHeight = b - t, IsTextScaleFactorEnabled = false,
        };
        Canvas.SetLeft(tb, l);
        Canvas.SetTop(tb, t);
        c.Children.Add(tb);
    }

    private static void Glyph(Canvas c, string g, double l, double t, double r, double b, double size, Color ink)
    {
        var f = new FontIcon { Glyph = g, FontSize = size, Foreground = new SolidColorBrush(ink), Width = r - l, Height = b - t, FontFamily = new FontFamily("Segoe Fluent Icons") };
        Canvas.SetLeft(f, l);
        Canvas.SetTop(f, t);
        c.Children.Add(f);
    }

    private static void Circle(Canvas c, double cx, double cy, double r, Color fill)
    {
        var e = new Ellipse { Width = 2 * r, Height = 2 * r, Fill = new SolidColorBrush(fill) };
        Canvas.SetLeft(e, cx - r);
        Canvas.SetTop(e, cy - r);
        c.Children.Add(e);
    }

    private static void Fill(Canvas c, double l, double t, double r, double b, double radius, Color fill)
    {
        var e = new Rectangle { Width = Math.Max(0, r - l), Height = Math.Max(0, b - t), RadiusX = radius, RadiusY = radius, Fill = new SolidColorBrush(fill) };
        Canvas.SetLeft(e, l);
        Canvas.SetTop(e, t);
        c.Children.Add(e);
    }

    private static void Line(Canvas c, double x1, double y1, double x2, double y2, Color ink, double width) =>
        c.Children.Add(new Line { X1 = x1, Y1 = y1, X2 = x2, Y2 = y2, Stroke = new SolidColorBrush(ink), StrokeThickness = width,
                                  StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round });

    // An arc from `from` degrees (0 = right, -90 = top) over `sweep`, clockwise.
    private static void Arc(Canvas c, double cx, double cy, double r, double from, double sweep, Color ink, double width, bool caps)
    {
        if (sweep <= 0.01) return;
        if (sweep >= 359.99)
        {
            var ring = new Ellipse { Width = 2 * r + width, Height = 2 * r + width, Stroke = new SolidColorBrush(ink), StrokeThickness = width };
            Canvas.SetLeft(ring, cx - r - width / 2);
            Canvas.SetTop(ring, cy - r - width / 2);
            c.Children.Add(ring);
            return;
        }
        Point At(double deg) => new(cx + r * Math.Cos(deg * Math.PI / 180), cy + r * Math.Sin(deg * Math.PI / 180));
        var f = new PathFigure { StartPoint = At(from), IsClosed = false, IsFilled = false };
        f.Segments.Add(new ArcSegment { Point = At(from + sweep), Size = new Windows.Foundation.Size(r, r), SweepDirection = SweepDirection.Clockwise, IsLargeArc = sweep > 180 });
        c.Children.Add(new Path
        {
            Data = new PathGeometry { Figures = { f } }, Stroke = new SolidColorBrush(ink), StrokeThickness = width,
            StrokeStartLineCap = caps ? PenLineCap.Round : PenLineCap.Flat, StrokeEndLineCap = caps ? PenLineCap.Round : PenLineCap.Flat,
        });
    }
}
