param([string]$Log = 'E:\桌面\FUXI_IOS\builds\fuxi-usb-log-v2\logs\fuxi-20261111-194256-886.txt')
$c = Get-Content $Log
foreach ($name in 'PerfPowerService', 'mediaserverd', 'mobileassetd', 'ScreenTimeAgent', 'com.apple.mobile', 'maild') {
    $i = ($c | Select-String "unixnu crash: pid \d+ $([regex]::Escape($name)) " | Select-Object -First 1).LineNumber
    if (-not $i) { continue }
    "===== $name"
    $j = $i
    while ($j -lt $c.Count -and $j -lt $i + 16 -and ($j -eq $i -or $c[$j - 1] -match '^unixnu crash:  ')) { $c[$j - 1].Substring(0, [Math]::Min(240, $c[$j - 1].Length)); $j++ }
}
