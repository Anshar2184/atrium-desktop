<#
.SYNOPSIS
  Atrium's tray panel and control centre with the real mouse: the tray entry
  and the status icon on the bar, the panel listing a test tray icon (a
  WinForms NotifyIcon in a child process), a left and a right click on it in
  the panel reaching that program as its own tray clicks, the control centre's
  pieces of glass and its Bluetooth page, the wheel over the status icon and the volume slider (dragged from its symbol too), the sound's
  page (output devices, apps) and the cast page (wireless displays, projection modes), and
  a notice from that program opening it through its tray icon, and
  the calendar from the clock (a month with lunar dates, turning the page).

.DESCRIPTION
  Moves the mouse, so it only runs after the user has been idle for 30 s.
  Restores the cursor, the foreground window, the volume and the settings
  file; the test icon removes itself.
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
$clicks = Join-Path $env:TEMP "atrium-tray-test-clicks.txt"
$stop = Join-Path $env:TEMP "atrium-tray-test-stop.txt"
Remove-Item $clicks, $stop -ErrorAction SilentlyContinue
$failed = 0
$child = $null
$volume0 = $null
$muted0 = $null
function Check($name, $ok, $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail
}
function NewLog { (Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $script:logStart) }
function C { Start-Process $Core -ArgumentList (@("--command") + $args) -Wait; Start-Sleep -Milliseconds 250 }
function Last($pattern) { @(NewLog | Where-Object { $_ -match $pattern })[-1] }
function Field($line, $key) { if ($line -match " $key=([^ ]+)") { $Matches[1] } else { $null } }
function ItemRect($key) {
    $line = Last "layout: $([regex]::Escape($key)) "
    if (-not $line) { throw "no layout for $key" }
    $m = [regex]::Match($line, "(-?\d+),(-?\d+),(-?\d+),(-?\d+)$")
    @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value, [int]$m.Groups[3].Value, [int]$m.Groups[4].Value)
}
function ClickAt($x, $y, [switch]$Right) {
    [U]::SetCursorPos([int]$x, [int]$y) | Out-Null
    Start-Sleep -Milliseconds 150
    $down = if ($Right) { 8 } else { 2 }
    [U]::mouse_event($down, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [U]::mouse_event($down * 2, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 700
}
function Wheel($x, $y, $delta) { [U]::SetCursorPos([int]$x, [int]$y) | Out-Null; Start-Sleep -Milliseconds 150; [U]::mouse_event(0x0800, 0, 0, [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$delta), 0), [UIntPtr]::Zero); Start-Sleep -Milliseconds 700 }
function Key($vk) { [U]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); [U]::keybd_event($vk, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 300 }
function Centre($r) { @([int](($r[0] + $r[2]) / 2), [int](($r[1] + $r[3]) / 2)) }
function TrayState { C tray-state; Last "tray: state" }
function ControlState { C control-state; Last "control: state" }
function Spot($pattern, $id) {
    $line = Last $pattern
    $m = [regex]::Match($line, " $([regex]::Escape($id))@(\d+),(\d+)")
    if (-not $m.Success) { throw "no spot $id in: $line" }
    @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value)
}

