# Launched only after the user chooses to restart. Runs outside the install folder.
param(
    [Parameter(Mandatory)][string]$InstallDirectory,
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$StatePath,
    [Parameter(Mandatory)][int]$WaitForProcessId,
    [ValidateSet('0', '1')][string]$StartCore = '1'
)
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$package = [IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$cache = Split-Path -Parent ([IO.Path]::GetFullPath($StatePath))
$parent = Split-Path -Parent $install
$name = Split-Path -Leaf $install
$suffix = [Guid]::NewGuid().ToString('N')
$incoming = Join-Path $parent "$name.update-$suffix"
$backup = Join-Path $parent "$name.previous-$suffix"
$result = Join-Path $cache 'result.json'
$coreStopped = $false
$swapped = $false

try {
    if (!$parent -or !$name -or !$package.StartsWith($cache + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw '更新目录无效。'
    }
    foreach ($root in @($install, $package)) {
        if (!(Test-Path -LiteralPath (Join-Path $root 'Atrium.exe')) -or
            !(Test-Path -LiteralPath (Join-Path $root 'Settings\Atrium.Settings.exe'))) {
            throw '找不到完整的 Atrium 程序文件。'
        }
    }
    # Copy before stopping the app. A disk/access failure leaves the old app running.
    New-Item -ItemType Directory -Path $incoming | Out-Null
    Get-ChildItem -LiteralPath $package -Force | Copy-Item -Destination $incoming -Recurse -Force
    $settingsProcess = Get-Process -Id $WaitForProcessId -ErrorAction SilentlyContinue
    if ($settingsProcess -and !$settingsProcess.WaitForExit(30000)) { throw '设置中心尚未退出，更新未安装。' }
    $quit = Start-Process -FilePath (Join-Path $install 'Atrium.exe') -ArgumentList '--exit' -WindowStyle Hidden -PassThru
    if (!$quit.WaitForExit(15000) -or $quit.ExitCode -ne 0) { throw 'Atrium 尚未正常退出，更新未安装。' }
    $coreStopped = $true
    # Rename whole folders on the same volume, so a partial copy is never installed.
    Move-Item -LiteralPath $install -Destination $backup
    try { Move-Item -LiteralPath $incoming -Destination $install }
    catch {
        Move-Item -LiteralPath $backup -Destination $install
        throw
    }
    $swapped = $true
    Remove-Item -LiteralPath $StatePath -Force
    @{ error = '' } | ConvertTo-Json | Set-Content -LiteralPath $result -Encoding UTF8
} catch {
    @{ error = $_.Exception.Message } | ConvertTo-Json | Set-Content -LiteralPath $result -Encoding UTF8
} finally {
    # These are only the sibling directories created above, never a computed install root.
    foreach ($temporary in @($incoming, $(if ($swapped) { $backup }))) {
        if ($temporary -and (Split-Path -Parent $temporary) -eq $parent -and
            ((Split-Path -Leaf $temporary) -eq "$name.update-$suffix" -or (Split-Path -Leaf $temporary) -eq "$name.previous-$suffix")) {
            if (Test-Path -LiteralPath $temporary) {
                try { Remove-Item -LiteralPath $temporary -Recurse -Force } catch { }
            }
        }
    }
    if ($coreStopped -and $StartCore -eq '1') {
        Start-Process -FilePath (Join-Path $install 'Atrium.exe') -WorkingDirectory $install -WindowStyle Hidden
    }
    Start-Process -FilePath (Join-Path $install 'Settings\Atrium.Settings.exe') -ArgumentList '--page about' -WorkingDirectory $install
    if ($swapped) {
        $work = Split-Path -Parent $package
        if ((Split-Path -Parent $work) -eq $cache -and (Split-Path -Leaf $work) -match '^package-[0-9a-f]{32}$') {
            try { Remove-Item -LiteralPath $work -Recurse -Force } catch { }
        }
    }
}
