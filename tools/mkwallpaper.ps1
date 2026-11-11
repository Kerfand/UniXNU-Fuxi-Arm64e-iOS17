param([int]$MaxWidth = 0)
# fuxi (iPhone15,2, 393w-852h@3x) default wallpaper 7565 iOS 17: both layers' HEIC images -> PNG (Windows WIC, alpha kept), main.caml
# pointed at the PNG files. Output: F:\UniXNU-FUXI-work\ios17-cache\wallpaper-21H16-overlay\{background,floating}.
$ErrorActionPreference = 'Stop'
$wp = 'F:\UniXNU-FUXI-work\ios17-cache\wallpaper-21H16-src\Library\Wallpaper\Collections\iOS_17~iphone.wallpaperCollection\Wallpapers\7565.iOS_17-393w-852h@3x~iphone.wallpaper'
$outRoot = 'F:\UniXNU-FUXI-work\ios17-cache\wallpaper-21H16-overlay'
$conv = 'C:\Users\Xiaomi\AppData\Local\Temp\claude\fa78473d8419f0a5\bb81b53f-24a8-4b58-bdef-56a9b385157b\scratchpad\heic2png.ps1'
if (Test-Path $outRoot) { throw "refuse to overwrite $outRoot" }
foreach ($layer in @(@{ Name = 'background'; Ca = '7565.iOS_17_Background-393w-852h@3x~iphone.ca' }, @{ Name = 'floating'; Ca = '7565.iOS_17_Floating-393w-852h@3x~iphone.ca' })) {
    $src = Join-Path $wp $layer.Ca
    $dst = Join-Path $outRoot $layer.Name
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item (Join-Path $src 'index.xml'), (Join-Path $src 'assetManifest.caml') $dst
    $caml = [IO.File]::ReadAllText((Join-Path $src 'main.caml'))
    foreach ($h in Get-ChildItem -LiteralPath (Join-Path $src 'assets') -Filter *.HEIC) {
        $base = $h.BaseName
        $png = ($base -replace ' ', '-') + '.png'
        $enc = $base -replace ' ', '%20'
        & $conv -Src $h.FullName -Dst (Join-Path $dst $png) -MaxWidth ($(if ($layer.Name -eq 'floating') { 1000 } else { 0 }))
        foreach ($form in @($enc, $base)) {
            $caml = $caml.Replace("`"/assets/$form.HEIC`"", "`"/$png`"").Replace("`"assets/$form.HEIC`"", "`"$png`"")
        }
    }
    if ($caml -match '\.HEIC"') { throw "$($layer.Name): main.caml still references HEIC: $([regex]::Match($caml, '[^"]*\.HEIC"').Value)" }
    [IO.File]::WriteAllText((Join-Path $dst 'main.caml'), $caml)
}
Get-ChildItem -Recurse $outRoot -File | ForEach-Object { '{0,9} {1}' -f $_.Length, $_.FullName.Substring($outRoot.Length + 1) }
'total {0:N0} B' -f (Get-ChildItem -Recurse $outRoot -File | Measure-Object Length -Sum).Sum
