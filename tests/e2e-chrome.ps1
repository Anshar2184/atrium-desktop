<#
.SYNOPSIS
  Hiding Windows' own shell (settings hideTaskbar and hideBanners,
  shell/system_chrome): the system taskbar hidden and hidden again when
  Explorer shows it, Do Not Disturb turned on so no banner shows while a
  Windows toast still becomes an Atrium card, everything put back when the
  settings are turned off, and a run that was killed undone by the next start.

.DESCRIPTION
  No mouse or keyboard. Restores the settings file, the taskbar and the Do
  Not Disturb mode it found.
#>
param([string]$Core = (Join-Path (Split-Path -Parent $PSScriptRoot) "native/build/win-x64/Release/Atrium.Core.exe"))
$ErrorActionPreference = "Stop"
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class U {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
[ComImport, Guid("6BFF4732-81EC-4FFB-AE67-B6C1BC29631F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IQuietHoursSettings { [PreserveSig] int Get(out IntPtr p); [PreserveSig] int Put([MarshalAs(UnmanagedType.LPWStr)] string p); }
public static class Quiet {
  static IQuietHoursSettings Q() { return (IQuietHoursSettings)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("F53321FA-34F8-4B7F-B9A3-361877CB94CF"))); }
  public static string Get() { IntPtr p; if (Q().Get(out p) != 0) return ""; string s = Marshal.PtrToStringUni(p); Marshal.FreeCoTaskMem(p); return s; }
  public static void Set(string s) { Q().Put(s); }
}
'@

$file = Join-Path $env:LOCALAPPDATA "Atrium/settings.json"
$state = Join-Path $env:LOCALAPPDATA "Atrium/system.json"
$logFile = Join-Path $env:LOCALAPPDATA "Atrium/logs/core.log"
$backup = if (Test-Path $file) { [IO.File]::ReadAllBytes($file) } else { $null }
$tray = [U]::FindWindow("Shell_TrayWnd", $null)
$trayVisible = [U]::IsWindowVisible($tray)
$profile0 = [Quiet]::Get()
$logStart = if (Test-Path $logFile) { @(Get-Content $logFile -Encoding UTF8).Count } else { 0 }
$unrestricted = "Microsoft.QuietHoursProfile.Unrestricted"
$failed = 0
function Check($name, $ok, $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail
}
function NewLog { (Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $script:logStart) }
function C { Start-Process $Core -ArgumentList (@("--command") + $args) -Wait; Start-Sleep -Milliseconds 250 }
function Last($pattern) { @(NewLog | Where-Object { $_ -match $pattern })[-1] }
function Field($line, $key) { if ($line -match " $key=([^ ]+)") { $Matches[1] } else { $null } }
function ChromeState { C chrome-state; Last "chrome: state" }
function Connect {
    $p = New-Object System.IO.Pipes.NamedPipeClientStream(".", "Atrium.Core.$((Get-Process -Id $PID).SessionId)", [System.IO.Pipes.PipeDirection]::InOut)
    $p.Connect(3000)
    $p.ReadMode = "Message"
    $p
}
function Req($p, $s) {
    $b = [Text.Encoding]::UTF8.GetBytes($s)
    $p.Write($b, 0, $b.Length); $p.Flush()
    $buf = New-Object byte[] 65536
    $ms = New-Object IO.MemoryStream
    do { $n = $p.Read($buf, 0, $buf.Length); $ms.Write($buf, 0, $n) } while (-not $p.IsMessageComplete)
    [Text.Encoding]::UTF8.GetString($ms.ToArray()) | ConvertFrom-Json
}

if ($profile0 -and $profile0 -ne $unrestricted) { "SKIP: Do Not Disturb is on already ($profile0)"; exit 0 }
$pipe = $null
try {
    Start-Process $Core -ArgumentList "--exit" -Wait
    [IO.File]::WriteAllText($file, '{"notifyListener":true,"notifyWeChat":false,"notifyFeishu":false,"widgets":false,"pins":[],"showRunning":true,"notifyTimeout":4}')
    Start-Process $Core
    Start-Sleep 3

    # Both on, as a preview: dropping the pipe turns them off again.
    $pipe = Connect
    $r = Req $pipe '{"op":"preview","settings":{"hideTaskbar":true,"hideBanners":true}}'
    Start-Sleep 1
    $s = ChromeState
    Check "taskbar-hidden" ($r.op -eq "ok" -and (Field $s "taskbarHidden") -eq "1" -and (Field $s "visible") -eq "0" -and [int](Field $s "taskbars") -ge 1 -and (Field $s "autoHide") -eq "1" -and -not [U]::IsWindowVisible($tray)) $s
    Check "state-kept" ((Test-Path $state) -and (Get-Content $state -Raw) -match '"taskbarHidden":true') "system.json: $(if (Test-Path $state) { Get-Content $state -Raw })"

    [U]::ShowWindow($tray, 8) | Out-Null # SW_SHOWNA, as Explorer does
    Start-Sleep -Milliseconds 400
    Check "hidden-again" (-not [U]::IsWindowVisible($tray)) "visible 400 ms after it was shown: $([U]::IsWindowVisible($tray)); $(Last 'chrome: Explorer showed')"

    $s = ChromeState
    $quns = Field $s "quns"
    Check "banners-off" ((Field $s "quietOurs") -eq "1" -and [Quiet]::Get() -eq "Microsoft.QuietHoursProfile.AlarmsOnly") "profile=$([Quiet]::Get()) quns=$quns"

    $listening = @(NewLog | Where-Object { $_ -match "listener: (mirroring|access)" })[-1]
    if ($listening -notmatch "mirroring") {
        "SKIP toast-still-a-card: $listening"
    } else {
        [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
        [Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime] | Out-Null
        $psApp = '{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\WindowsPowerShell\v1.0\powershell.exe'
        $xml = New-Object Windows.Data.Xml.Dom.XmlDocument
        $xml.LoadXml("<toast><visual><binding template='ToastGeneric'><text>Atrium quiet test</text><text>no banner</text></binding></visual></toast>")
        $toast = New-Object Windows.UI.Notifications.ToastNotification $xml
        $toast.Tag = 'atrium-e2e-quiet'; $toast.Group = 'atrium'
        [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($psApp).Show($toast)
        Start-Sleep 2.5
        C notify-state
        $n = Last "notify: state"
        [Windows.UI.Notifications.ToastNotificationManager]::History.Remove('atrium-e2e-quiet', 'atrium', $psApp)
        Check "toast-still-a-card" ((Field $n "first") -eq "'Atrium" -and (Field $n "popup") -eq "1" -and (Field $n "quiet") -eq "5") "card popup=$(Field $n 'popup') quiet=$(Field $n 'quiet') (system state $quns): $n"
    }

    $pipe.Dispose(); $pipe = $null
    Start-Sleep 1.5
    $s = ChromeState
    Check "put-back" ((Field $s "taskbarHidden") -eq "0" -and [U]::IsWindowVisible($tray) -eq $trayVisible -and [Quiet]::Get() -eq $unrestricted -and -not (Test-Path $state)) "$s; system.json $(Test-Path $state)"

    # Killed while both are on: the next start puts everything back.
    $pipe = Connect
    Req $pipe '{"op":"preview","settings":{"hideTaskbar":true,"hideBanners":true}}' | Out-Null
    Start-Sleep 1
    Get-Process Atrium.Core | Stop-Process -Force
    $pipe.Dispose(); $pipe = $null
    Start-Sleep 1
    $left = (Test-Path $state) -and -not [U]::IsWindowVisible($tray) -and [Quiet]::Get() -ne $unrestricted
    Start-Process $Core
    Start-Sleep 3
    Check "undone-after-kill" ($left -and [U]::IsWindowVisible($tray) -eq $trayVisible -and [Quiet]::Get() -eq $unrestricted -and -not (Test-Path $state)) "left behind by the kill: $left; after the next start: taskbar visible=$([U]::IsWindowVisible($tray)) profile=$([Quiet]::Get()); $(Last 'chrome: undoing')"
}
finally {
    if ($pipe) { $pipe.Dispose() }
    Start-Process $Core -ArgumentList "--exit" -Wait
    if ($backup) { [IO.File]::WriteAllBytes($file, $backup) } else { Remove-Item $file -ErrorAction SilentlyContinue }
    Remove-Item $state -ErrorAction SilentlyContinue
    if ($trayVisible -and -not [U]::IsWindowVisible($tray)) { [U]::ShowWindow($tray, 8) | Out-Null }
    if ($profile0 -and [Quiet]::Get() -ne $profile0) { [Quiet]::Set($profile0) }
}
"failed: $failed"
exit $failed
