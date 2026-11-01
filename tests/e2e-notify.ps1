<#
.SYNOPSIS
  Atrium notifications end to end, with simulated notices (Atrium.Core.exe
  --command notify ...) and the real mouse: popup without taking the focus,
  timeout, hover keeps it, queue and merge, expand the stack (upwards, every
  card its own piece of glass), dismiss one,
  click action, clear, unread badge, full-screen suppression (simulated and
  with a real full-screen window), cards waiting while another Atrium panel
  is open, hidden text with capture exclusion, and the
  window's place and size at the monitor's DPI.

.DESCRIPTION
  Moves the mouse, so it only runs after the user has been idle for 30 s.
  Restores the cursor, the foreground window and the settings file.
#>
param([string]$Core = (Join-Path (Split-Path -Parent $PSScriptRoot) "native/build/win-x64/Release/Atrium.Core.exe"))
$ErrorActionPreference = "Stop"
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class U {
  [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l, t, r, b; }
  [StructLayout(LayoutKind.Sequential)] public struct L { public uint s; public uint t; }
  [StructLayout(LayoutKind.Sequential)] public struct MI { public int size; public R mon; public R work; public uint flags; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowDisplayAffinity(IntPtr h, out uint a);
  [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint f);
  [DllImport("user32.dll")] public static extern bool GetMonitorInfo(IntPtr m, ref MI i);
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
$psKey = "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
function Check($name, $ok, $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail
}
function NewLog { (Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $script:logStart) }
function C { Start-Process $Core -ArgumentList (@("--command") + $args) -Wait }
function Post($app, $title, $body, $action = "") { C notify "$app|$title|$body|$action"; Start-Sleep -Milliseconds 250 }
function State {
    C notify-state
    Start-Sleep -Milliseconds 150
    $line = @(NewLog | Where-Object { $_ -match "notify: state" })[-1]
    $s = @{}
    foreach ($m in [regex]::Matches($line, "(\w+)=('[^']*'|[^ ]+)")) { $s[$m.Groups[1].Value] = $m.Groups[2].Value.Trim("'") }
    $s["line"] = $line
    $s
}
# Centre (screen px) of a spot of the visible notification window ("popup" or "center").
function Spot($view, $id) {
    State | Out-Null
    $line = @(NewLog | Where-Object { $_ -match "notify: spots $view" })[-1]
    $m = [regex]::Match($line, " $([regex]::Escape($id))@(\d+),(\d+)")
    if (-not $m.Success) { throw "no spot $id in: $line" }
    @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value)
}
function ClickAt($p) {
    [U]::SetCursorPos($p[0], $p[1]) | Out-Null; Start-Sleep -Milliseconds 200
    [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 600
}
function TestWindow($title, [switch]$FullScreen) {
    $extra = if ($FullScreen) { "`$f.FormBorderStyle = 'None'; `$f.WindowState = 'Maximized'; `$f.TopMost = `$true;" } else { "`$f.StartPosition = 'Manual'; `$f.Left = 60; `$f.Top = 60;" }
    $code = "Add-Type -AssemblyName System.Windows.Forms; `$f = New-Object Windows.Forms.Form; `$f.Text = '$title'; `$f.Width = 420; `$f.Height = 260; $extra [Windows.Forms.Application]::Run(`$f)"
    $p = Start-Process "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList "-NoProfile", "-EncodedCommand", ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))) -PassThru
    $script:children += $p
    for ($i = 0; $i -lt 40; $i++) { $h = [U]::FindWindow([NullString]::Value, $title); if ($h -ne [IntPtr]::Zero) { return $h }; Start-Sleep -Milliseconds 250 }
    throw "test window $title did not appear"
}
function FocusWindow($h) { $r = New-Object U+R; [U]::GetWindowRect($h, [ref]$r) | Out-Null; ClickAt @([int](($r.l + $r.r) / 2), [int](($r.t + $r.b) / 2)) }
function StartCore($json) {
    Start-Process $Core -ArgumentList "--exit" -Wait
    [IO.File]::WriteAllText($file, $json)
    Start-Process $Core -ArgumentList "--debug-layout"
    Start-Sleep 2.5
}

try {
    StartCore '{"notifyListener":false,"notifyWeChat":false,"pins":[],"showRunning":true,"notifyTimeout":3}'
    $a = TestWindow "Atrium Notify Test"
    FocusWindow $a

    Post "Mail" "Weekly report" "Sent to 5 people." 
    $s = State
    $popup = [U]::FindWindow("AtriumSurface", "Atrium Notification")
    Check "popup-without-focus" ($s.popup -eq "1" -and [U]::GetForegroundWindow() -eq $a) "popup visible, foreground still the test window ($([U]::Class([U]::GetForegroundWindow())))"
    Check "unread-badge" ($s.unread -eq "1") "unread=$($s.unread) after one notice"

    # Place and size: right-aligned near the bell, above the bar, inside the work area, 384 DIP wide.
    # (The window adds a margin for the glass's shadow; the cards' area is logged.)
    $pm = [regex]::Match((@(NewLog | Where-Object { $_ -match "notify: state" })[-1]), "popupRect=(-?\d+),(-?\d+),(-?\d+),(-?\d+)")
    $r = New-Object U+R; $r.l = [int]$pm.Groups[1].Value; $r.t = [int]$pm.Groups[2].Value; $r.r = [int]$pm.Groups[3].Value; $r.b = [int]$pm.Groups[4].Value
    $sc = [U]::GetDpiForWindow($popup) / 96.0
    $mi = New-Object U+MI; $mi.size = 40; [U]::GetMonitorInfo([U]::MonitorFromWindow($popup, 2), [ref]$mi) | Out-Null
    $bell = @(NewLog | Where-Object { $_ -match "layout: #notify" })[-1]
    $bm = [regex]::Match($bell, "(-?\d+),(-?\d+),(-?\d+),(-?\d+)$")
    $inside = $r.l -ge $mi.work.l -and $r.r -le $mi.work.r -and $r.t -ge $mi.work.t -and $r.b -le $mi.work.b
    Check "bounds-and-dpi" ($inside -and [math]::Abs(($r.r - $r.l) - 384 * $sc) -le 1 -and $r.b -le [int]$bm.Groups[2].Value) "popup $($r.l),$($r.t)-$($r.r),$($r.b) at $([int]($sc * 100))%, work area $($mi.work.l),$($mi.work.t)-$($mi.work.r),$($mi.work.b), bell top $($bm.Groups[2].Value)"

    Start-Sleep 3.5
    $s = State
    Check "timeout" ($s.popup -eq "0" -and $s.unread -eq "1") "after the 3 s timeout: popup=$($s.popup), still unread=$($s.unread)"

    Post "Mail" "Weekly report" "Second copy."
    $s = State
    Check "merge" ($s.count -eq "1" -and $s.firstCount -eq "2") "same app and title: count=$($s.count) merged x$($s.firstCount)"

    Post "Calendar" "Design review" "Room 3B, in 15 minutes"
    Post "Chat" "Alex" "Looks good."
    $s = State
    Check "queue" ($s.popupCards -eq "3" -and $s.first -eq "Alex") "popup stacks $($s.popupCards) cards, newest '$($s.first)' on top"

    # Hover keeps the popup past its timeout (the pointer goes there right after
    # a new notice restarted the timer).
    Post "Chat" "Sam" "Lunch?"
    $popup = [U]::FindWindow("AtriumSurface", "Atrium Notification")
    $r = New-Object U+R; [U]::GetWindowRect($popup, [ref]$r) | Out-Null
    $p = @([int](($r.l + $r.r) / 2), [int](($r.t + $r.b) / 2 + 20))
    [U]::SetCursorPos($p[0], $p[1]) | Out-Null
    Start-Sleep 4
    $s = State
    Check "hover-keeps" ($s.popup -eq "1") "pointer on the popup for 4 s (timeout 3 s): popup=$($s.popup)"

    ClickAt $p
    $s = State
    Check "click-expands" ($s.center -eq "1" -and $s.expanded -eq "1" -and $s.popup -eq "0" -and $s.unread -eq "0") "clicking the stack opens the expanded list (center=$($s.center) expanded=$($s.expanded)) and marks all read"

    # Upwards: the newest card lowest (next to the bar), older ones above it,
    # the list's capsule below them; every card and the capsule is a piece of glass.
    Start-Sleep -Milliseconds 400
    $c0 = Spot "center" "card:0"; $c1 = Spot "center" "card:1"; $cl = Spot "center" "clear"
    $s = State
    Check "stacks-upwards" ($c0[1] -gt $c1[1] -and $cl[1] -gt $c0[1] -and [int]$s.centerShapes -eq [int]$s.count + 1) "newest card at y $($c0[1]), the next older at $($c1[1]), the capsule at $($cl[1]); $($s.centerShapes) pieces of glass for $($s.count) cards"

    $before = [int]$s.count
    $card = Spot "center" "card:1"
    [U]::SetCursorPos($card[0], $card[1]) | Out-Null; Start-Sleep -Milliseconds 400
    ClickAt (Spot "center" "close:1")
    $s = State
    Check "dismiss-one" ([int]$s.count -eq $before - 1) "count $before -> $($s.count)"

    # Click action: bring the sender (the test window's app) to the front.
    C notify-open
    FocusWindow $a
    # Another app in front (Character Map: not in the test windows' group).
    $charmap = Start-Process "$env:SystemRoot\System32\charmap.exe" -PassThru
    $script:children += $charmap
    Start-Sleep 1.5
    $other = $charmap.MainWindowHandle
    FocusWindow $other
    Post "PowerShell" "Build finished" "Click to switch to the app" "app:$psKey"
    ClickAt (Spot "popup" "card:0")
    Start-Sleep 0.8
    $fg = [U]::GetForegroundWindow()
    Check "click-runs-action" (((NewLog) -match "notify: activated \d+ action 'app:.*' ok") -and $fg -eq $a) "log reports the action; the sender's window replaced Character Map in front ($([U]::Class($fg)))"

    C notify-open; Start-Sleep 0.5
    $s = State
    if ($s.center -ne "1") { C notify-open; Start-Sleep 0.5 }
    ClickAt (Spot "center" "clear")
    $s = State
    Check "clear" ($s.count -eq "0" -and $s.unread -eq "0") "after clearing: count=$($s.count)"
    if ($s.center -eq "1") { C notify-open }

    # Full screen: simulated state, then a real full-screen window.
    C notify-quiet 3
    Post "Mail" "During a game" "Should not pop up"
    $s = State
    Check "suppressed-simulated" ($s.popup -eq "0" -and $s.unread -eq "1") "state 3 (D3D full screen): popup=$($s.popup), unread=$($s.unread)"
    C notify-quiet 0
    C notify-clear
    $full = TestWindow "Atrium Full Screen" -FullScreen
    FocusWindow $full
    Start-Sleep 1
    Post "Mail" "During a presentation" "Should not pop up"
    $s = State
    Check "suppressed-real-fullscreen" ($s.popup -eq "0" -and $s.quiet -ne "5") "real full-screen window: user notification state $($s.quiet), popup=$($s.popup)"
    foreach ($p in $children) { if (-not $p.HasExited -and $p.MainWindowTitle -eq "Atrium Full Screen") { $p.Kill() } }
    Get-Process powershell -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -eq "Atrium Full Screen" } | Stop-Process -Force
    C notify-clear

    # Another of Atrium's panels open: a new card waits for it to close instead of overlapping it.
    C control-open; Start-Sleep 1
    Post "Mail" "While a panel is open" "Shown after it closes"
    $s = State
    $held = $s.popup -eq "0" -and $s.popupCards -eq "1"
    C control-open; Start-Sleep 1
    $s = State
    Check "held-by-panel" ($held -and $s.popup -eq "1") "while the control centre was open the card waited ($held); after it closed: popup=$($s.popup)"
    C notify-clear

    # Hidden text: setting on, cards excluded from capture.
    # A real Windows toast (from PowerShell's own identity, tagged so it can be
    # removed again) comes back as an Atrium card through the listener.
    StartCore '{"notifyListener":true,"notifyWeChat":false,"pins":[],"showRunning":true,"notifyTimeout":3}'
    $listening = @(NewLog | Where-Object { $_ -match "listener: (mirroring|access)" })[-1]
    if ($listening -notmatch "mirroring") {
        "SKIP windows-toast: $listening"
    } else {
        [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
        [Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime] | Out-Null
        $psApp = '{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\WindowsPowerShell\v1.0\powershell.exe'
        $xml = New-Object Windows.Data.Xml.Dom.XmlDocument
        $xml.LoadXml("<toast><visual><binding template='ToastGeneric'><text>Atrium toast test</text><text>mirrored</text></binding></visual></toast>")
        $toast = New-Object Windows.UI.Notifications.ToastNotification $xml
        $toast.Tag = 'atrium-e2e'; $toast.Group = 'atrium'
        [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($psApp).Show($toast)
        Start-Sleep 3
        $s = State
        [Windows.UI.Notifications.ToastNotificationManager]::History.Remove('atrium-e2e', 'atrium', $psApp)
        Check "windows-toast" ($s.first -eq "Atrium toast test" -and $s.popup -eq "1") "a Windows toast became the card '$($s.first)' (popup=$($s.popup)); the toast was removed from the notification centre again"
    }

    StartCore '{"notifyListener":false,"notifyWeChat":false,"pins":[],"showRunning":true,"notifyTimeout":5,"notifyHideBody":true}'
    Post "Bank" "Verification code" "Your code is 123456"
    $s = State
    $popup = [U]::FindWindow("AtriumSurface", "Atrium Notification")
    $aff = 0; [U]::GetWindowDisplayAffinity($popup, [ref]$aff) | Out-Null
    Check "hide-body" ($s.bodyHidden -eq "1" -and $aff -eq 0x11) "bodyHidden=$($s.bodyHidden), display affinity 0x$('{0:X}' -f $aff) (0x11 = excluded from capture)"
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