try {
    # A tray icon of our own: it writes the button of every click it gets.
    $code = @"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
`$n = New-Object Windows.Forms.NotifyIcon
`$n.Icon = [System.Drawing.SystemIcons]::Information
`$n.Text = 'Atrium tray test'
`$n.Visible = `$true
`$n.add_MouseClick({ param(`$s, `$e) Add-Content -Path '$clicks' -Value `$e.Button })
`$t = New-Object Windows.Forms.Timer
`$t.Interval = 300
`$t.add_Tick({ if (Test-Path '$stop') { `$n.Visible = `$false; `$n.Dispose(); [Windows.Forms.Application]::Exit() } })
`$t.Start()
[Windows.Forms.Application]::Run()
"@
    $child = Start-Process "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList "-NoProfile", "-EncodedCommand", ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))) -PassThru -WindowStyle Hidden
    [IO.File]::WriteAllText($file, '{"notifyListener":false,"notifyWeChat":false,"notifyFeishu":false,"pins":[],"showRunning":false,"showClock":true}')
    Start-Process $Core -ArgumentList "--debug-layout"
    Start-Sleep 4

    $tray = ItemRect "#tray"; $status = ItemRect "#status"
    Check "entries-on-bar" (($status[2] - $status[0]) -gt 40 -and ($tray[2] - $tray[0]) -gt 40 -and $tray[2] -le $status[0]) "tray entry $($tray -join ','), status icon $($status -join ',') (right of the tray entry)"

    $p = Centre $tray; ClickAt $p[0] $p[1]; Start-Sleep 1
    $st = TrayState
    Check "tray-panel-opens" ((Field $st "visible") -eq "1" -and (Field $st "foreground") -eq "1" -and (Field $st "available") -eq "1" -and [int](Field $st "icons") -ge 1) $st

    C tray-find "Atrium tray test"
    $found = Last "tray: (found|not found)"
    Check "test-icon-listed" ($found -match "found icon:") $found
    if ($found -match "@(\d+),(\d+)") {
        ClickAt ([int]$Matches[1]) ([int]$Matches[2]); Start-Sleep 1.5
        $got = if (Test-Path $clicks) { @(Get-Content $clicks) } else { @() }
        $st = TrayState
        Check "left-click-reaches-app" ($got -contains "Left" -and (Field $st "visible") -eq "0") "the icon's program got: $($got -join ','); panel visible=$(Field $st 'visible')"

        $p = Centre (ItemRect "#tray"); ClickAt $p[0] $p[1]; Start-Sleep 1
        C tray-find "Atrium tray test"
        if ((Last "tray: (found|not found)") -match "@(\d+),(\d+)") {
            ClickAt ([int]$Matches[1]) ([int]$Matches[2]) -Right; Start-Sleep 1.5
        }
        $got = if (Test-Path $clicks) { @(Get-Content $clicks) } else { @() }
        Check "right-click-reaches-app" ($got -contains "Right") "the icon's program got: $($got -join ',')"
        $st = TrayState
        if ((Field $st "visible") -eq "1") { Key 0x1B }

        # A notice from an app that lives in the tray: clicking it is the app's
        # own tray click (QQ then opens the unread chat, not a second instance's login).
        $lefts = @($got | Where-Object { $_ -eq "Left" }).Count
        C notify-quiet 5
        C notify "Atrium tray test|Message|From an app in the tray|app:Atrium tray test"
        Start-Sleep 1
        C notify-click card:0
        Start-Sleep 1.5
        $got = if (Test-Path $clicks) { @(Get-Content $clicks) } else { @() }
        $via = @(NewLog | Where-Object { $_ -match "notify: action through the tray icon" })
        Check "notice-opens-tray-app" (@($got | Where-Object { $_ -eq "Left" }).Count -gt $lefts -and $via.Count -ge 1) "the icon's program got: $($got -join ','); $(@($via)[-1])"
        C notify-clear
    }

    # The calendar from the clock: this month, a month card and a capsule; the arrow turns the page.
    $p = Centre (ItemRect "#clock"); ClickAt $p[0] $p[1]; Start-Sleep 1
    C calendar-state; $st = Last "calendar: state"
    $now = Get-Date
    Check "calendar-opens" ((Field $st "visible") -eq "1" -and (Field $st "month") -eq $now.ToString("yyyy-MM") -and (Field $st "shapes") -eq "2" -and (Field $st "lunar")) $st
    $next = Spot "calendar: spots" "next"
    ClickAt $next[0] $next[1]; Start-Sleep 0.6
    C calendar-state; $st = Last "calendar: state"
    Check "calendar-next-month" ((Field $st "month") -eq $now.AddMonths(1).ToString("yyyy-MM")) $st
    Key 0x1B
    C calendar-state; $st = Last "calendar: state"
    Check "calendar-esc-closes" ((Field $st "visible") -eq "0") $st

    # Control centre: pieces of glass for the connections, the battery, casting, the sound and,
    # with a built-in screen, its brightness.
    $p = Centre (ItemRect "#status"); ClickAt $p[0] $p[1]; Start-Sleep 2.5
    $st = ControlState
    $volume0 = [int](Field $st "volume"); $muted0 = Field $st "muted"
    $expected = 4 + $(if ([int](Field $st "brightness") -ge 0) { 1 } else { 0 })
    Check "control-centre-opens" ((Field $st "visible") -eq "1" -and (Field $st "page") -eq "main" -and [int](Field $st "shapes") -eq $expected) $st
    # The Bluetooth row opens its page in place (one piece of glass); Esc goes back.
    if ((Field $st "bluetooth") -eq "1") {
        $bt = Spot "control: spots" "bt-page"
        ClickAt $bt[0] $bt[1]; Start-Sleep 2
        $sb = ControlState
        Key 0x1B; Start-Sleep 0.6
        $back = ControlState
        Check "bluetooth-page" ((Field $sb "page") -eq "bluetooth" -and [int](Field $sb "shapes") -eq 1 -and (Field $back "page") -eq "main" -and (Field $back "visible") -eq "1") "$sb; after Esc page=$(Field $back 'page')"
    } else {
        "SKIP bluetooth-page: no Bluetooth radio"
    }
    $slider = Spot "control: spots" "slider"
    ClickAt $slider[0] $slider[1]; Start-Sleep 0.5
    $v = [int](Field (ControlState) "volume")
    Check "slider-sets-volume" ($v -ge 35 -and $v -le 65) "a click in the middle of the slider: volume $volume0 -> $v"
    # A press on the slider's symbol that moves drags the slider (a press in place mutes).
    C control-volume 10; C control-state
    $mute = Spot "control: spots" "mute"; $slider = Spot "control: spots" "slider"
    [U]::SetCursorPos($mute[0], $mute[1]) | Out-Null; Start-Sleep -Milliseconds 150
    [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    for ($i = 1; $i -le 10; $i++) { [U]::SetCursorPos([int]($mute[0] + ($slider[0] - $mute[0]) * $i / 10), $mute[1]) | Out-Null; Start-Sleep -Milliseconds 30 }
    [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 500
    $sd = ControlState
    Check "slider-drags-from-symbol" ([int](Field $sd "volume") -ge 35 -and [int](Field $sd "volume") -le 65 -and (Field $sd "muted") -eq "0") "a drag from the symbol to the middle: volume=$(Field $sd 'volume') muted=$(Field $sd 'muted')"
    # The sound's page: the playback devices (the default one marked) and the apps; choosing the
    # default device again changes nothing. Only read here: the test never switches the user's device.
    C control-state
    $sp = Spot "control: spots" "sound-page"
    ClickAt $sp[0] $sp[1]; Start-Sleep 2.5
    $ss = ControlState
    $def = [int](Field $ss "defaultOutput")
    if ($def -ge 0) { C control-click "out:$def"; Start-Sleep 1 }
    $ss2 = ControlState
    Key 0x1B; Start-Sleep 0.6
    Check "sound-page" ((Field $ss "page") -eq "sound" -and [int](Field $ss "outputs") -ge 1 -and $def -ge 0 -and [int](Field $ss2 "defaultOutput") -eq $def -and (Field (ControlState) "page") -eq "main") "$ss; after choosing it again defaultOutput=$(Field $ss2 'defaultOutput')"
    # The cast page: the wireless displays (searched for) and Win+P's modes (read, never set here).
    $cp = Spot "control: spots" "cast"
    ClickAt $cp[0] $cp[1]; Start-Sleep 3
    $cs = ControlState
    Key 0x1B; Start-Sleep 0.6
    Check "cast-page" ((Field $cs "page") -eq "cast" -and [int](Field $cs "mode") -ge 1 -and [int](Field $cs "shapes") -eq 1) $cs
    C control-volume $volume0
    Key 0x1B

    # The wheel over the status icon: 2 points per notch.
    $p = Centre (ItemRect "#status"); Wheel $p[0] $p[1] 120
    $p2 = Centre (ItemRect "#status"); ClickAt $p2[0] $p2[1]; Start-Sleep 1
    $up = [int](Field (ControlState) "volume")
    Key 0x1B
    Check "wheel-changes-volume" ($up -eq [Math]::Min(100, $volume0 + 2)) "volume $volume0 -> $up after one notch up"
}
finally {
    if ($volume0 -ne $null) {
        C control-volume $volume0
        if ($muted0 -eq "1") { C control-click mute } # the slider and the wheel unmute
    }
    New-Item -ItemType File $stop -Force | Out-Null
    Start-Sleep 1
    if ($child -and -not $child.HasExited) { $child.Kill() }
    Start-Process $Core -ArgumentList "--exit" -Wait
    Remove-Item $clicks, $stop -ErrorAction SilentlyContinue
    if ($backup) { [IO.File]::WriteAllBytes($file, $backup) } else { Remove-Item $file -ErrorAction SilentlyContinue }
    [U]::SetCursorPos($cursor.x, $cursor.y) | Out-Null
    if ($userForeground -ne [IntPtr]::Zero) { [U]::SwitchToThisWindow($userForeground, $true) }
}
"failed: $failed"
exit $failed
