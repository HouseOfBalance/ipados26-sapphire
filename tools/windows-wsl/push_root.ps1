param(
    [string]$AdbPath = "adb.exe",
    [string]$WslDistro = "Ubuntu",
    [string]$WslImgPath = "~/work/out/ipados26-root.img",
    [string]$WslChunkPath = "~/work/out/chunk.bin",
    [int]$TotalGiB = 24,
    [string]$Partition = "iosroot",
    [switch]$AllowUserdataOverwrite
)

$ErrorActionPreference = "Stop"
if ($Partition -notmatch '^[A-Za-z0-9_-]+$') {
    throw "Invalid partition name: $Partition"
}
if ($Partition -eq "userdata" -and -not $AllowUserdataOverwrite) {
    throw "Refusing to overwrite /dev/block/by-name/userdata without -AllowUserdataOverwrite! Default dual-boot target is -Partition iosroot."
}

$wslHome = (wsl -d $WslDistro -- bash -c 'printf "%s" "$HOME"').Trim()
$imgExpanded = $WslImgPath -replace '^~', $wslHome
$chunkExpanded = $WslChunkPath -replace '^~', $wslHome
$winChunk = "\\wsl.localhost\$WslDistro" + ($chunkExpanded -replace '/', '\')

$devPath = "/dev/block/by-name/$Partition"
$exists = (& $AdbPath shell "if [ -b '$devPath' ]; then echo PRESENT; else echo MISSING; fi").Trim()
if ($exists -ne "PRESENT") {
    throw "Target partition $devPath does not exist on device! Check partition layout or pass -Partition userdata -AllowUserdataOverwrite for single-boot."
}

if ($Partition -eq "userdata") {
    Write-Output "Unmounting /sdcard and /data..."
    & $AdbPath shell "umount /sdcard 2>/dev/null; umount /data 2>/dev/null; mount | grep -E 'sda18|userdata' || echo UNMOUNT_OK"
}

for ($i = 0; $i -lt $TotalGiB; $i++) {
    $seek = $i * 1024
    Write-Output "=== Chunk $i/$($TotalGiB - 1) -> $devPath (offset ${seek}M) ==="
    $srcMd5 = (wsl -d $WslDistro -- bash -c "dd if='$imgExpanded' of='$chunkExpanded' bs=1M skip=$seek count=1024 status=none && md5sum '$chunkExpanded' | cut -d' ' -f1").Trim()
    Write-Output "WSL MD5: $srcMd5"
    & $AdbPath push $winChunk /tmp/chunk.bin
    if ($LASTEXITCODE -ne 0) { throw "adb push failed on chunk $i" }
    $dstOut = & $AdbPath shell "dd if=/tmp/chunk.bin of='$devPath' bs=1M seek=$seek conv=notrunc 2>/dev/null && rm -f /tmp/chunk.bin && sync && dd if='$devPath' bs=1M skip=$seek count=1024 2>/dev/null | md5sum"
    $dstMd5 = ($dstOut -split '\s+')[0].Trim()
    Write-Output "DEV MD5: $dstMd5"
    if ($srcMd5 -ne $dstMd5) {
        throw "MD5 mismatch on chunk ${i}: $srcMd5 != $dstMd5"
    }
}
wsl -d $WslDistro -- rm -f "$chunkExpanded"
Write-Output "ALL $TotalGiB CHUNKS VERIFIED OK ON $devPath!"
