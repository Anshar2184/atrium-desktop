using System.Text.Json;
using System.Text.Json.Nodes;

namespace Atrium.Probe;

// Collects probe findings and writes them to poc/results/<name>.json.
internal sealed class Report
{
    private readonly string _name;
    public JsonObject Root { get; } = new();
    public static string ResultsDir => Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "results"));

    public Report(string name)
    {
        _name = name;
        Root["probe"] = name;
        Root["startedAt"] = DateTimeOffset.Now.ToString("O");
        Root["os"] = Environment.OSVersion.VersionString;
    }

    public void Set(string key, JsonNode? value)
    {
        Root[key] = value;
        Console.WriteLine($"  {key} = {value?.ToJsonString()}");
    }

    public void Verdict(string key, string verdict, string note)
    {
        var v = new JsonObject { ["verdict"] = verdict, ["note"] = note };
        Root.TryAdd("verdicts", new JsonObject());
        ((JsonObject)Root["verdicts"]!)[key] = v;
        Console.WriteLine($"[{verdict}] {key}: {note}");
    }

    public string Save()
    {
        Root["finishedAt"] = DateTimeOffset.Now.ToString("O");
        Directory.CreateDirectory(ResultsDir);
        var path = Path.Combine(ResultsDir, _name + ".json");
        File.WriteAllText(path, Root.ToJsonString(new JsonSerializerOptions { TypeInfoResolver = new System.Text.Json.Serialization.Metadata.DefaultJsonTypeInfoResolver(), WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));
        Console.WriteLine("saved " + path);
        return path;
    }
}
