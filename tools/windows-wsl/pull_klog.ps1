param(
    [string]$AdbPath = "adb.exe",
    [string]$Partition = "iosroot",
    [string]$OutFile = "logs\ipados26-xnu.log",
    [string]$AndroidDownloadCopy = "/sdcard/Download/ipados26-xnu.log"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$fullOut = if ([System.IO.Path]::IsPathRooted($OutFile)) { $OutFile } else { Join-Path $repo $OutFile }
New-Item -ItemType Directory -Force (Split-Path -Parent $fullOut) | Out-Null

$devPath = "/dev/block/by-name/$Partition"
$logBlocks = 1025
$sizeStr = (& $AdbPath shell "su -c 'blockdev --getsize64 $devPath'").Trim()
if ($sizeStr -notmatch '^\d+$') {
    throw "Cannot query size of $devPath on device (got: $sizeStr)"
}
$totalBlocks = [int64]$sizeStr / 4096
$skip = $totalBlocks - $logBlocks

$tmpArea = Join-Path $env:TEMP "sweetlog_area.bin"
& $AdbPath shell "su -c 'dd if=$devPath bs=4096 skip=$skip count=$logBlocks 2>/dev/null > /data/local/tmp/sweetlog_area.bin && chmod 644 /data/local/tmp/sweetlog_area.bin'"
& $AdbPath pull /data/local/tmp/sweetlog_area.bin $tmpArea | Out-Null
& $AdbPath shell "rm -f /data/local/tmp/sweetlog_area.bin"

python (Join-Path $repo "tools\ipados26-klog.py") $tmpArea $fullOut
Remove-Item -Force -ErrorAction SilentlyContinue $tmpArea

if ($AndroidDownloadCopy) {
    & $AdbPath push $fullOut /data/local/tmp/ipados26-xnu.log | Out-Null
    & $AdbPath shell "su -c 'cp -f /data/local/tmp/ipados26-xnu.log $AndroidDownloadCopy && chmod 664 $AndroidDownloadCopy && rm -f /data/local/tmp/ipados26-xnu.log'"
    Write-Output "Saved XNU log on Android: $AndroidDownloadCopy"
}
Write-Output "Saved XNU log on PC:      $fullOut"
