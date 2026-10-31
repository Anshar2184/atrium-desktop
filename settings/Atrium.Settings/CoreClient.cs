using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;

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
    private NamedPipeClientStream? _pipe;

    public bool Connected => _pipe is { IsConnected: true };

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
                !string.Equals(Process.GetProcessById((int)pid).ProcessName, "Atrium.Core", StringComparison.OrdinalIgnoreCase))
            {
                pipe.Dispose();
                return false;
            }
            _pipe = pipe;
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
    }

    private async Task<JsonObject?> RequestAsync(JsonObject request)
    {
        await _io.WaitAsync();
        try
        {
            if (_pipe is not { IsConnected: true }) return null;
            var bytes = Encoding.UTF8.GetBytes(request.ToJsonString());
            await _pipe.WriteAsync(bytes);
            await _pipe.FlushAsync();
            var buffer = new byte[64 * 1024];
            using var message = new MemoryStream();
            do
            {
                int n = await _pipe.ReadAsync(buffer);
                if (n == 0) throw new IOException("pipe closed");
                message.Write(buffer, 0, n);
            } while (!_pipe.IsMessageComplete);
            return JsonNode.Parse(message.ToArray()) as JsonObject;
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

    // Null on success, otherwise an error message.
    private async Task<string?> Send(string op, BarSettings? s)
    {
        var request = new JsonObject { ["op"] = op };
        if (s != null) request["settings"] = s.ToJson();
        var r = await RequestAsync(request);
        if (r == null) return Connected ? "no reply" : null; // not connected: nothing to preview
        return r["op"]?.GetValue<string>() == "ok" ? null : r["message"]?.GetValue<string>() ?? "error";
    }

    private static BarSettings ReadFile()
    {
        try
        {
            return BarSettings.FromJson(JsonNode.Parse(File.ReadAllText(SettingsPath)) as JsonObject, new());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
            return new BarSettings();
        }
    }

    private static void WriteFile(BarSettings s)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath)!);
        var tmp = SettingsPath + ".tmp";
        File.WriteAllText(tmp, s.ToJson().ToJsonString(), new UTF8Encoding(false));
        File.Move(tmp, SettingsPath, overwrite: true);
    }

    public void Dispose()
    {
        Disconnect();
        _io.Dispose();
    }
}
