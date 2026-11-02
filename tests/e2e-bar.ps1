<#
.SYNOPSIS
  Clicks real items on the Atrium bar and checks what Windows does: minimize
  the foreground app, restore it, cycle between two windows of one app, open
  the calendar from the clock, reorder pins by drag and drop, cancel a drag by
  dropping away from the bar, pin a running app by dragging it among the
  pins, bring an app to the front by holding a dragged file over it, and open
  a dropped file with a pinned app, and the overflow entry of a narrow bar.

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
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
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
function Center($r) { @([int](($r[0] + $r[2]) / 2), [int](($r[1] + $r[3]) / 2)) }
function Drag($r, $toX, $toY) {
    $c = Center $r
    [U]::SetCursorPos($c[0], $c[1]) | Out-Null
    Start-Sleep -Milliseconds 150
    [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    for ($i = 1; $i -le 15; $i++) {
        [U]::SetCursorPos([int]($c[0] + ($toX - $c[0]) * $i / 15), [int]($c[1] + ($toY - $c[1]) * $i / 15)) | Out-Null
        Start-Sleep -Milliseconds 30
    }
    Start-Sleep -Milliseconds 250
    [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 900
}
function PinOrder { @((Get-Content $file -Raw -Encoding UTF8 | ConvertFrom-Json).pins | ForEach-Object { [IO.Path]::GetFileName($_.path).ToLowerInvariant() }) -join "," }
function Click($r) {
    [U]::SetCursorPos([int](($r[0] + $r[2]) / 2), [int](($r[1] + $r[3]) / 2)) | Out-Null
    Start-Sleep -Milliseconds 150
    [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 700
}

try {
    [IO.File]::WriteAllText($file, '{"notifyListener":false,"notifyWeChat":false,"notifyFeishu":false,"widgets":false,"pins":[],"showRunning":true,"showClock":true}')
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
    $calendar = [U]::FindWindow("AtriumSurface", "Atrium Calendar")
    Check "clock-opens-calendar" ($calendar -ne [IntPtr]::Zero -and $fg -eq $calendar) "foreground after clicking the clock: $([U]::Class($fg)) (Atrium's calendar: $calendar)"
    [U]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero); [U]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero) # Esc closes the panel
    Start-Sleep 0.5

    # Drag and drop, with three pins (never launched) and the test windows running.
    Start-Process $Core -ArgumentList "--exit" -Wait
    $pins = @("cmd.exe", "charmap.exe", "magnify.exe", "where.exe") | ForEach-Object { '{"name":"' + $_ + '","path":"C:\\Windows\\System32\\' + $_ + '"}' }
    [IO.File]::WriteAllText($file, '{"notifyListener":false,"notifyWeChat":false,"notifyFeishu":false,"widgets":false,"showRunning":true,"showClock":true,"pins":[' + ($pins -join ",") + ']}')
    $script:logStart = @(Get-Content $logFile -Encoding UTF8).Count
    Start-Process $Core -ArgumentList "--debug-layout"
    Start-Sleep 2
    $to = Center (ItemRect "magnify.exe")
    Drag (ItemRect "cmd.exe") $to[0] $to[1]
    $order = PinOrder
    Check "drag-reorders-pins" ($order -eq "charmap.exe,magnify.exe,cmd.exe,where.exe") "pins after dragging the first onto the third: $order"

    $r = ItemRect "charmap.exe"
    Drag $r (Center $r)[0] ($r[1] - 200)
    $order = PinOrder
    Check "drag-away-cancels" ($order -eq "charmap.exe,magnify.exe,cmd.exe,where.exe") "pins after dropping far above the bar: $order"

    $first = ItemRect "charmap.exe"
    Drag (ItemRect $key) ($first[0] + 4) (Center $first)[1]
    $order = PinOrder
    Check "drag-pins-running-app" ($order -eq "powershell.exe,charmap.exe,magnify.exe,cmd.exe,where.exe") "pins after dragging a running app in front of them: $order"

    # Files dragged from another app (an STA PowerShell 7 window, so it is not
    # in the test windows' group): hovering brings the test app to the front,
    # dropping on where.exe (a console tool that exits at once) opens the file with it.
    $drop = Join-Path $env:TEMP "atrium-drop-test.txt"
    Set-Content $drop "atrium" -Encoding ASCII
    $code = "Add-Type -AssemblyName System.Windows.Forms; `$f = New-Object Windows.Forms.Form; `$f.Text = 'Atrium Drag Source'; `$f.Width = 360; `$f.Height = 240; `$f.StartPosition = 'CenterScreen'; `$f.TopMost = `$true; " +
        "`$f.Add_MouseDown({ `$d = New-Object Windows.Forms.DataObject; `$c = New-Object System.Collections.Specialized.StringCollection; [void]`$c.Add('$drop'); `$d.SetFileDropList(`$c); [void]`$f.DoDragDrop(`$d, [Windows.Forms.DragDropEffects]::Copy) }); [Windows.Forms.Application]::Run(`$f)"
    $pwsh = (Get-Command pwsh.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source
    if ($pwsh) {
        $p = Start-Process $pwsh -ArgumentList "-NoProfile", "-STA", "-EncodedCommand", ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))) -PassThru
        $script:children += $p
        $src = [IntPtr]::Zero
        for ($i = 0; $i -lt 60 -and $src -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $src = [U]::FindWindow([NullString]::Value, "Atrium Drag Source") }
        $wr = New-Object U+R; [U]::GetWindowRect($src, [ref]$wr) | Out-Null
        [U]::SwitchToThisWindow($src, $true); Start-Sleep -Milliseconds 400
        $target = Center (ItemRect $key)
        [U]::SetCursorPos([int](($wr.l + $wr.r) / 2), [int](($wr.t + $wr.b) / 2)) | Out-Null
        Start-Sleep -Milliseconds 200
        [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 200
        $start = @([int](($wr.l + $wr.r) / 2), [int](($wr.t + $wr.b) / 2))
        for ($i = 1; $i -le 20; $i++) {
            [U]::SetCursorPos([int]($start[0] + ($target[0] - $start[0]) * $i / 20), [int]($start[1] + ($target[1] - $start[1]) * $i / 20)) | Out-Null
            Start-Sleep -Milliseconds 30
        }
        for ($i = 0; $i -lt 12; $i++) { [U]::SetCursorPos($target[0] + ($i % 2), $target[1]) | Out-Null; Start-Sleep -Milliseconds 100 }
        $fg = [U]::GetForegroundWindow()
        Check "file-hover-shows-app" ($fg -eq $a -or $fg -eq $b) "foreground after holding a file over the app for 1.2 s: $(if ($fg -eq $a -or $fg -eq $b) { 'test window' } else { [U]::Class($fg) })"
        $w = Center (ItemRect "where.exe")
        for ($i = 1; $i -le 10; $i++) {
            [U]::SetCursorPos([int]($target[0] + ($w[0] - $target[0]) * $i / 10), $w[1]) | Out-Null
            Start-Sleep -Milliseconds 30
        }
        Start-Sleep -Milliseconds 300
        [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep 1.5
        $opened = (Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $script:logStart) -match "opening 1 file\(s\) with C:\\Windows\\System32\\where.exe"
        Check "file-drop-opens" ([bool]$opened) "core log reports opening the dropped file with where.exe"
    } else {
        "SKIP file-drag: PowerShell 7 (pwsh.exe) is not installed"
    }
    Remove-Item $drop -ErrorAction SilentlyContinue

    # Overflow: a narrow bar keeps the apps that do not fit behind an entry.
    Start-Process $Core -ArgumentList "--exit" -Wait
    [IO.File]::WriteAllText($file, '{"notifyListener":false,"notifyWeChat":false,"notifyFeishu":false,"widgets":false,"showRunning":true,"showClock":true,"width":300,"pins":[' + ($pins -join ",") + ']}')
    $script:logStart = @(Get-Content $logFile -Encoding UTF8).Count
    Start-Process $Core -ArgumentList "--debug-layout"
    Start-Sleep 2
    $more = ItemRect "#more"
    Click $more
    $menu = [U]::FindWindow("#32768", [NullString]::Value)
    Check "overflow-entry" ($menu -ne [IntPtr]::Zero) "a 300 DIP bar shows the overflow entry; clicking it lists the hidden apps in a menu"
    [U]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero); [U]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero)
    Start-Sleep 0.3
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
