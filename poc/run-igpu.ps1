# Runs a probe with the per-app "power saving GPU" preference set for the probe
# executable only, and always removes that preference afterwards.
param([Parameter(Mandatory)][string]$Exe, [Parameter(ValueFromRemainingArguments)][string[]]$ProbeArgs)
$k = 'HKCU:\Software\Microsoft\DirectX\UserGpuPreferences'
if (-not (Test-Path $k)) { New-Item $k -Force | Out-Null }
$had = (Get-ItemProperty $k).PSObject.Properties.Name -contains $Exe
$old = if ($had) { (Get-ItemProperty $k).$Exe } else { $null }
New-ItemProperty -Path $k -Name $Exe -Value 'GpuPreference=1;' -PropertyType String -Force | Out-Null
try { & $Exe @ProbeArgs }
finally {
    if ($had) { Set-ItemProperty -Path $k -Name $Exe -Value $old } else { Remove-ItemProperty -Path $k -Name $Exe -ErrorAction SilentlyContinue }
    "gpu-preference restored: " + ((Get-ItemProperty $k).PSObject.Properties.Name -contains $Exe -eq $had)
}
