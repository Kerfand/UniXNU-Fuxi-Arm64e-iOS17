param(
    [string]$Image = 'F:\UniXNU-FUXI-work\outputs\sadnut-fuxi-ios17-ufs-root-20261106\boot.img',
    [string]$Fastboot = 'C:\platform-tools\fastboot.exe',
    [int]$WaitSeconds = 900
)
# Wait for a fastboot device, temporarily boot $Image (fastboot boot: nothing is flashed), then start the
# USB capture + control tool, which waits for the kernel's CDC port.
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Image)) { throw "missing image $Image" }
$hash = (Get-FileHash -LiteralPath $Image -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "image  $Image"
Write-Host "sha256 $hash"
Write-Host 'Waiting for a fastboot device (hold power + volume down on the phone) ...'
$deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
$serial = $null
while ([DateTime]::UtcNow -lt $deadline) {
    $lines = @(& $Fastboot devices 2>$null | Where-Object { $_ -match '\S+\s+fastboot' })
    if ($lines.Count -eq 1) { $serial = ($lines[0] -split '\s+')[0]; break }
    if ($lines.Count -gt 1) { throw 'more than one fastboot device; unplug the others' }
    Start-Sleep -Seconds 1
}
if (-not $serial) { throw 'no fastboot device appeared' }
Write-Host "fastboot device $serial"
& $Fastboot -s $serial boot $Image
if ($LASTEXITCODE -ne 0) { throw "fastboot boot failed ($LASTEXITCODE)" }
Write-Host 'Booted. Starting the USB capture + control tool (keys: F=REBOOT-FASTBOOT S=STATUS H=HELLO Q=stop).'
& (Join-Path $PSScriptRoot 'fuxi-usb-control.ps1')
