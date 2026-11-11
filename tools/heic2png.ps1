param([string]$Src, [string]$Dst, [int]$MaxWidth = 0)
# HEIC -> PNG with Windows WIC (Microsoft HEIF decoder): colour from the primary item, alpha from a copy whose primary item is the
# alpha auxiliary image (heicalpha.py). Optional MaxWidth scales images wider than it by one third (layers stretch to their bounds).
Add-Type -AssemblyName PresentationCore, WindowsBase
$ErrorActionPreference = 'Stop'
function Decode([string]$path, $format) {
    $s = [IO.File]::OpenRead($path)
    try {
        $dec = [Windows.Media.Imaging.BitmapDecoder]::Create($s, [Windows.Media.Imaging.BitmapCreateOptions]::PreservePixelFormat, [Windows.Media.Imaging.BitmapCacheOption]::OnLoad)
        $conv = New-Object Windows.Media.Imaging.FormatConvertedBitmap($dec.Frames[0], $format, $null, 0)
        $w = $conv.PixelWidth; $h = $conv.PixelHeight
        $bpp = [int][Math]::Floor(($format.BitsPerPixel + 7) / 8)
        $px = New-Object byte[] ($w * $h * $bpp)
        $conv.CopyPixels($px, $w * $bpp, 0)
        return @{ W = $w; H = $h; Px = $px }
    } finally { $s.Close() }
}
$color = Decode $Src ([Windows.Media.PixelFormats]::Bgra32)
$tmp = [IO.Path]::GetTempFileName() + '.heic'
$alphaItem = & 'C:\Program Files\Python314\python.exe' -I 'C:\Users\Xiaomi\AppData\Local\Temp\claude\fa78473d8419f0a5\bb81b53f-24a8-4b58-bdef-56a9b385157b\scratchpad\heicalpha.py' $Src $tmp
$hasAlpha = $alphaItem -and $alphaItem -ne 'none'
if ($hasAlpha) {
    $alpha = Decode $tmp ([Windows.Media.PixelFormats]::Gray8)
    Remove-Item $tmp -ErrorAction SilentlyContinue
    if ($alpha.W -ne $color.W -or $alpha.H -ne $color.H) { throw "alpha $($alpha.W)x$($alpha.H) differs from colour $($color.W)x$($color.H)" }
    if (-not ('AlphaMerge' -as [type])) {
        Add-Type -TypeDefinition 'public static class AlphaMerge { public static void Run(byte[] p, byte[] a) { for (int i = 0; i < a.Length; i++) p[4 * i + 3] = a[i]; } }'
    }
    [AlphaMerge]::Run($color.Px, $alpha.Px)
}
$bmp = [Windows.Media.Imaging.BitmapSource]::Create($color.W, $color.H, 72, 72, [Windows.Media.PixelFormats]::Bgra32, $null, $color.Px, $color.W * 4)
if ($MaxWidth -gt 0 -and $color.W -gt $MaxWidth) {
    $third = [double](1.0 / 3)
    $scale = New-Object Windows.Media.ScaleTransform -ArgumentList $third, $third
    $bmp = New-Object Windows.Media.Imaging.TransformedBitmap -ArgumentList $bmp, $scale
}
if (-not $hasAlpha) { $bmp = New-Object Windows.Media.Imaging.FormatConvertedBitmap($bmp, [Windows.Media.PixelFormats]::Bgr24, $null, 0) }
$enc = New-Object Windows.Media.Imaging.PngBitmapEncoder
$enc.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bmp))
$out = [IO.File]::Create($Dst)
try { $enc.Save($out) } finally { $out.Close() }
'{0} -> {1}x{2} alpha={3} {4} B' -f (Split-Path $Src -Leaf), $bmp.PixelWidth, $bmp.PixelHeight, $hasAlpha, (Get-Item $Dst).Length
