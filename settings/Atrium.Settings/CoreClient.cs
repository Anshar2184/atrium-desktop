using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Win32;

namespace Atrium.Settings;

// Client of the Atrium.Core settings pipe (docs/SETTINGS-PROTOCOL.md). When the
// core is not running, settings are read from and written to the file directly.
internal sealed class CoreClient : IDisposable
{
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint processId);

    private static string PipeName => "Atrium.Core." + Process.GetCurrentProcess().SessionId;
    public static string SettingsPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Atrium", "settings.json");

    private readonly SemaphoreSlim _io = new(1, 1);
    private readonly byte[] _readBuffer = new byte[4096];
    private NamedPipeClientStream? _pipe;

    public bool Connected => _pipe is { IsConnected: true };
    public int ProcessId { get; private set; }

    public static string? CoreExe()
    {
        foreach (var p in new[] { Path.Combine(AppContext.BaseDirectory, "..", "Atrium.exe"), Path.Combine(AppContext.BaseDirectory, "Atrium.exe") })
            if (File.Exists(p)) return Path.GetFullPath(p);
        return null;
    }

    public async Task<bool> ConnectAsync(int timeoutMs)
    {
        Disconnect();
        var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        try
        {
            await pipe.ConnectAsync(timeoutMs);
            pipe.ReadMode = PipeTransmissionMode.Message;
            // Only talk to the real core, not to another process holding the name.
            if (!GetNamedPipeServerProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out var pid) ||
                !string.Equals(Process.GetProcessById((int)pid).ProcessName, "Atrium", StringComparison.OrdinalIgnoreCase))
            {
                pipe.Dispose();
                return false;
            }
            _pipe = pipe;
            ProcessId = (int)pid;
            return true;
        }
        catch (Exception e) when (e is TimeoutException or IOException or UnauthorizedAccessException or ArgumentException or InvalidOperationException)
        {
            pipe.Dispose();
            return false;
        }
    }

    public void Disconnect()
    {
        _pipe?.Dispose();
        _pipe = null;
        ProcessId = 0;
    }

    private async Task<JsonObject?> RequestAsync(JsonObject request)
    {
        await _io.WaitAsync();
        try
        {
            var pipe = _pipe;
            if (pipe is not { IsConnected: true }) return null;
            var bytes = JsonSerializer.SerializeToUtf8Bytes(request);
            await pipe.WriteAsync(bytes);
            await pipe.FlushAsync();
            using var message = new MemoryStream();
            do
            {
                int n = await pipe.ReadAsync(_readBuffer);
                if (n == 0) throw new IOException("pipe closed");
                message.Write(_readBuffer, 0, n);
            } while (!pipe.IsMessageComplete);
            return JsonNode.Parse(message.GetBuffer().AsSpan(0, (int)message.Length)) as JsonObject;
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException or System.Text.Json.JsonException)
        {
            Disconnect();
            return null;
        }
        finally
        {
            _io.Release();
        }
    }

    // (saved, current) from the core, or (file, file) when it is not running.
    public async Task<(BarSettings Saved, BarSettings Current)> GetAsync()
    {
        var r = await RequestAsync(new JsonObject { ["op"] = "get" });
        if (r?["op"]?.GetValue<string>() == "state")
            return (BarSettings.FromJson(r["saved"] as JsonObject, new()), BarSettings.FromJson(r["current"] as JsonObject, new()));
        var file = ReadFile();
        return (file, file.Clone());
    }

    public Task<string?> PreviewAsync(BarSettings s) => Send("preview", s);

    public async Task<string?> CommitAsync(BarSettings s)
    {
        if (Connected)
            return await Send("commit", s);
        try
        {
            WriteFile(s);
            return null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return e.Message;
        }
    }

    public Task<string?> CancelAsync() => Send("cancel", null);

    // Puts a desktop widget in the first free place ("clock", "medium"); null on success.
    public async Task<string?> AddWidgetAsync(string kind, string size)
    {
        var r = await RequestAsync(new JsonObject { ["op"] = "add-widget", ["kind"] = kind, ["size"] = size });
        if (r == null) return "Atrium 未运行";
        return r["op"]?.GetValue<string>() == "ok" ? null : r["message"]?.GetValue<string>() ?? "error";
    }

    // Null on success, otherwise an error message.
    private async Task<string?> Send(string op, BarSettings? s)
    {
        var request = new JsonObject { ["op"] = op };
        if (s != null) request["settings"] = s.ToJson();
        bool connected = Connected;
        var r = await RequestAsync(request);
        // Not connected: nothing to preview. A connection lost on the way: not done.
        if (r == null) return connected ? "no reply" : null;
        return r["op"]?.GetValue<string>() == "ok" ? null : r["message"]?.GetValue<string>() ?? "error";
    }

    private static BarSettings ReadFile()
    {
        BarSettings s;
        try
        {
            s = BarSettings.FromJson(JsonNode.Parse(File.ReadAllText(SettingsPath)) as JsonObject, new());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
            s = new BarSettings();
        }
        // First run (no "pins" key yet): show what the core would import.
        s.Pins ??= ImportTaskbarPins();
        return s;
    }

    // The pins of the Windows taskbar, as the core imports them on first run.
    public static List<PinnedApp> ImportTaskbarPins()
    {
        var pins = new List<PinnedApp>();
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "Microsoft", "Internet Explorer", "Quick Launch", "User Pinned", "TaskBar");
            if (!Directory.Exists(dir)) return pins;
            foreach (var f in Directory.EnumerateFiles(dir, "*.lnk").OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
                pins.Add(new PinnedApp(Path.GetFileNameWithoutExtension(f), f, "", ""));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
        return pins.Count > 64 ? pins.GetRange(0, 64) : pins;
    }

    private static void WriteFile(BarSettings s)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath)!);
        var tmp = SettingsPath + ".tmp";
        File.WriteAllText(tmp, s.ToJson().ToJsonString(), new UTF8Encoding(false));
        File.Move(tmp, SettingsPath, overwrite: true);
        using var run = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run");
        if (s.StartOnLogin)
        {
            var exe = CoreExe() ?? throw new FileNotFoundException("没有找到 Atrium.exe");
            run.SetValue("Atrium", $"\"{exe}\"");
        }
        else run.DeleteValue("Atrium", throwOnMissingValue: false);
    }

    public void Dispose()
    {
        Disconnect();
        // In-flight requests still release this semaphore after Disconnect completes their I/O.
    }
}
