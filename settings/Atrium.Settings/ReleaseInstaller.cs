using System.Diagnostics;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;

namespace Atrium.Settings;

// Download/stage in the user cache. Only the user's restart action runs the
// short-lived replacement script; no updater service or background installer.
internal static class ReleaseInstaller
{
    internal sealed record Prepared(string Tag, string Version, string Directory);
    internal static string InstallDirectory => Path.GetDirectoryName(CoreClient.CoreExe()
        ?? throw new IOException("找不到 Atrium 程序目录。"))!;
    internal static string CacheDirectory => Path.Combine(Path.GetDirectoryName(CoreClient.SettingsPath)!, "Updates",
        Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(InstallDirectory.ToUpperInvariant())))[..16]);
    private static string StatePath => Path.Combine(CacheDirectory, "ready.json");
    internal static string ResultPath => Path.Combine(CacheDirectory, "result.json");

    internal static Prepared? ReadPrepared(Version current)
    {
        if (!File.Exists(StatePath)) return null;
        var ready = JsonSerializer.Deserialize<Prepared>(File.ReadAllText(StatePath));
        if (ready == null || !Version.TryParse(ready.Version, out var version) || version <= current) return null;
        if (!Path.GetFullPath(ready.Directory).StartsWith(CacheDirectory + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new IOException("更新缓存路径无效，请重新下载。");
        ValidatePackage(ready.Directory, version);
        return ready;
    }

    internal static string? ReadFailure()
    {
        if (!File.Exists(ResultPath)) return null;
        using var result = JsonDocument.Parse(File.ReadAllText(ResultPath));
        string? error = result.RootElement.GetProperty("error").GetString();
        File.Delete(ResultPath);
        return string.IsNullOrEmpty(error) ? null : error;
    }

    internal static async Task<Prepared> PrepareAsync(ReleaseInfo release, IProgress<string> progress, CancellationToken cancel)
    {
        Directory.CreateDirectory(CacheDirectory);
        string work = Path.Combine(CacheDirectory, "package-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(work);
        try
        {
            string zip = Path.Combine(work, "release.zip");
            using (var response = await ReleaseInfo.Http.GetAsync(release.Download!, HttpCompletionOption.ResponseHeadersRead, cancel))
            {
                response.EnsureSuccessStatusCode();
                using var input = await response.Content.ReadAsStreamAsync(cancel);
                using var output = File.Create(zip);
                byte[] buffer = new byte[81920];
                long total = response.Content.Headers.ContentLength ?? 0, downloaded = 0;
                int lastPercent = -1;
                while (true)
                {
                    int count = await input.ReadAsync(buffer, cancel);
                    if (count == 0) break;
                    await output.WriteAsync(buffer.AsMemory(0, count), cancel);
                    downloaded += count;
                    int percent = total > 0 ? (int)(downloaded * 100 / total) : 0;
                    if (percent != lastPercent)
                    {
                        progress.Report(total > 0 ? $"正在下载… {percent}%" : "正在下载…");
                        lastPercent = percent;
                    }
                }
                if (total > 0 && downloaded != total) throw new IOException("下载未完成，请重试。");
            }
            progress.Report("正在准备安装…");
            string package = await Task.Run(() =>
            {
                cancel.ThrowIfCancellationRequested();
                if (release.Digest is { } digest)
                {
                    using var stream = File.OpenRead(zip);
                    string actual = "sha256:" + Convert.ToHexString(SHA256.HashData(stream));
                    if (!actual.Equals(digest, StringComparison.OrdinalIgnoreCase)) throw new IOException("更新包校验失败，请重新下载。");
                }
                // ExtractToDirectory rejects entries escaping the destination directory.
                string unpacked = Path.Combine(work, "unpacked");
                ZipFile.ExtractToDirectory(zip, unpacked);
                cancel.ThrowIfCancellationRequested();
                string root = File.Exists(Path.Combine(unpacked, "Atrium.exe")) ? unpacked : Path.Combine(unpacked, "Atrium");
                ValidatePackage(root, release.Version);
                File.Delete(zip);
                return root;
            }, cancel);
            var prepared = new Prepared(release.Tag, release.Version.ToString(), package);
            string temporary = StatePath + ".tmp";
            await File.WriteAllTextAsync(temporary, JsonSerializer.Serialize(prepared), cancel);
            File.Move(temporary, StatePath, true);
            return prepared;
        }
        catch
        {
            Directory.Delete(work, true);
            throw;
        }
    }

    private static void ValidatePackage(string directory, Version expected)
    {
        string core = Path.Combine(directory, "Atrium.exe");
        string settings = Path.Combine(directory, "Settings", "Atrium.Settings.exe");
        if (!File.Exists(core) || !File.Exists(settings) || !File.Exists(Path.ChangeExtension(settings, ".dll")))
            throw new IOException("更新包缺少程序文件，请重新下载。");
        var info = FileVersionInfo.GetVersionInfo(core);
        var version = new Version(info.FileMajorPart, info.FileMinorPart, info.FileBuildPart, info.FilePrivatePart);
        var settingsInfo = FileVersionInfo.GetVersionInfo(Path.ChangeExtension(settings, ".dll"));
        var settingsVersion = new Version(settingsInfo.FileMajorPart, settingsInfo.FileMinorPart, settingsInfo.FileBuildPart, settingsInfo.FilePrivatePart);
        if (version != expected || settingsVersion != expected) throw new IOException("更新包与发布版本不一致，请重新下载。");
    }

    internal static void Restart(Prepared ready, bool startCore)
    {
        ValidatePackage(ready.Directory, Version.Parse(ready.Version));
        // Copy the helper out of the install directory before replacing that directory.
        string script = Path.Combine(CacheDirectory, "Update.ps1");
        // Windows PowerShell 5.1 needs a BOM to read the helper's Chinese messages.
        File.WriteAllText(script, File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Update.ps1")), new System.Text.UTF8Encoding(true));
        var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
            "WindowsPowerShell", "v1.0", "powershell.exe")) { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = CacheDirectory };
        foreach (string arg in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script,
            "-InstallDirectory", InstallDirectory, "-PackageDirectory", ready.Directory, "-StatePath", StatePath,
            "-WaitForProcessId", Environment.ProcessId.ToString(), "-StartCore", startCore ? "1" : "0" }) start.ArgumentList.Add(arg);
        // Check before closing any UI. Program Files installations need a writable
        // parent directory; do not stop the app and then discover access was denied.
        string probe = Path.Combine(Path.GetDirectoryName(InstallDirectory)!, ".atrium-update-" + Guid.NewGuid().ToString("N"));
        using (File.Create(probe)) { }
        File.Delete(probe);
        using var helper = Process.Start(start) ?? throw new IOException("无法启动更新，请重试。");
    }
}
