# Saves the bottom strip of the primary screen (physical pixels, half size).
param([string]$Out, [int]$Height = 360)
Add-Type -AssemblyName System.Drawing
Add-Type 'using System.Runtime.InteropServices; public static class Dpi { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }'
[Dpi]::SetProcessDPIAware() | Out-Null
$screen = [System.Windows.Forms.Screen]
Add-Type -AssemblyName System.Windows.Forms
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $Height
[System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Left, $b.Bottom - $Height, 0, 0, $bmp.Size)
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($b.Width / 2)), ([int]($Height / 2))
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
