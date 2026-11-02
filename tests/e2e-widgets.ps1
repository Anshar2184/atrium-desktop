<#
.SYNOPSIS
  Atrium's desktop widgets: the layout read from widgets.json, their layer
  (just above the desktop, under every window; over it when every app is
  minimised), dragging one with the real mouse (it snaps to the grid and the
  layout is saved), changing a size, adding and removing, and turning them off.

.DESCRIPTION
  Moves the mouse, so it only runs after the user has been idle for 30 s.
  Restores the cursor, the foreground window, the settings file and widgets.json.
#>
param([string]$Core = (Join-Path (Split-Path -Parent $PSScriptRoot) "native/build/win-x64/Release/Atrium.Core.exe"))
$ErrorActionPreference = "Stop"
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class U {
  [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
  [StructLayout(LayoutKind.Sequential)] public struct L { public uint s; public uint t; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool alt);
  [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref L l);
  [DllImport("kernel32.dll")] static extern uint GetTickCount();
  public static double Idle() { var l = new L(); l.s = 8; GetLastInputInfo(ref l); return (GetTickCount() - l.t) / 1000.0; }
}
'@
[U]::SetProcessDPIAware() | Out-Null
if ([U]::Idle() -lt 30) { "SKIP: the user is active (idle $([U]::Idle()) s)"; exit 0 }

$file = Join-Path $env:LOCALAPPDATA "Atrium/settings.json"
$wfile = Join-Path $env:LOCALAPPDATA "Atrium/widgets.json"
$logFile = Join-Path $env:LOCALAPPDATA "Atrium/logs/core.log"
$backup = if (Test-Path $file) { [IO.File]::ReadAllBytes($file) } else { $null }
$wbackup = if (Test-Path $wfile) { [IO.File]::ReadAllBytes($wfile) } else { $null }
$cursor = New-Object U+P; [U]::GetCursorPos([ref]$cursor) | Out-Null
$userForeground = [U]::GetForegroundWindow()
$logStart = if (Test-Path $logFile) { @(Get-Content $logFile -Encoding UTF8).Count } else { 0 }
$failed = 0
function Check($name, $ok, $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail
}
function NewLog { (Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $script:logStart) }
function C { Start-Process $Core -ArgumentList (@("--command") + $args) -Wait; Start-Sleep -Milliseconds 300 }
function Field($line, $key) { if ($line -match " $key=([^ ]+)") { $Matches[1] } else { $null } }
function State {
    C widgets-state
    $lines = @(NewLog | Where-Object { $_ -match "widgets: (state|w\d+) " })
    $i = [array]::FindLastIndex([string[]]$lines, [Predicate[string]] { param($l) $l -match "widgets: state " })
    if ($i -lt 0) { return @() }
    $n = [int](Field $lines[$i] "count")
    @($lines[$i..($i + $n)])
}
function Rect($line) { (Field $line "rect") -split "," | ForEach-Object { [int]$_ } }
function Layout { (Get-Content $wfile -Raw -Encoding UTF8 | ConvertFrom-Json).widgets }
function Settings($widgets) {
    [IO.File]::WriteAllText($file, '{"notifyListener":false,"notifyWeChat":false,"notifyFeishu":false,"widgets":' + $widgets + ',"pins":[],"showRunning":false,"showClock":true}')
}

try {
    # Two widgets where the file says.
    Settings "true"
    [IO.File]::WriteAllText($wfile, '{"widgets":[{"kind":"clock","size":"small","col":0,"row":0},{"kind":"calendar","size":"small","col":1,"row":0}]}')
    Start-Process $Core
    Start-Sleep 4
    $st = @(State)
    Check "layout-read" ($st.Count -eq 3 -and (Field $st[1] "kind") -eq "clock" -and (Field $st[1] "cell") -eq "0,0" -and (Field $st[2] "kind") -eq "calendar" -and (Field $st[2] "cell") -eq "1,0" -and (Field $st[1] "visible") -eq "1") ($st -join " | ")
    # Just above the desktop and under the windows; or, with every app minimised, over the desktop.
    $raised = (Field $st[0] "raised") -eq "1"
    $layerOk = if ($raised) { (Field $st[1] "topmost") -eq "1" } else { (Field $st[1] "topmost") -eq "0" -and (Field $st[1] "under") -in @("Progman", "WorkerW") }
    Check "layer" $layerOk "raised=$(Field $st[0] 'raised') topmost=$(Field $st[1] 'topmost') under=$(Field $st[1] 'under')"
    $r0 = Rect $st[1]; $r1 = Rect $st[2]
    Check "grid" ($r0[1] -eq $r1[1] -and $r1[0] -gt $r0[2] -and ($r0[2] - $r0[0]) -eq ($r0[3] - $r0[1])) "clock $($r0 -join ','), calendar $($r1 -join ',')"
    Start-Process $Core -ArgumentList "--exit" -Wait
    Start-Sleep 1

    # Over the windows for the mouse (the test mode), then drag the clock two cells right and one down.
    $env:ATRIUM_WIDGETS_TEST = "1"
    Start-Process $Core
    Start-Sleep 4
    $st = @(State)
    $r0 = Rect $st[1]; $r1 = Rect $st[2]
    $pitch = $r1[0] - $r0[0]
    $x = [int](($r0[0] + $r0[2]) / 2); $y = [int](($r0[1] + $r0[3]) / 2)
    [U]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 200
    [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    for ($i = 1; $i -le 20; $i++) { [U]::SetCursorPos([int]($x + 2.1 * $pitch * $i / 20), [int]($y + 0.9 * $pitch * $i / 20)) | Out-Null; Start-Sleep -Milliseconds 25 }
    [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep 0.8
    $st = @(State)
    $moved = Layout | Where-Object { $_.kind -eq "clock" }
    Check "drag-snaps" ((Field $st[1] "cell") -eq "2,1" -and $moved.col -eq 2 -and $moved.row -eq 1) "after the drag: $(Field $st[1] 'cell'), saved $($moved.col),$($moved.row)"
    $r = Rect $st[1]
    Check "drag-glides-home" ($r[0] -eq $r0[0] + 2 * $pitch -and $r[1] -eq $r0[1] + $pitch) "rect $($r -join ',') (pitch $pitch)"

    # Sizes, adding, removing.
    C widgets-do "size:1:medium"; Start-Sleep 0.6
    $st = @(State)
    $r = Rect $st[2]
    Check "size-medium" ((Field $st[2] "size") -eq "medium" -and ($r[2] - $r[0]) -gt 1.9 * ($r[3] - $r[1])) "calendar $(Field $st[2] 'size') rect $($r -join ',')"
    C widgets-do "add:battery:small"; Start-Sleep 0.8
    $st = @(State)
    Check "add" ($st.Count -eq 4 -and (Field $st[3] "kind") -eq "battery" -and (Field $st[3] "cell") -eq "0,0" -and @(Layout).Count -eq 3) "added: $(Field $st[3] 'kind') at $(Field $st[3] 'cell'), file $(@(Layout).Count)"
    C widgets-do "remove:0"; Start-Sleep 0.8
    $st = @(State)
    Check "remove" ($st.Count -eq 3 -and @(Layout | Where-Object { $_.kind -eq "clock" }).Count -eq 0) "left: $(($st | Select-Object -Skip 1 | ForEach-Object { Field $_ 'kind' }) -join ',')"
    Start-Process $Core -ArgumentList "--exit" -Wait
    Start-Sleep 1
    Remove-Item Env:ATRIUM_WIDGETS_TEST

    # Turned off: no widgets, the layout kept for later.
    Settings "false"
    Start-Process $Core
    Start-Sleep 4
    $st = @(State)
    Check "off" ((Field $st[0] "enabled") -eq "0" -and @(Layout).Count -eq 2) "$($st[0]); file keeps $(@(Layout).Count)"
}
finally {
    Start-Process $Core -ArgumentList "--exit" -Wait
    Remove-Item Env:ATRIUM_WIDGETS_TEST -ErrorAction SilentlyContinue
    if ($backup) { [IO.File]::WriteAllBytes($file, $backup) } else { Remove-Item $file -ErrorAction SilentlyContinue }
    if ($wbackup) { [IO.File]::WriteAllBytes($wfile, $wbackup) } else { Remove-Item $wfile -ErrorAction SilentlyContinue }
    [U]::SetCursorPos($cursor.x, $cursor.y) | Out-Null
    if ($userForeground -ne [IntPtr]::Zero) { [U]::SwitchToThisWindow($userForeground, $true) }
}
"failed: $failed"
exit $failed
