# Hướng Dẫn Thiết Lập Windows + WSL2 Ubuntu Cho `ipados26-sapphire` (iPadOS 26.1)

Tài liệu này hướng dẫn chuẩn bị môi trường **Windows 10/11 + WSL2 Ubuntu**, sao lưu phân vùng bảo vệ máy, trích xuất firmware GPU/IPA từ điện thoại và vận hành các công cụ nạp/debug cho **iPadOS 26.1 (`23B85`)** trên **Xiaomi Redmi Note 13 4G (`sapphire`)**.

---

## 1. Cài Đặt Môi Trường WSL2 Ubuntu

```bash
sudo apt-get update
sudo apt-get install -y \
  clang lld llvm gcc-aarch64-linux-gnu \
  meson ninja-build cmake bison flex zstd \
  build-essential git python3 python3-pip \
  hfsprogs apfs-fuse libplist-dev libssl-dev openssh-client openssl unzip curl wget
```
Cài thêm `ipsw` (của `blacktop/ipsw`) để giải mã luồng `.dmg.aea` của iPadOS 26.1 và chuẩn bị module `hfsplus.ko` khớp `uname -r` của kernel WSL2 tại `~/modules/hfsplus.ko`.

---

## 2. Sao Lưu Phân Vùng Nhạy Cảm & Trích Xuất Firmware GPU/IPA

1. **Sao lưu phân vùng EFS & Boot gốc (trong OrangeFox Recovery, giữ riêng trên PC, không đưa vào git):**
   ```powershell
   mkdir backup
   foreach ($p in @("persist","modemst1","modemst2","fsg","fsc","boot_a","dtbo_a","vbmeta_a")) {
       adb shell "dd if=/dev/block/by-name/$p of=/tmp/$p.img"
       adb pull "/tmp/$p.img" ".\backup\$p.img"
       adb shell "rm -f /tmp/$p.img"
   }
   ```
2. **Trích xuất `a630_sqe.fw`, `a610_zap.*` và `ipa_fws.*` từ `/vendor/firmware` của máy** vào `~/work/gpu-fw/` để truyền cho `./tools/ipados26-boot.sh --gpu-fw ~/work/gpu-fw --ipa-fw ~/work/gpu-fw`.

---

## 3. Nạp Ảnh 24 GiB & Đọc Log Kernel (`SWEETLOG`)

- **Đẩy `ipados26-root.img` an toàn (chia 24 chunk x 1 GiB + kiểm tra MD5 từng chunk):**
  ```powershell
  powershell -ExecutionPolicy Bypass -File .\tools\windows-wsl\push_root.ps1 `
    -AdbPath ".\platform-tools\adb.exe" `
    -WslImgPath "~/work/out-ipados26/ipados26-root.img" `
    -Partition "iosroot" `
    -TotalGiB 24
  ```
- **Kéo log kernel (`SWEETLOG`) từ cuối phân vùng `iosroot` khi cần chẩn đoán lỗi boot:**
  ```powershell
  powershell -ExecutionPolicy Bypass -File .\tools\windows-wsl\pull_klog.ps1 `
    -AdbPath ".\platform-tools\adb.exe" `
    -Partition "iosroot" `
    -OutTxt .\ipados26-klog.txt
  ```
  *(Giải mã bằng `tools/ipados26-klog.py`).*
