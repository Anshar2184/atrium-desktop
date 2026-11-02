<#
.SYNOPSIS
  Motion and material of v0.4: the dock magnification under the pointer (the
  bar stretches, then settles), the launch bounce, the press ripple in the
  glass, the popups' glass layer (start menu, notification) with the bar's
  corner radius, and the bar's width gliding when an app closes.

.DESCRIPTION
  Moves the mouse and clicks, so it only runs after the user has been idle for
  30 s. Reads what the render thread really drew from `--command bar-state`.
  Restores the cursor, the foreground window and the settings file afterwards.
#>
param([string]$Core = (Join-Path (Split-Path -Parent $PSScriptRoot) "native/build/win-x64/Release/Atrium.Core.exe"))
$ErrorActionPreference = "Stop"
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class U {
  [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
  [StructLayout(LayoutKind.Sequential)] public struct L { public uint s; public uint t; }
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool alt);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref L l);
  [DllImport("kernel32.dll")] static extern uint GetTickCount();
  public static double Idle() { var l = new L(); l.s = 8; GetLastInputInfo(ref l); return (GetTickCount() - l.t) / 1000.0; }
  public static string Class(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
  // True if window a is above window b in the z-order.
  public static bool Above(IntPtr a, IntPtr b) { for (IntPtr w = GetWindow(b, 3); w != IntPtr.Zero; w = GetWindow(w, 3)) if (w == a) return true; return false; }
}
'@
[U]::SetProcessDPIAware() | Out-Null
if ([U]::Idle() -lt 30) { "SKIP: the user is active (idle $([U]::Idle()) s)"; exit 0 }

$file = Join-Path $env:LOCALAPPDATA "Atrium/settings.json"
$logFile = Join-Path $env:LOCALAPPDATA "Atrium/logs/core.log"
$backup = if (Test-Path $file) { [IO.File]::ReadAllBytes($file) } else { $null }
$cursor = New-Object U+P; [U]::GetCursorPos([ref]$cursor) | Out-Null
$userForeground = [U]::GetForegroundWindow()
$logStart = if (Test-Path $logFile) { @(Get-Content $logFile -Encoding UTF8).Count } else { 0 }
$failed = 0
$children = @()
function Check($name, $ok, $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail
}
function NewLog { @(Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $script:logStart) }
function C { Start-Process $Core -ArgumentList (@("--command") + $args) -Wait }
function ItemRect($keyPart) {
    Start-Sleep -Milliseconds 300
    $lines = NewLog | Where-Object { $_ -match "layout: .*$([regex]::Escape($keyPart)) (-?\d+),(-?\d+),(-?\d+),(-?\d+)" }
    if (-not $lines) { throw "no layout for $keyPart" }
    $m = [regex]::Match(@($lines)[-1], "(-?\d+),(-?\d+),(-?\d+),(-?\d+)$")
    return @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value, [int]$m.Groups[3].Value, [int]$m.Groups[4].Value)
}
function Center($r) { @([int](($r[0] + $r[2]) / 2), [int](($r[1] + $r[3]) / 2)) }
function MoveTo($x, $y) { [U]::SetCursorPos($x + 1, $y) | Out-Null; Start-Sleep -Milliseconds 40; [U]::SetCursorPos($x, $y) | Out-Null }
# A WinForms window in a child process; $setup is PowerShell run on the form `$f before it is shown.
function TestForm($title, $setup) {
    $code = "Add-Type -MemberDefinition '[DllImport(`"user32.dll`")] public static extern bool SetProcessDPIAware();' -Name Dpi -Namespace W; [W.Dpi]::SetProcessDPIAware() | Out-Null; " +
        "Add-Type -AssemblyName System.Windows.Forms; Add-Type -AssemblyName System.Drawing; `$f = New-Object Windows.Forms.Form; `$f.Text = '$title'; $setup; [Windows.Forms.Application]::Run(`$f)"
    $p = Start-Process "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList "-NoProfile", "-EncodedCommand", ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))) -PassThru
    $script:children += $p
    for ($i = 0; $i -lt 40; $i++) { $h = [U]::FindWindow([NullString]::Value, $title); if ($h -ne [IntPtr]::Zero) { return @($p, $h) }; Start-Sleep -Milliseconds 250 }
    throw "test window $title did not appear"
}
function Frame {
    C bar-state
    Start-Sleep -Milliseconds 350
    $line = @(NewLog | Where-Object { $_ -match "bar: frame " })[-1]
    $m = [regex]::Match($line, "shape=(-?\d+)-(-?\d+) base=(-?\d+)-(-?\d+) magnified=([\d.]+) hop=([\d.]+) ripple=([\d.]+)")
    if (-not $m.Success) { throw "no frame state" }
    [pscustomobject]@{ Width = [int]$m.Groups[2].Value - [int]$m.Groups[1].Value; Base = [int]$m.Groups[4].Value - [int]$m.Groups[3].Value
        Magnified = [double]$m.Groups[5].Value; Hop = [double]$m.Groups[6].Value; Ripple = [double]$m.Groups[7].Value; Line = $line }
}

