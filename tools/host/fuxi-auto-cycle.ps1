param(
    [Parameter(Mandatory)][string]$Image,
    [string]$Fastboot = 'C:\platform-tools\fastboot.exe',
    [int]$CaptureSeconds = 300,
    # How long to wait for fastboot after a kernel panic: the kernel resets to the bootloader 30 s after its panic drain
    # (fuxi-panic-fastboot=30), plus the reset and the bootloader's own USB bring-up.
    [int]$PanicFastbootSeconds = 150,
    [switch]$SkipRebootFirst,
    [switch]$NoRebootAfter
)
# Unattended test cycle (no one at the phone): get to fastboot -> fastboot boot $Image (nothing flashed) -> capture the USB CDC
# log for $CaptureSeconds -> back to fastboot so the next cycle can start.
# CDC state check (2026-11-12): the capture's own record says how it ended. panic_complete (QCOM PANIC END seen) or a log with
# panic( means a kernel panic: the control port is useless then, so nothing is sent and the cycle waits for the kernel's own
# panic -> fastboot reset. device_gone (CDC vanished without a reboot command) is treated the same. A phone that lands in
# recovery is sent to the bootloader with adb reboot bootloader (it only reboots; nothing is written).
$ErrorActionPreference = 'Stop'
$tool = 'E:\桌面\FUXI_IOS\builds\fuxi-usb-log-v2\Capture-FuxiUsbControl.ps1'
$logs = 'E:\桌面\FUXI_IOS\builds\fuxi-usb-log-v2\logs'
$adb = Join-Path (Split-Path $Fastboot) 'adb.exe'
function Wait-Fastboot([int]$seconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $l = @(& $Fastboot devices 2>$null | Where-Object { $_ -match '\S+\s+fastboot' })
        if ($l.Count -eq 1) { return ($l[0] -split '\s+')[0] }
        Start-Sleep -Seconds 1
    }
    return $null
}
function Send-AdbBootloader {
    if (-not (Test-Path -LiteralPath $adb)) { return $false }
    $l = @(& $adb devices 2>$null | Where-Object { $_ -match '\srecovery$|\sdevice$|\ssideload$' })
    if ($l.Count -ne 1) { return $false }
    Write-Host "phone in adb ($(($l[0] -split '\s+')[1])): adb reboot bootloader"
    & $adb reboot bootloader 2>&1 | Out-Null
    return $true
}
function Get-Fastboot([int]$seconds) {
    # Fastboot first; else adb recovery -> bootloader; else wait out the rest.
    $s = Wait-Fastboot $seconds
    if ($s) { return $s }
    if (Send-AdbBootloader) { return (Wait-Fastboot 120) }
    return $null
}
function Get-CaptureState($since) {
    # The newest capture record written after $since: its status, and whether the text log shows a panic.
    $rec = Get-ChildItem $logs -Filter 'fuxi-*.json' | Where-Object { $_.LastWriteTime -ge $since } |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $rec) { return [pscustomobject]@{ Status = 'no_capture'; Panic = $false; Text = $null; Lines = @() } }
    $r = Get-Content -LiteralPath $rec.FullName -Raw | ConvertFrom-Json
    $text = [IO.Path]::ChangeExtension($rec.FullName, '.txt')
    $lines = @()
    if (Test-Path -LiteralPath $text) {
        $lines = @(Select-String -LiteralPath $text -Pattern 'panic\(cpu|panic\(|QCOM USB PANIC|QCOM PANIC END|Debugger message|Panicked task' |
                   Select-Object -First 12 | ForEach-Object { $_.Line.Trim() })
    }
    $panic = $r.panic_end_seen -or $r.status -eq 'panic_complete' -or @($lines | Where-Object { $_ -match 'panic\(' }).Count -gt 0
    return [pscustomobject]@{ Status = $r.status; Panic = [bool]$panic; Text = $text; Lines = $lines; Bytes = $r.bytes }
}

if (-not (Test-Path -LiteralPath $Image)) { throw "missing image $Image" }
Write-Host ("image  {0}`nsha256 {1}" -f $Image, (Get-FileHash -LiteralPath $Image -Algorithm SHA256).Hash.ToLowerInvariant())
$serial = Wait-Fastboot 3
if (-not $serial -and (Send-AdbBootloader)) { $serial = Wait-Fastboot 120 }
if (-not $serial -and -not $SkipRebootFirst) {
    Write-Host 'phone not in fastboot: sending REBOOT-FASTBOOT over the kernel control port'
    try { & $tool -OutputDirectory $logs -WaitSeconds 60 -CaptureSeconds 30 -Send fastboot } catch { Write-Host "port closed: $($_.Exception.Message)" }
    $serial = Get-Fastboot 120
}
if (-not $serial) { throw 'no fastboot device (no CDC control port, no adb; the phone needs a manual power + volume-down)' }
Write-Host "fastboot device $serial"
& $Fastboot -s $serial boot $Image
if ($LASTEXITCODE -ne 0) { throw "fastboot boot failed ($LASTEXITCODE)" }
Write-Host "booted; capturing $CaptureSeconds s"
$since = Get-Date
try { & $tool -OutputDirectory $logs -WaitSeconds 300 -CaptureSeconds $CaptureSeconds } catch { Write-Host "capture ended: $($_.Exception.Message)" }
$state = Get-CaptureState $since
$last = Get-ChildItem $logs -Filter 'fuxi-*.txt' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Host "LOG $($last.FullName)"
Write-Host ("CDC state: {0}; panic={1}; bytes={2}" -f $state.Status, $state.Panic, $state.Bytes)
foreach ($l in $state.Lines) { Write-Host "  | $l" }
if ($NoRebootAfter) { return }
if ($state.Panic -or $state.Status -eq 'device_gone') {
    # Kernel panic (or CDC gone): the kernel resets to the bootloader on its own; do not talk to a dead control port.
    Write-Host ("{0}: waiting up to {1} s for the kernel's panic -> fastboot reset" -f $(if ($state.Panic) { 'KERNEL PANIC' } else { 'CDC gone' }), $PanicFastbootSeconds)
    if (Get-Fastboot $PanicFastbootSeconds) { Write-Host 'back in fastboot' }
    else { Write-Host 'WARNING: phone did not return to fastboot after the panic (reset windows not mapped? fuxi-panic-fastboot=30 missing?)' }
    return
}
try { & $tool -OutputDirectory $logs -WaitSeconds 30 -CaptureSeconds 20 -Send fastboot } catch { Write-Host "port closed: $($_.Exception.Message)" }
if (Get-Fastboot 120) { Write-Host 'back in fastboot' } else { Write-Host 'WARNING: phone did not return to fastboot' }
