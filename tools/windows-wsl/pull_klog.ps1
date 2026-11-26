param(
    [string]$AdbPath = "adb.exe",
    [string]$WslDistro = "Ubuntu",
    [string]$OutTxt = "klog.txt",
    [string]$Partition = "iosroot"
)

$ErrorActionPreference = "Stop"
if ($Partition -notmatch '^[A-Za-z0-9_-]+$') {
    throw "Invalid partition name: $Partition"
}
$devPath = "/dev/block/by-name/$Partition"
$exists = (& $AdbPath shell "if [ -b '$devPath' ]; then echo PRESENT; else echo MISSING; fi").Trim()
if ($exists -ne "PRESENT") {
    throw "Target partition $devPath does not exist on device! Pass -Partition userdata if single-boot."
}
$bytesStr = (& $AdbPath shell "blockdev --getsize64 '$devPath'").Trim()
$bytes = [int64]$bytesStr
$skip = [int64]($bytes / 4096) - 1025
Write-Output "$Partition=$bytes bytes, pulling 1025 4K blocks from skip=$skip..."

& $AdbPath shell "dd if='$devPath' of=/tmp/klog.bin bs=4096 skip=$skip count=1025"
& $AdbPath pull /tmp/klog.bin "$env:TEMP\klog.bin"

$wslTemp = (wsl -d $WslDistro -- wslpath -u "$env:TEMP\klog.bin").Trim()
wsl -d $WslDistro -- python3 -c "
import subprocess, sys
data = open('$wslTemp', 'rb').read()
idx = data.find(b'SWEETLOG')
print('SWEETLOG offset:', idx)
if idx < 0:
    sys.exit(1)
open('/tmp/klog_slice.bin', 'wb').write(data[idx:])
out = subprocess.check_output(['python3', '/home/' + __import__('os').environ['USER'] + '/ios-sapphire/tools/ipados26-klog.py', '/tmp/klog_slice.bin'], text=True, errors='replace')
print('Lines:', len(out.splitlines()))
open('/tmp/klog_decoded.txt', 'w').write(out)
"
Copy-Item "\\wsl.localhost\$WslDistro\tmp\klog_decoded.txt" $OutTxt -Force
Write-Output "Saved decoded log to $OutTxt"