try {
    $pins = @("where.exe", "charmap.exe", "magnify.exe") | ForEach-Object { '{"name":"' + $_ + '","path":"C:\\Windows\\System32\\' + $_ + '"}' }
    [IO.File]::WriteAllText($file, '{"notifyListener":false,"notifyWeChat":false,"notifyFeishu":false,"widgets":false,"showRunning":false,"showClock":true,"cornerRadius":30,"height":60,"magnify":1.7,"pins":[' + ($pins -join ",") + ']}')
    Start-Process $Core -ArgumentList "--debug-layout"
    Start-Sleep 2

    # Magnification: the pointer on an app grows the icons around it; the bar stretches.
    $c = Center (ItemRect "charmap.exe")
    MoveTo $c[0] $c[1]; Start-Sleep -Milliseconds 700
    $f = Frame
    Check "magnify-stretches" ($f.Magnified -ge 0.95 -and $f.Width -ge $f.Base + 20) "pointer on an app: $($f.Line)"
    MoveTo $c[0] ($c[1] - 600); Start-Sleep -Milliseconds 900
    $f = Frame
    Check "magnify-settles" ($f.Magnified -eq 0 -and $f.Width -eq $f.Base) "pointer off the bar: $($f.Line)"

    # Launch bounce and press ripple: where.exe is not running, so a click starts it and its icon hops.
    $c = Center (ItemRect "where.exe")
    MoveTo $c[0] $c[1]; Start-Sleep -Milliseconds 400
    [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    $f = Frame
    $bounce = (NewLog) -match "bar: bounce .*where\.exe \(4\)"
    Check "launch-bounce" ([bool]$bounce -and $f.Hop -gt 0) "click on a pinned app that is not running: $($f.Line)"
    Check "press-ripple" ($f.Ripple -gt 0) "the glass ripples after a press: $($f.Line)"
    MoveTo $c[0] ($c[1] - 600); Start-Sleep 2.5

    # Popups: the glass layer comes from the bar's renderer; corners follow the bar.
    C start; Start-Sleep 1
    C start-state; Start-Sleep -Milliseconds 300
    $ready = (NewLog) -match "render: popup glass ready for Atrium Start"
    $st = @(NewLog | Where-Object { $_ -match "start: state" })[-1]
    Check "start-menu-glass" ([bool]$ready) "$(@($ready)[-1])"
    Check "start-menu-radius" ($st -match "radius=30") "the bar's radius is 30 DIP: $st"
    C start; Start-Sleep -Milliseconds 400
    C notify-quiet 5
    C notify "Atrium|Glass|A card on the bar's material|"
    Start-Sleep 1
    C notify-state; Start-Sleep -Milliseconds 300
    $ready = (NewLog) -match "render: popup glass ready for Atrium Notification$|render: popup glass ready for Atrium Notification "
    $st = @(NewLog | Where-Object { $_ -match "notify: state" })[-1]
    Check "notification-glass" ([bool]$ready -and $st -match "radius=30") "$(@($ready)[-1]); $st"
    C notify-clear

    # An app that is not pinned closes: the bar's width glides to its new size
    # (the window keeps its width; the shape eases) instead of jumping.
    Start-Process $Core -ArgumentList "--exit" -Wait
    [IO.File]::WriteAllText($file, '{"notifyListener":false,"notifyWeChat":false,"notifyFeishu":false,"widgets":false,"showRunning":true,"showClock":true,"pins":[]}')
    Start-Process $Core -ArgumentList "--debug-layout"
    Start-Sleep 2
    $app = Start-Process "$env:SystemRoot\System32\charmap.exe" -PassThru
    $script:children += $app
    Start-Sleep 2.5
    $before = Frame
    $mark = @(Get-Content $logFile -Encoding UTF8).Count
    $app.Kill()
    for ($i = 0; $i -lt 6; $i++) { Start-Process $Core -ArgumentList "--command", "bar-state"; Start-Sleep -Milliseconds 50 }
    Start-Sleep 1.5
    $after = Frame
    $widths = @(Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $mark | Where-Object { $_ -match "bar: frame shape=(-?\d+)-(-?\d+) base" } |
        ForEach-Object { [regex]::Match($_, "shape=(-?\d+)-(-?\d+)") } | ForEach-Object { [int]$_.Groups[2].Value - [int]$_.Groups[1].Value })
    $between = @($widths | Where-Object { $_ -lt $before.Width -and $_ -gt $after.Base })
    Check "close-glides" ($after.Base -lt $before.Base -and $after.Width -eq $after.Base -and $between.Count -ge 1) "bar $($before.Base) -> $($after.Base) px; widths drawn meanwhile: $($widths -join ',')"
}
finally {
    foreach ($p in $children) { if (-not $p.HasExited) { $p.Kill() } }
    Start-Process $Core -ArgumentList "--exit" -Wait
    if ($backup) { [IO.File]::WriteAllBytes($file, $backup) } else { Remove-Item $file -ErrorAction SilentlyContinue }
    [U]::SetCursorPos($cursor.x, $cursor.y) | Out-Null
    if ($userForeground -ne [IntPtr]::Zero) { [U]::SwitchToThisWindow($userForeground, $true) }
}
"failed: $failed"
exit $failed
