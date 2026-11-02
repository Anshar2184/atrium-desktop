<#
.SYNOPSIS
  Builds Atrium and assembles a portable folder: dist/Atrium/.

.DESCRIPTION
  native (CMake, MSVC) -> settings (dotnet publish, self-contained WinUI 3) -> dist.
  Layout:
    dist/Atrium/Atrium.Core.exe
    dist/Atrium/Settings/Fonts/ (MiSans, D40; the core reads it from there)
    dist/Atrium/Settings/Atrium.Settings.exe (+ runtime)
  -Zip also writes dist/Atrium-<version>-win-x64.zip.
#>
param(
    [string]$Configuration = "Release",
    [switch]$Zip
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist/Atrium"

Push-Location $root
try {
    cmake --preset win-x64 -S native
    if ($LASTEXITCODE) { throw "cmake configure failed" }
    cmake --build native/build/win-x64 --config $Configuration
    if ($LASTEXITCODE) { throw "native build failed" }

    if (Test-Path $dist) {
        # A running Atrium (or its settings window) locks files: stop before deleting anything.
        $full = (Resolve-Path $dist).Path
        $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($full, [StringComparison]::OrdinalIgnoreCase) })
        if ($busy) { throw "dist/Atrium is in use by $(($busy | ForEach-Object { "$($_.Name) ($($_.Id))" }) -join ', '); close it first" }
        Remove-Item -Recurse -Force $dist
    }
    New-Item -ItemType Directory -Force $dist | Out-Null
    dotnet publish settings/Atrium.Settings/Atrium.Settings.csproj -c $Configuration -o (Join-Path $dist "Settings")
    if ($LASTEXITCODE) { throw "settings publish failed" }

    Copy-Item (Join-Path "native/build/win-x64/$Configuration" "Atrium.Core.exe") $dist
    Copy-Item LICENSE $dist

    if ($Zip) {
        $version = (Select-String -Path native/CMakeLists.txt -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
        $zipPath = Join-Path $root "dist/Atrium-$version-win-x64.zip"
        if (Test-Path $zipPath) { Remove-Item $zipPath }
        Compress-Archive -Path $dist -DestinationPath $zipPath
        Write-Host "wrote $zipPath"
    }
    Write-Host "dist ready: $dist"
}
finally {
    Pop-Location
}
