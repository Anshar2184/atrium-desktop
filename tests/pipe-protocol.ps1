<#
.SYNOPSIS
  Robustness of the Atrium.Core settings pipe (docs/SETTINGS-PROTOCOL.md):
  malformed requests, unknown ops, out-of-range and mistyped values, and the
  revert of an unsaved preview when the client disconnects.
#>
param([string]$Core = (Join-Path (Split-Path -Parent $PSScriptRoot) "native/build/win-x64/Release/Atrium.Core.exe"))
$ErrorActionPreference = "Stop"
$file = Join-Path $env:LOCALAPPDATA "Atrium/settings.json"
$backup = if (Test-Path $file) { [IO.File]::ReadAllBytes($file) } else { $null }
$failed = 0
function Check($name, $ok, $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail
}
function Connect {
    $p = New-Object System.IO.Pipes.NamedPipeClientStream(".", "Atrium.Core.$((Get-Process -Id $PID).SessionId)", [System.IO.Pipes.PipeDirection]::InOut)
    $p.Connect(3000)
    $p.ReadMode = "Message"
    $p
}
function Req($p, $s) {
    $b = [Text.Encoding]::UTF8.GetBytes($s)
    $p.Write($b, 0, $b.Length); $p.Flush()
    $buf = New-Object byte[] 65536
    $ms = New-Object IO.MemoryStream
    do { $n = $p.Read($buf, 0, $buf.Length); $ms.Write($buf, 0, $n) } while (-not $p.IsMessageComplete)
    [Text.Encoding]::UTF8.GetString($ms.ToArray()) | ConvertFrom-Json
}
try {
    Remove-Item $file -ErrorAction SilentlyContinue
    Start-Process $Core
    Start-Sleep 2
    $p = Connect
    $r = Req $p "not json"
    Check "malformed" ($r.op -eq "error") $r.message
    $r = Req $p '{"op":"explode"}'
    Check "unknown-op" ($r.op -eq "error") $r.message
    $r = Req $p '{"op":"preview","settings":5}'
    Check "settings-not-object" ($r.op -eq "error") $r.message
    $r = Req $p '{"op":"preview","settings":{"transparency":7,"height":-3,"tint":"#zzzzzz","width":"wide"}}'
    $s = Req $p '{"op":"get"}'
    Check "clamped" ($r.op -eq "ok" -and $s.current.transparency -eq 1 -and $s.current.height -eq 32) "transparency=$($s.current.transparency) height=$($s.current.height)"
    Check "invalid-ignored" ($s.current.tint -eq "#FFFFFF" -and $s.current.width -eq 720) "tint=$($s.current.tint) width=$($s.current.width)"
    Check "saved-untouched" ($s.saved.transparency -eq 0.9 -and -not (Test-Path $file)) "saved transparency=$($s.saved.transparency)"
    $p.Dispose()
    Start-Sleep 1
    $p = Connect
    $s = Req $p '{"op":"get"}'
    Check "revert-on-disconnect" ($s.current.transparency -eq 0.9 -and $s.current.height -eq 56) "current after reconnect: transparency=$($s.current.transparency) height=$($s.current.height)"
    $p.Dispose()
}
finally {
    Start-Process $Core -ArgumentList "--exit" -Wait
    if ($backup) { [IO.File]::WriteAllBytes($file, $backup) } else { Remove-Item $file -ErrorAction SilentlyContinue }
}
"failed: $failed"
exit $failed
