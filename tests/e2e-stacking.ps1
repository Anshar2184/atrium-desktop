<#
.SYNOPSIS
  Stacking of the bar (D25): raised again over a topmost window that covers
  it; auto-hide (slides away while a full-screen or maximized app is in
  front, comes up at the screen's bottom edge below it and goes again when
  the pointer leaves, back when the app leaves); kept below a revealed
  (auto-hidden) system taskbar the user points at beside the bar.

.DESCRIPTION
  Opens test windows of its own and clicks into them, and moves the pointer
  to the bottom edge to reveal an auto-hidden taskbar, so it only runs after
  the user has been idle for 30 s. The z-order is read with GetWindow, not
  from Atrium's log. Restores the cursor, the foreground window and the
  settings file afterwards.
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
function Click($x, $y) { MoveTo $x $y; Start-Sleep -Milliseconds 120; [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero) }

try {
    [IO.File]::WriteAllText($file, '{"notifyListener":false,"notifyWeChat":false,"notifyFeishu":false,"widgets":false,"pins":[],"showRunning":false,"showClock":true}')
    Start-Process $Core -ArgumentList "--debug-layout"
    Start-Sleep 2
    $bar = [U]::FindWindow("AtriumBarVisual", [NullString]::Value)
    $br = New-Object U+R; [U]::GetWindowRect($bar, [ref]$br) | Out-Null
    $sw = [U]::GetSystemMetrics(0); $sh = [U]::GetSystemMetrics(1)

    # A topmost window that appears over the bar: the bar comes back on top.
    $x = [int](($br.l + $br.r) / 2 - 200); $y = $br.t
    $t = TestForm "Atrium Cover" "`$f.TopMost = `$true; `$f.StartPosition = 'Manual'; `$f.Location = New-Object Drawing.Point($x, $y); `$f.Size = New-Object Drawing.Size(400, 160)"
    Start-Sleep 2.5
    $cr = New-Object U+R; [U]::GetWindowRect($t[1], [ref]$cr) | Out-Null
    $overlaps = $cr.l -lt $br.r -and $cr.r -gt $br.l -and $cr.t -lt $br.b -and $cr.b -gt $br.t
    Check "raised-over-topmost" ($overlaps -and [U]::Above($bar, $t[1])) "a topmost window opened over the bar ($($cr.l),$($cr.t),$($cr.r),$($cr.b)); the bar is above it after 2.5 s: $([U]::Above($bar, $t[1]))"
    $t[0].Kill(); Start-Sleep 0.5

    # A full-screen window in front tucks the bar away; closing it brings the bar back.
    $t = TestForm "Atrium Fullscreen" "`$f.FormBorderStyle = 'None'; `$f.StartPosition = 'Manual'; `$f.Bounds = [Windows.Forms.Screen]::PrimaryScreen.Bounds; `$f.BackColor = [Drawing.Color]::Black"
    Click ([int]($sw / 2)) ([int]($sh / 3)); Start-Sleep 1.5
    $fg = [U]::GetForegroundWindow() -eq $t[1]
    Check "fullscreen-hides" ($fg -and -not [U]::IsWindowVisible($bar)) "full-screen window in front: $fg; bar visible: $([U]::IsWindowVisible($bar))"
    $t[0].Kill(); Start-Sleep 1.8
    Check "fullscreen-restores" ([U]::IsWindowVisible($bar)) "after the full-screen window closed the bar is visible: $([U]::IsWindowVisible($bar))"

    # A maximized window tucks the bar away too; the bottom edge below the bar
    # brings it up over the window, leaving lets it go again.
    $t = TestForm "Atrium Maximized" "`$f.WindowState = 'Maximized'"
    Click ([int]($sw / 2)) ([int]($sh / 3)); Start-Sleep 1.5
    $fg = [U]::GetForegroundWindow() -eq $t[1]
    Check "maximized-tucks" ($fg -and -not [U]::IsWindowVisible($bar)) "maximized window in front: $fg; bar visible: $([U]::IsWindowVisible($bar))"
    MoveTo ([int](($br.l + $br.r) / 2)) ($sh - 1); Start-Sleep 0.9
    Check "edge-reveals" ([U]::IsWindowVisible($bar) -and [U]::Above($bar, $t[1])) "pointer at the bottom edge below the bar: bar visible $([U]::IsWindowVisible($bar)), above the window $([U]::Above($bar, $t[1]))"
    MoveTo ([int](($br.l + $br.r) / 2)) ([int](($br.t + $br.b) / 2)); Start-Sleep 1.2
    Check "stays-while-on-bar" ([U]::IsWindowVisible($bar)) "pointer on the bar for 1.2 s: bar visible $([U]::IsWindowVisible($bar))"
    MoveTo ([int]($sw / 2)) ([int]($sh / 3)); Start-Sleep 1.5
    Check "leave-tucks" (-not [U]::IsWindowVisible($bar)) "pointer gone from the bar for 1.5 s: bar visible $([U]::IsWindowVisible($bar))"
    $t[0].Kill(); Start-Sleep 1.5
    Check "maximized-gone-restores" ([U]::IsWindowVisible($bar)) "after the maximized window closed the bar is visible: $([U]::IsWindowVisible($bar))"

    # An auto-hidden system taskbar revealed at the bottom edge stays in front of the bar.
    $stuck = (Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StuckRects3" -ErrorAction SilentlyContinue).Settings
    if ($stuck -and ($stuck[8] -band 1)) {
        MoveTo ([int]($br.l / 2)) ($sh - 1); Start-Sleep 1.6 # beside the bar: below it the edge is the bar's
        $tray = [U]::FindWindow("Shell_TrayWnd", [NullString]::Value)
        $tr = New-Object U+R; [U]::GetWindowRect($tray, [ref]$tr) | Out-Null
        $revealed = $tr.t -lt $br.b
        Check "below-revealed-taskbar" ($revealed -and [U]::Above($tray, $bar)) "taskbar top $($tr.t) (bar bottom $($br.b)); taskbar above the bar: $([U]::Above($tray, $bar))"
        MoveTo ([int]($sw / 2)) ([int]($sh / 3)); Start-Sleep 1.2
    } else {
        "SKIP below-revealed-taskbar: the system taskbar is not set to hide automatically (not counted as passed)"
    }
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
