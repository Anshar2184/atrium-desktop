<#
.SYNOPSIS
  Atrium's lock screen (a cover that hands over to Windows' own lock) with the
  real keyboard and mouse, in dry-run mode (ATRIUM_LOCK_DRYRUN=1: the hand-over
  is only logged, the computer is not locked): the cover shows the clock as a
  distance-field piece of glass, the bar steps aside, keys do not reach the
  window in front, the first key ends the cover and asks for the lock, the bar
  comes back; a click does the same.

.DESCRIPTION
  Presses keys and clicks, so it only runs after the user has been idle for
  30 s. Restores the cursor, the foreground window and the settings file.
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
  [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool alt);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref L l);
  [DllImport("kernel32.dll")] static extern uint GetTickCount();
  public static double Idle() { var l = new L(); l.s = 8; GetLastInputInfo(ref l); return (GetTickCount() - l.t) / 1000.0; }
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
$typed = Join-Path $env:TEMP "atrium-lock-test-typed.txt"
Remove-Item $typed -ErrorAction SilentlyContinue
$failed = 0
$child = $null
function Check($name, $ok, $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail
}
function NewLog { (Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $script:logStart) }
function C { Start-Process $Core -ArgumentList (@("--command") + $args) -Wait; Start-Sleep -Milliseconds 250 }
function Last($pattern) { @(NewLog | Where-Object { $_ -match $pattern })[-1] }
function Field($line, $key) { if ($line -match " $key=([^ ]+)") { $Matches[1] } else { $null } }
function Key($vk) { [U]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); [U]::keybd_event($vk, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 400 }

try {
    # A window with a text box in front: what reaches it is written to a file.
    $code = @"
Add-Type -AssemblyName System.Windows.Forms
`$f = New-Object Windows.Forms.Form; `$f.Text = 'Atrium Lock Test'; `$f.Width = 420; `$f.Height = 200
`$t = New-Object Windows.Forms.TextBox; `$t.Dock = 'Fill'; `$t.Multiline = `$true; `$f.Controls.Add(`$t)
`$t.Add_TextChanged({ [IO.File]::WriteAllText('$typed', `$t.Text) })
[Windows.Forms.Application]::Run(`$f)
"@
    $child = Start-Process "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList "-NoProfile", "-EncodedCommand", ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))) -PassThru
    $win = [IntPtr]::Zero
    for ($i = 0; $i -lt 40 -and $win -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $win = [U]::FindWindow([NullString]::Value, "Atrium Lock Test") }
    [IO.File]::WriteAllText($file, '{"notifyListener":false,"notifyWeChat":false,"notifyFeishu":false,"pins":[],"showRunning":false,"showClock":true}')
    $env:ATRIUM_LOCK_DRYRUN = "1"
    Start-Process $Core
    Start-Sleep 3
    [U]::SwitchToThisWindow($win, $true); Start-Sleep 0.5

    C lock-open; Start-Sleep 1.5
    C lock-state; $st = Last "lock: state"
    C bar-state; $bar = Last "bar: state"
    Check "cover-shows" ((Field $st "visible") -eq "1" -and (Field $st "hook") -eq "1" -and (Field $st "sdf") -notmatch "^0x" -and [int](Field $st "shapes") -ge 1) $st
    Check "bar-steps-aside" ((Field $bar "visible") -eq "0") $bar

    Key 0x41 # 'A': ends the cover; it must not reach the text box
    C lock-state; $st = Last "lock: state"
    $ended = Last "lock: cover ended"
    $text = if (Test-Path $typed) { [IO.File]::ReadAllText($typed) } else { "" }
    Check "key-hands-over" ((Field $st "visible") -eq "0" -and $ended -match "dry run" -and $text -eq "") "$ended; the window in front got '$text'"
    C bar-state; $bar = Last "bar: state"
    Check "bar-returns" ((Field $bar "visible") -eq "1") $bar

    C lock-open; Start-Sleep 1.5
    [U]::SetCursorPos(1600, 1000) | Out-Null; Start-Sleep -Milliseconds 300
    [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep 0.6
    C lock-state; $st = Last "lock: state"
    Check "click-hands-over" ((Field $st "visible") -eq "0" -and @(NewLog | Where-Object { $_ -match "lock: cover ended.*dry run" }).Count -ge 2) $st
}
finally {
    C lock-dismiss
    Start-Process $Core -ArgumentList "--exit" -Wait
    Remove-Item Env:ATRIUM_LOCK_DRYRUN -ErrorAction SilentlyContinue
    if ($child -and -not $child.HasExited) { $child.Kill() }
    Remove-Item $typed -ErrorAction SilentlyContinue
    if ($backup) { [IO.File]::WriteAllBytes($file, $backup) } else { Remove-Item $file -ErrorAction SilentlyContinue }
    [U]::SetCursorPos($cursor.x, $cursor.y) | Out-Null
    if ($userForeground -ne [IntPtr]::Zero) { [U]::SwitchToThisWindow($userForeground, $true) }
}
"failed: $failed"
exit $failed
