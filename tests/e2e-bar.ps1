<#
.SYNOPSIS
  Clicks real items on the Atrium bar and checks what Windows does: minimize
  the foreground app, restore it, cycle between two windows of one app, and
  open the calendar from the clock.

.DESCRIPTION
  Uses two test windows of its own (WinForms in child processes), moves the
  mouse and clicks, so it only runs after the user has been idle for 30 s.
  Restores the cursor, the foreground window and the settings file afterwards.
#>
param([string]$Core = (Join-Path (Split-Path -Parent $PSScriptRoot) "native/build/win-x64/Release/Atrium.Core.exe"))
$ErrorActionPreference = "Stop"
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class U {
  [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
  [StructLayout(LayoutKind.Sequential)] public struct L { public uint s; public uint t; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool alt);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref L l);
  [DllImport("kernel32.dll")] static extern uint GetTickCount();
  public static double Idle() { var l = new L(); l.s = 8; GetLastInputInfo(ref l); return (GetTickCount() - l.t) / 1000.0; }
  public static string Class(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
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
function TestWindow($title) {
    $code = "Add-Type -AssemblyName System.Windows.Forms; `$f = New-Object Windows.Forms.Form; `$f.Text = '$title'; `$f.Width = 420; `$f.Height = 260; `$f.StartPosition = 'CenterScreen'; [Windows.Forms.Application]::Run(`$f)"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    $p = Start-Process "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList "-NoProfile", "-EncodedCommand", $encoded -PassThru
    $script:children += $p
    for ($i = 0; $i -lt 40; $i++) { $h = [U]::FindWindow([NullString]::Value, $title); if ($h -ne [IntPtr]::Zero) { return $h }; Start-Sleep -Milliseconds 250 }
    throw "test window $title did not appear"
}
function ItemRect($keyPart) {
    Start-Sleep -Milliseconds 400
    $lines = Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $script:logStart | Where-Object { $_ -match "layout: .*$([regex]::Escape($keyPart)) (-?\d+),(-?\d+),(-?\d+),(-?\d+)" }
    if (-not $lines) { throw "no layout for $keyPart" }
    $m = [regex]::Match(@($lines)[-1], "(-?\d+),(-?\d+),(-?\d+),(-?\d+)$")
    return @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value, [int]$m.Groups[3].Value, [int]$m.Groups[4].Value)
}
function Click($r) {
    [U]::SetCursorPos([int](($r[0] + $r[2]) / 2), [int](($r[1] + $r[3]) / 2)) | Out-Null
    Start-Sleep -Milliseconds 150
    [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 700
}

try {
    [IO.File]::WriteAllText($file, '{"pins":[],"showRunning":true,"showClock":true}')
    Start-Process $Core -ArgumentList "--debug-layout"
    Start-Sleep 2
    $key = "windowspowershell\v1.0\powershell.exe"

    # A window started from a background script may not get the foreground
    # (foreground lock); a click on its item must bring it in front.
    $a = TestWindow "Atrium Test A"
    $r = ItemRect $key
    Click $r
    Check "click-activates" ([U]::GetForegroundWindow() -eq $a) "clicking the app of a background window brings it in front"
    Click $r
    Check "click-minimizes-foreground" ([U]::IsIconic($a)) "clicking the app of the foreground window minimizes it"
    Click $r
    Check "click-restores" (-not [U]::IsIconic($a) -and [U]::GetForegroundWindow() -eq $a) "clicking a minimized app restores it in front"

    $b = TestWindow "Atrium Test B"
    $r = ItemRect $key
    Click $r
    $first = [U]::GetForegroundWindow()
    Click $r
    $second = [U]::GetForegroundWindow()
    Check "cycle-two-windows" (($first -eq $a -and $second -eq $b) -or ($first -eq $b -and $second -eq $a)) "two windows of one app: successive clicks alternate between them"

    $clock = ItemRect "#clock"
    Click $clock
    Start-Sleep 0.8
    $fg = [U]::GetForegroundWindow()
    $cls = [U]::Class($fg)
    Check "clock-opens-calendar" ($fg -ne $a -and $fg -ne $b -and $cls -ne "WindowsForms10.Window.8.app.0.141b42a_r6_ad1") "foreground after clicking the clock: $cls"
    [U]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero); [U]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero) # Esc closes the panel
    Start-Sleep 0.5
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
