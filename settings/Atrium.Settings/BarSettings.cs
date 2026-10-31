using System.Globalization;
using System.Text.Json.Nodes;

namespace Atrium.Settings;

// One pinned app of the bar. Either Path (.lnk / .exe) or Aumid identifies it.
internal sealed record PinnedApp(string Name, string Path, string Args, string Aumid);

// Mirrors native/core/settings.h (same keys, ranges and presets). Lengths in DIP.
internal sealed class BarSettings
{
    public string Preset { get; set; } = "liquid";
    public double Transparency { get; set; } = 0.95;
    public double CornerRadius { get; set; } = 30;
    public double Blur { get; set; } = 0.50;
    public double Refraction { get; set; } = 30;
    public double Edge { get; set; } = 24;
    public double Dispersion { get; set; } = 0.30;
    public double Highlight { get; set; } = 0.80;
    public double Shadow { get; set; } = 0.30;
    public uint Tint { get; set; } = 0xFFFFFF;
    public double Width { get; set; } = 1200;
    public double Height { get; set; } = 60;
    public double Margin { get; set; } = 10;
    public double IconSize { get; set; } = 30;
    public bool AutoWidth { get; set; } = true;
    public bool ShowClock { get; set; } = true;
    public bool ShowRunning { get; set; } = true;
    public string TextColor { get; set; } = "auto"; // auto | light | dark
    public double NotifyTimeout { get; set; } = 6;    // seconds a popup stays
    public bool NotifyHideBody { get; set; }           // privacy: no message text, excluded from capture
    public bool NotifyListener { get; set; }           // experimental: mirror Windows notifications
    // Null = the key was absent (never saved); the core then imports the Windows pins.
    public List<PinnedApp>? Pins { get; set; }

    public static BarSettings Liquid() => new();

    public static BarSettings Frosted() => new()
    {
        Preset = "frosted", Transparency = 0.80, Blur = 1.0, Refraction = 6, Edge = 10, Dispersion = 0.04, Highlight = 0.45, Shadow = 0.30, Tint = 0xF5F5FA,
    };

    // Copies the material of `preset`, keeping layout and content options.
    public void ApplyMaterial(BarSettings preset)
    {
        Preset = preset.Preset;
        Transparency = preset.Transparency;
        Blur = preset.Blur;
        Refraction = preset.Refraction;
        Edge = preset.Edge;
        Dispersion = preset.Dispersion;
        Highlight = preset.Highlight;
        Shadow = preset.Shadow;
        Tint = preset.Tint;
    }

    public BarSettings Clone() => FromJson(ToJson(), new BarSettings());

    public JsonObject ToJson()
    {
        var o = new JsonObject
        {
            ["version"] = 1,
            ["preset"] = Preset,
            ["transparency"] = Transparency,
            ["cornerRadius"] = CornerRadius,
            ["blur"] = Blur,
            ["refraction"] = Refraction,
            ["edge"] = Edge,
            ["dispersion"] = Dispersion,
            ["highlight"] = Highlight,
            ["shadow"] = Shadow,
            ["tint"] = "#" + Tint.ToString("X6", CultureInfo.InvariantCulture),
            ["width"] = Width,
            ["height"] = Height,
            ["margin"] = Margin,
            ["iconSize"] = IconSize,
            ["showClock"] = ShowClock,
            ["autoWidth"] = AutoWidth,
            ["showRunning"] = ShowRunning,
            ["textColor"] = TextColor,
            ["notifyTimeout"] = NotifyTimeout,
            ["notifyHideBody"] = NotifyHideBody,
            ["notifyListener"] = NotifyListener,
        };
        if (Pins != null)
        {
            var a = new JsonArray();
            foreach (var p in Pins)
                a.Add(new JsonObject { ["name"] = p.Name, ["path"] = p.Path, ["args"] = p.Args, ["aumid"] = p.Aumid });
            o["pins"] = a;
        }
        return o;
    }

    // Missing or malformed keys keep the values of `fallback`.
    public static BarSettings FromJson(JsonObject? o, BarSettings fallback)
    {
        var s = new BarSettings();
        var f = fallback;
        double Num(string k, double d) => o?[k] is JsonValue v && v.TryGetValue(out double x) && double.IsFinite(x) ? x : d;
        s.Preset = o?["preset"] is JsonValue p && p.TryGetValue(out string? ps) && ps != null ? ps : f.Preset;
        s.Transparency = Math.Clamp(Num("transparency", f.Transparency), 0, 1);
        s.CornerRadius = Math.Clamp(Num("cornerRadius", f.CornerRadius), 0, 200);
        s.Blur = Math.Clamp(Num("blur", f.Blur), 0, 1);
        s.Refraction = Math.Clamp(Num("refraction", f.Refraction), 0, 60);
        s.Edge = Math.Clamp(Num("edge", f.Edge), 1, 80);
        s.Dispersion = Math.Clamp(Num("dispersion", f.Dispersion), 0, 0.5);
        s.Highlight = Math.Clamp(Num("highlight", f.Highlight), 0, 1);
        s.Shadow = Math.Clamp(Num("shadow", f.Shadow), 0, 1);
        s.Width = Math.Clamp(Num("width", f.Width), 200, 4000);
        s.Height = Math.Clamp(Num("height", f.Height), 32, 160);
        s.Margin = Math.Clamp(Num("margin", f.Margin), 0, 400);
        s.IconSize = Math.Clamp(Num("iconSize", f.IconSize), 16, 64);
        s.Tint = f.Tint;
        if (o?["tint"] is JsonValue t && t.TryGetValue(out string? ts) && ts is { Length: 7 } && ts[0] == '#' &&
            uint.TryParse(ts.AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var tint))
            s.Tint = tint;
        bool Flag(string k, bool d) => o?[k] is JsonValue v && v.TryGetValue(out bool b) ? b : d;
        s.ShowClock = Flag("showClock", f.ShowClock);
        s.AutoWidth = Flag("autoWidth", f.AutoWidth);
        s.ShowRunning = Flag("showRunning", f.ShowRunning);
        s.NotifyTimeout = Math.Clamp(Num("notifyTimeout", f.NotifyTimeout), 3, 30);
        s.NotifyHideBody = Flag("notifyHideBody", f.NotifyHideBody);
        s.NotifyListener = Flag("notifyListener", f.NotifyListener);
        s.TextColor = o?["textColor"] is JsonValue tc && tc.TryGetValue(out string? tcs) && tcs != null
            ? (tcs is "light" or "dark" ? tcs : "auto")
            : f.TextColor;
        s.Pins = f.Pins == null ? null : new List<PinnedApp>(f.Pins);
        if (o?["pins"] is JsonArray arr)
        {
            var pins = new List<PinnedApp>();
            foreach (var node in arr)
            {
                if (node is not JsonObject e) continue;
                string Str(string k) => e[k] is JsonValue v && v.TryGetValue(out string? x) && x != null ? x : "";
                var pin = new PinnedApp(Str("name"), Str("path"), Str("args"), Str("aumid"));
                if (pin.Path.Length > 0 || pin.Aumid.Length > 0) pins.Add(pin);
            }
            if (pins.Count > 64) pins.RemoveRange(64, pins.Count - 64);
            s.Pins = pins;
        }
        return s;
    }

    public bool SameAs(BarSettings other) => ToJson().ToJsonString() == other.ToJson().ToJsonString();
}
