<#
.SYNOPSIS
  End-to-end check of Atrium.Settings -> pipe -> Atrium.Core, driven through
  UI Automation like a user would: live preview, save, revert on close.

.DESCRIPTION
  Starts Atrium.Core (with --debug-capturable when -Shots is given, so the
  glass shows up in screenshots) and Atrium.Settings from the build output,
  changes the preset and two sliders, saves, makes an unsaved change, closes
  without saving and checks that the core reverted it. The user's settings
  file is backed up and put back afterwards.
#>
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Shots = "",
    # Test the packaged layout from tools/build.ps1 (dist/Atrium) instead of the build output.
    [switch]$Package
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$A = [Windows.Automation.AutomationElement]
if ($Package) {
    $core = Join-Path $Root "dist/Atrium/Atrium.Core.exe"
    $settingsExe = Join-Path $Root "dist/Atrium/Settings/Atrium.Settings.exe"
} else {
    $core = Join-Path $Root "native/build/win-x64/Release/Atrium.Core.exe"
    $settingsExe = (Get-ChildItem (Join-Path $Root "settings/Atrium.Settings/bin/Release") -Recurse -Filter Atrium.Settings.exe | Select-Object -First 1).FullName
}
$file = Join-Path $env:LOCALAPPDATA "Atrium/settings.json"
$logFile = Join-Path $env:LOCALAPPDATA "Atrium/logs/core.log"
$backup = if (Test-Path $file) { [IO.File]::ReadAllBytes($file) } else { $null }
$failed = 0

function Check($name, $ok, $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail
}
function Shot($name) {
    if ($Shots) { & (Join-Path $PSScriptRoot "shot-bottom.ps1") -Out (Join-Path $Shots "$name.png") | Out-Null }
}
function NewLog { if (Test-Path $logFile) { (Get-Content $logFile -Encoding UTF8 | Select-Object -Skip $script:logStart) -join "`n" } else { "" } }
function Find($root, $name, $type) {
    $c = New-Object Windows.Automation.AndCondition(
        (New-Object Windows.Automation.PropertyCondition($A::NameProperty, $name)),
        (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, $type)))
    for ($i = 0; $i -lt 40; $i++) {
        $e = $root.FindFirst([Windows.Automation.TreeScope]::Descendants, $c)
        if ($e) { return $e }
        Start-Sleep -Milliseconds 250
    }
    throw "UI element not found: $name"
}
function SetSlider($win, $name, $value) {
    (Find $win $name ([Windows.Automation.ControlType]::Slider)).GetCurrentPattern([Windows.Automation.RangeValuePattern]::Pattern).SetValue($value)
}
function Invoke($win, $name) {
    (Find $win $name ([Windows.Automation.ControlType]::Button)).GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
}
function StartSettings {
    $p = Start-Process $settingsExe -PassThru
    for ($i = 0; $i -lt 60 -and $p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 250; $p.Refresh() }
    return $p
}

$script:logStart = if (Test-Path $logFile) { @(Get-Content $logFile -Encoding UTF8).Count } else { 0 }
try {
    Remove-Item $file -ErrorAction SilentlyContinue
    $coreArgs = if ($Shots) { @("--debug-capturable") } else { @() }
    if ($coreArgs) { Start-Process $core -ArgumentList $coreArgs } else { Start-Process $core }
    Start-Sleep 2
    Shot "e2e-0-default"

    $p = StartSettings
    $win = $A::FromHandle($p.MainWindowHandle)
    $status = Find $win "Atrium 正在运行" ([Windows.Automation.ControlType]::Text)
    Check "connected" ($null -ne $status) "settings window shows the core as running"

    # Preset -> frosted, then shape changes; all of this is a live preview.
    $combo = $win.FindFirst([Windows.Automation.TreeScope]::Descendants, (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::ComboBox)))
    $combo.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    (Find $win "磨砂玻璃" ([Windows.Automation.ControlType]::ListItem)).GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select()
    $combo.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse()
    SetSlider $win "圆角" 12
    SetSlider $win "高度" 64
    Start-Sleep 1.5
    Shot "e2e-1-preview-frosted"
    Check "preview-not-saved" (-not (Test-Path $file)) "a preview does not write the settings file"

    Invoke $win "保存"
    Start-Sleep 1
    $saved = if (Test-Path $file) { Get-Content $file -Raw | ConvertFrom-Json } else { $null }
    Check "commit-written" ($saved -and $saved.preset -eq "frosted" -and $saved.cornerRadius -eq 12 -and $saved.height -eq 64 -and [math]::Abs($saved.transparency - 0.78) -lt 1e-6) `
        "settings.json after save: preset=$($saved.preset) cornerRadius=$($saved.cornerRadius) height=$($saved.height) transparency=$($saved.transparency)"

    # Unsaved change, then close without saving: the core must revert it.
    SetSlider $win "透明度" 30
    Start-Sleep 1.5
    Shot "e2e-2-unsaved-preview"
    $win.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close()
    Invoke $win "不保存"
    $exited = $p.WaitForExit(10000)
    Start-Sleep 1
    Check "settings-closed" $exited "the window closed after choosing not to save"
    Check "preview-reverted" ((NewLog) -match "client left with an unsaved preview, reverted") "core log reports the revert"
    $after = Get-Content $file -Raw | ConvertFrom-Json
    Check "file-unchanged" ([math]::Abs($after.transparency - 0.78) -lt 1e-6) "settings.json transparency still $($after.transparency)"
    Shot "e2e-3-reverted"

    # A new settings window shows the saved values from the core.
    $p = StartSettings
    $win = $A::FromHandle($p.MainWindowHandle)
    $t = (Find $win "透明度" ([Windows.Automation.ControlType]::Slider)).GetCurrentPattern([Windows.Automation.RangeValuePattern]::Pattern).Current.Value
    Check "reopen-shows-saved" ([math]::Abs($t - 78) -lt 0.5) "transparency slider shows $t"
    $win.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close()
    Check "clean-close" ($p.WaitForExit(10000)) "closing without changes needs no confirmation"
    Check "commit-logged" ((NewLog) -match "pipe: commit saved") "core log reports the save"
}
finally {
    Get-Process Atrium.Settings -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Process $core -ArgumentList "--exit" -Wait
    if ($backup) { [IO.File]::WriteAllBytes($file, $backup) } else { Remove-Item $file -ErrorAction SilentlyContinue }
}
"failed: $failed"
exit $failed
