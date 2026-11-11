param(
    [Parameter(Mandatory)][string]$Image,
    [string]$Fastboot = 'C:\platform-tools\fastboot.exe',
    [int]$CaptureSeconds = 300,
    [switch]$SkipRebootFirst,
    [switch]$NoRebootAfter
)
# Unattended test cycle (no one at the phone): serial REBOOT-FASTBOOT -> fastboot boot $Image (nothing flashed) ->
# capture the USB log for $CaptureSeconds -> serial REBOOT-FASTBOOT again so the next cycle can start.
$ErrorActionPreference = 'Stop'
$tool = 'E:\桌面\FUXI_IOS\builds\fuxi-usb-log-v2\Capture-FuxiUsbControl.ps1'
$logs = 'E:\桌面\FUXI_IOS\builds\fuxi-usb-log-v2\logs'
function Wait-Fastboot([int]$seconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $l = @(& $Fastboot devices 2>$null | Where-Object { $_ -match '\S+\s+fastboot' })
        if ($l.Count -eq 1) { return ($l[0] -split '\s+')[0] }
        Start-Sleep -Seconds 1
    }
    return $null
}
if (-not (Test-Path -LiteralPath $Image)) { throw "missing image $Image" }
Write-Host ("image  {0}`nsha256 {1}" -f $Image, (Get-FileHash -LiteralPath $Image -Algorithm SHA256).Hash.ToLowerInvariant())
$serial = Wait-Fastboot 3
if (-not $serial -and -not $SkipRebootFirst) {
    Write-Host 'phone not in fastboot: sending REBOOT-FASTBOOT over the kernel control port'
    try { & $tool -OutputDirectory $logs -WaitSeconds 60 -CaptureSeconds 30 -Send fastboot } catch { Write-Host "port closed: $($_.Exception.Message)" }
    $serial = Wait-Fastboot 120
}
if (-not $serial) { throw 'no fastboot device (kernel control port gone? the phone needs a manual power + volume-down)' }
Write-Host "fastboot device $serial"
& $Fastboot -s $serial boot $Image
if ($LASTEXITCODE -ne 0) { throw "fastboot boot failed ($LASTEXITCODE)" }
Write-Host "booted; capturing $CaptureSeconds s"
try { & $tool -OutputDirectory $logs -WaitSeconds 300 -CaptureSeconds $CaptureSeconds } catch { Write-Host "capture ended: $($_.Exception.Message)" }
$last = Get-ChildItem $logs -Filter 'fuxi-*.txt' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Host "LOG $($last.FullName)"
if (-not $NoRebootAfter) {
    try { & $tool -OutputDirectory $logs -WaitSeconds 30 -CaptureSeconds 20 -Send fastboot } catch { Write-Host "port closed: $($_.Exception.Message)" }
    if (Wait-Fastboot 120) { Write-Host 'back in fastboot' }
    else {
        # The phone can land in recovery (adb) instead; adb reboot bootloader only reboots, nothing is written.
        $adb = Join-Path (Split-Path $Fastboot) 'adb.exe'
        if (@(& $adb devices 2>$null | Where-Object { $_ -match '\srecovery$|\sdevice$' }).Count -eq 1) {
            Write-Host 'phone in adb/recovery: adb reboot bootloader'
            & $adb reboot bootloader 2>&1 | Out-Null
        }
        if (Wait-Fastboot 120) { Write-Host 'back in fastboot' } else { Write-Host 'WARNING: phone did not return to fastboot' }
    }
}

