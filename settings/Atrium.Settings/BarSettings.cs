using System.Globalization;
using System.Text.Json.Nodes;

namespace Atrium.Settings;

// Mirrors native/core/settings.h (same keys, ranges and presets). Lengths in DIP.
internal sealed class BarSettings
{
    public string Preset { get; set; } = "liquid";
    public double Transparency { get; set; } = 0.90;
    public double CornerRadius { get; set; } = 28;
    public double Blur { get; set; } = 0.35;
    public double Refraction { get; set; } = 13;
    public double Edge { get; set; } = 17;
    public double Dispersion { get; set; } = 0.08;
    public double Highlight { get; set; } = 0.55;
    public double Shadow { get; set; } = 0.28;
    public uint Tint { get; set; } = 0xFFFFFF;
    public double Width { get; set; } = 720;
    public double Height { get; set; } = 56;
    public double Margin { get; set; } = 10;
    public bool ShowClock { get; set; } = true;

    public static BarSettings Liquid() => new();

    public static BarSettings Frosted() => new()
    {
        Preset = "frosted", Transparency = 0.78, Blur = 1.0, Refraction = 0, Edge = 1, Dispersion = 0, Highlight = 0.30, Tint = 0xF5F5FA,
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

    public JsonObject ToJson() => new()
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
        ["showClock"] = ShowClock,
    };

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
        s.Tint = f.Tint;
        if (o?["tint"] is JsonValue t && t.TryGetValue(out string? ts) && ts is { Length: 7 } && ts[0] == '#' &&
            uint.TryParse(ts.AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var tint))
            s.Tint = tint;
        s.ShowClock = o?["showClock"] is JsonValue c && c.TryGetValue(out bool b) ? b : f.ShowClock;
        return s;
    }

    public bool SameAs(BarSettings other) => ToJson().ToJsonString() == other.ToJson().ToJsonString();
}
