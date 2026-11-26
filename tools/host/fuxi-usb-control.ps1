# Starts the fuxi USB capture + control tool; it waits for VID 1209 / PID 316D and opens the port when it appears.
$tool = 'E:\桌面\FUXI_IOS\builds\fuxi-usb-log-v2\Capture-FuxiUsbControl.ps1'
& $tool -OutputDirectory 'E:\桌面\FUXI_IOS\builds\fuxi-usb-log-v2\logs' -WaitSeconds 3600 @args