<#
.SYNOPSIS
  Drives Atrium's start menu with the real mouse and keyboard: open from the
  logo (and take the keyboard), type to search, Esc, toggle with the logo,
  close on an outside click, launch with Enter, pin and unpin from the menu,
  the power menu (dry run: nothing is shut down), the tray entry, and the
  window size at the monitor's DPI.

.DESCRIPTION
  Moves the mouse and types, so it only runs after the user has been idle for
  30 s. Uses its own test window; restores the cursor, the foreground window
  and the settings file afterwards and closes the Calculator it starts.
#>
param([string]$Core = (Join-Path (Split-Path -Parent $PSScriptRoot) "native/build/win-x64/Release/Atrium.Core.exe"), [switch]$Trace)
$ErrorActionPreference = "Stop"
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class U {
  [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l, t, r, b; }
  [StructLayout(LayoutKind.Sequential)] public struct L { public uint s; public uint t; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
  [StructLayout(LayoutKind.Explicit, Size = 40)] public struct KI { [FieldOffset(0)] public uint type; [FieldOffset(8)] public ushort vk; [FieldOffset(10)] public ushort scan; [FieldOffset(12)] public uint flags; [FieldOffset(16)] public uint time; [FieldOffset(24)] public UIntPtr extra; }
  [DllImport("user32.dll")] static extern uint SendInput(uint n, KI[] i, int size);
  // Typed text as Unicode key events (VK_PACKET): reaches the window as WM_CHAR
  // without going through the user's input method.
  public static void Type(string s) {
    foreach (char c in s) {
      var i = new KI[2];
      i[0].type = 1; i[0].scan = c; i[0].flags = 4;
      i[1].type = 1; i[1].scan = c; i[1].flags = 4 | 2;
      SendInput(2, i, Marshal.SizeOf(typeof(KI)));
      System.Threading.Thread.Sleep(40);
    }
  }
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
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
# Apps the menu may start (the pinned tool, the search result); the new ones are closed afterwards.
$started = "CalculatorApp", "cmd", "where"
$before = @(Get-Process $started -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
$failed = 0
$children = @()
function Check($name, $ok, $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail
}
function NewLog { (Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $script:logStart) }
function LastState {
    Start-Process $Core -ArgumentList "--command", "start-state" -Wait
    Start-Sleep -Milliseconds 200
    @(NewLog | Where-Object { $_ -match "start: state" })[-1]
}
function ItemRect($keyPart) {
    $lines = NewLog | Where-Object { $_ -match "layout: $([regex]::Escape($keyPart)) (-?\d+),(-?\d+),(-?\d+),(-?\d+)" }
    if (-not $lines) { throw "no layout for $keyPart" }
    $m = [regex]::Match(@($lines)[-1], "(-?\d+),(-?\d+),(-?\d+),(-?\d+)$")
    return @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value, [int]$m.Groups[3].Value, [int]$m.Groups[4].Value)
}
function ClickAt($x, $y, [switch]$Right) {
    if ($Trace) { [Console]::Out.WriteLine(("  click {0},{1} right={2} fg={3}" -f [int]$x, [int]$y, [bool]$Right, [U]::Class([U]::GetForegroundWindow()))) }
    [U]::SetCursorPos([int]$x, [int]$y) | Out-Null
    Start-Sleep -Milliseconds 150
    $down = if ($Right) { 8 } else { 2 }
    [U]::mouse_event($down, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [U]::mouse_event($down * 2, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 600
}
function Click($r) { ClickAt (($r[0] + $r[2]) / 2) (($r[1] + $r[3]) / 2) }
# The logo moves when the bar's width changes (apps start and stop): look it up each time.
function ClickLogo { Start-Sleep -Milliseconds 300; Click (ItemRect "#start") }
function Key($vk) { [U]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); [U]::keybd_event($vk, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 120 }
function TypeText($text) { [U]::Type($text); Start-Sleep -Milliseconds 200 }
function Menu { [U]::FindWindow("AtriumSurface", "Atrium Start") }
# A point of the menu in DIP from its top-left (negative: from the right or bottom).
function MenuPoint($dx, $dy) {
    $h = Menu; $r = New-Object U+R; [U]::GetWindowRect($h, [ref]$r) | Out-Null
    $s = [U]::GetDpiForWindow($h) / 96.0
    $x = if ($dx -ge 0) { $r.l + $dx * $s } else { $r.r + $dx * $s }
    $y = if ($dy -ge 0) { $r.t + $dy * $s } else { $r.b + $dy * $s }
    @([int]$x, [int]$y)
}
# Centre (screen px) of a fixed menu button, from the positions the menu logs.
function Spot($id) {
    LastState | Out-Null
    $line = @(NewLog | Where-Object { $_ -match "start: spots" })[-1]
    $m = [regex]::Match($line, "$id@(\d+),(\d+)")
    if (-not $m.Success) { throw "no position for $id" }
    MenuPoint ([int]$m.Groups[1].Value) ([int]$m.Groups[2].Value)
}
function PinCount { $j = Get-Content $file -Raw -Encoding UTF8 | ConvertFrom-Json; @($j.pins).Count }
function TestWindow($title) {
    $code = "Add-Type -AssemblyName System.Windows.Forms; `$f = New-Object Windows.Forms.Form; `$f.Text = '$title'; `$f.Width = 420; `$f.Height = 260; `$f.StartPosition = 'Manual'; `$f.Left = 60; `$f.Top = 60; [Windows.Forms.Application]::Run(`$f)"
    $p = Start-Process "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList "-NoProfile", "-EncodedCommand", ([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))) -PassThru
    $script:children += $p
    for ($i = 0; $i -lt 40; $i++) { $h = [U]::FindWindow([NullString]::Value, $title); if ($h -ne [IntPtr]::Zero) { return $h }; Start-Sleep -Milliseconds 250 }
    throw "test window $title did not appear"
}

try {
    [IO.File]::WriteAllText($file, '{"pins":[{"name":"where","path":"C:\\Windows\\System32\\where.exe"}],"showRunning":true}')
    $env:ATRIUM_POWER_DRYRUN = "1"
    Start-Process $Core -ArgumentList "--debug-layout"
    Start-Sleep 3
    $a = TestWindow "Atrium Start Test"
    # The test window takes the foreground through a real click (away from the menu's area).
    function FocusTestWindow { $wr = New-Object U+R; [U]::GetWindowRect($a, [ref]$wr) | Out-Null; ClickAt (($wr.l + $wr.r) / 2) (($wr.t + $wr.b) / 2) }
    FocusTestWindow

    ClickLogo
    Start-Sleep -Milliseconds 400
    $menu = Menu
    Check "logo-opens-with-keyboard" ([U]::IsWindowVisible($menu) -and [U]::GetForegroundWindow() -eq $menu) "after clicking the logo the menu is visible and in the foreground ($([U]::Class([U]::GetForegroundWindow())))"

    $r = New-Object U+R; [U]::GetWindowRect($menu, [ref]$r) | Out-Null
    $s = [U]::GetDpiForWindow($menu) / 96.0
    $bar = ItemRect "#start"
    Check "size-at-dpi" ([math]::Abs(($r.r - $r.l) - 600 * $s) -le 1 -and ($r.b - $r.t) -le 640 * $s + 1 -and $r.b -le $bar[1]) "menu $($r.r - $r.l)x$($r.b - $r.t) px at $([int]($s * 100))% scale, bottom $($r.b) above the bar top $($bar[1])"

    TypeText "calc"
    $st = LastState
    Check "type-to-search" ($st -match "query='calc' entries=([1-9]\d*)") $st

    Key 0x1B; Key 0x1B
    Start-Sleep -Milliseconds 300
    Check "esc-closes-and-restores" (-not [U]::IsWindowVisible($menu) -and [U]::GetForegroundWindow() -eq $a) "after Esc twice: menu hidden, foreground back to the test window: $([U]::GetForegroundWindow() -eq $a)"

    ClickLogo; Start-Sleep -Milliseconds 300
    ClickLogo; Start-Sleep -Milliseconds 600
    Check "logo-toggles" (-not [U]::IsWindowVisible($menu)) "a second click on the logo closes the menu (and does not reopen it)"

    ClickLogo; Start-Sleep -Milliseconds 300
    FocusTestWindow
    Check "outside-click-closes" (-not [U]::IsWindowVisible($menu) -and [U]::GetForegroundWindow() -eq $a) "clicking the test window closes the menu and keeps the click's window in front"

    # Pin from all apps: the first app of the list, through its context menu.
    $pinsBefore = PinCount
    ClickLogo; Start-Sleep -Milliseconds 300
    $p = Spot "toggle"; ClickAt $p[0] $p[1]              # "all apps"
    $p = MenuPoint 120 134; ClickAt $p[0] $p[1] -Right   # first row
    Key 0x28; Key 0x28; Key 0x0D                          # Down, Down, Enter: "pin to Atrium"
    Start-Sleep 1
    $after = PinCount
    Check "pin-from-all-apps" ($after -eq $pinsBefore + 1) "pins $pinsBefore -> $after"

    # Unpin it again from the pinned grid (second tile).
    $st = LastState
    if (-not [U]::IsWindowVisible($menu)) { ClickLogo; Start-Sleep -Milliseconds 300 }
    if ($st -match "view=all") { $p = Spot "toggle"; ClickAt $p[0] $p[1] }
    $cell = (600 - 40) / 6
    $p = MenuPoint (20 + $cell * 1.5) 150; ClickAt $p[0] $p[1] -Right
    Key 0x28; Key 0x28; Key 0x0D
    Start-Sleep 1
    $final = PinCount
    Check "unpin-from-menu" ($final -eq $pinsBefore) "pins $after -> $final"

    # Power menu, dry run.
    if (-not [U]::IsWindowVisible($menu)) { ClickLogo; Start-Sleep -Milliseconds 300 }
    $p = Spot "power"; ClickAt $p[0] $p[1]
    Key 0x28; Key 0x0D
    Start-Sleep 0.8
    Check "power-sleep-dry-run" ([bool]((NewLog) -match "power: sleep \(dry run\)")) "the power menu's first entry requests sleep (dry run only logs it)"

    # Enter launches the first search result.
    FocusTestWindow
    ClickLogo; Start-Sleep -Milliseconds 300
    TypeText "calc"; Key 0x0D
    Start-Sleep 2.5
    $launched = (NewLog) -match "catalog: launched"
    Check "enter-launches" ([bool]$launched -and -not [U]::IsWindowVisible($menu)) "log: $(@($launched)[-1])"

    # Tray entry: the system's hidden icons.
    FocusTestWindow
    ClickLogo; Start-Sleep -Milliseconds 300
    $p = Spot "tray"; ClickAt $p[0] $p[1]
    Start-Sleep 1
    $fg = [U]::Class([U]::GetForegroundWindow())
    $trayLog = @(NewLog | Where-Object { $_ -match "tray: " })[-1]
    Check "tray-entry" ($fg -eq "TopLevelWindowForOverflowXamlIsland" -or $trayLog -match "left there") "foreground $fg; $trayLog"
    Key 0x1B
}
finally {
    foreach ($p in $children) { if (-not $p.HasExited) { $p.Kill() } }
    Get-Process $started -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | Stop-Process -Force
    Start-Process $Core -ArgumentList "--exit" -Wait
    if ($backup) { [IO.File]::WriteAllBytes($file, $backup) } else { Remove-Item $file -ErrorAction SilentlyContinue }
    [U]::SetCursorPos($cursor.x, $cursor.y) | Out-Null
    if ($userForeground -ne [IntPtr]::Zero) { [U]::SwitchToThisWindow($userForeground, $true) }
}
"failed: $failed"
exit $failed
