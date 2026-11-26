# Hướng Dẫn Toàn Diện Build & Boot iPadOS 26.1 (`iPad11,6` / `23B85`) Trên Xiaomi Redmi Note 13 4G (`sapphire`)

Tài liệu này hướng dẫn quy trình khép kín từ A–Z để biên dịch Kernel + Userland, đóng gói RootFS (kèm sẵn **Procursus + Sileo + ElleKit + OpenSSH `root`/`1234`**) và khởi động **iPadOS 26.1** (`iPad_10.2_2020_26.1_23B85_Restore.ipsw` — định danh `iPad11,6` Wi-Fi hoặc `iPad11,7` Cellular, chip A12 `arm64e`) trên **Xiaomi Redmi Note 13 4G (`sapphire` — Snapdragon 685 / SM6225Pro)**.

> **Tra cứu thông số phần cứng & ánh xạ mã nguồn Kernel/Boot:** Xem chi tiết tại [IPADOS26_XNU_SPECS.md](../IPADOS26_XNU_SPECS.md).

---

## 1. Quy Trình Thực Hiện Chi Tiết

### Bước 1: Build Kernel `xnu-ipados26` & Userland `userland-ipados26` (GitHub Actions / Codemagic)
1. Kích hoạt 2 workflow trên **GitHub Actions** (hoặc `xnu-ipados26` / `xnu-ipados26-4k` trên **Codemagic**):
   - **`xnu-ipados26`** (`.github/workflows/xnu-ipados26.yml`) -> tải artifact `ipados26-xnu-kernel` vào `xnu/out-ipados26`
   - **`userland-ipados26`** (`.github/workflows/userland-ipados26.yml`) -> tải artifact `ipados26-userland-binaries` vào `userland/obj-ipados26`
   ```bash
   gh run download <XNU_RUN_ID> -n ipados26-xnu-kernel -D xnu/out-ipados26
   gh run download <UL_RUN_ID> -n ipados26-userland-binaries -D userland/obj-ipados26
   ```

### Bước 2: Tải IPSW iPadOS 26.1 (`23B85`) Trong Linux / WSL2
```bash
mkdir -p ~/work/ipados26 ~/work/out-ipados26

curl -L -C - -o ~/work/ipados26/iPad_10.2_2020_26.1_23B85_Restore.ipsw \
  "https://updates.cdn-apple.com/2025FallFCS/fullrestores/089-13625/7EB04911-5F5A-41C5-82F5-408C3D06585C/iPad_10.2_2020_26.1_23B85_Restore.ipsw"
```

### Bước 3: Tạo RootFS `ipados26-root.img` (24 GiB, Có Sẵn SSH + Jailbreak + HakiosMetal Đã De-PAC)
```bash
sudo bash ./tools/ipados26-root.sh \
  --with-jb \
  --root-pass 1234 \
  --userland-dir ./userland/obj-ipados26 \
  ~/work/ipados26/iPad_10.2_2020_26.1_23B85_Restore.ipsw \
  ~/work/out-ipados26 \
  ~/modules/hfsplus.ko
```
*(Nếu chỉ muốn cập nhật riêng Procursus/SSH/Userland lên ảnh `ipados26-root.img` đã tạo sẵn: `sudo bash ./tools/ipados26-jb-install.sh ~/work/out-ipados26/ipados26-root.img --userland-dir ./userland/obj-ipados26 --ko ~/modules/hfsplus.ko`).*

### Bước 4: Đóng Gói File Boot `ipados26-boot.img`
- **Chế độ an toàn CPU Renderer + Console Log (cho lần boot kiểm tra đầu tiên):**
  ```bash
  ./tools/ipados26-boot.sh --console \
    --gpu-fw ~/work/gpu-fw --ipa-fw ~/work/gpu-fw \
    ./xnu/out-ipados26/kernel.development.sweet \
    ~/work/out-ipados26/ipados26-boot.img
  ```
- **Chế độ GPU Adreno 610 phần cứng đầy đủ (`HakiosMetal.bundle` `sweetmetal=2` + 120Hz + Tweaks):**
  ```bash
  ./tools/ipados26-boot.sh --gpu \
    --gpu-fw ~/work/gpu-fw --ipa-fw ~/work/gpu-fw \
    ./xnu/out-ipados26/kernel.development.sweet \
    ~/work/out-ipados26/ipados26-boot-gpu.img
  ```
  *(Thêm `--amp` nếu dùng kernel 4K 8 nhân `xnu-ipados26-4k`, `--ipad-canvas` cho khung hình `1620x2160`, hoặc `--cellular` cho `iPad11,7`).*

### Bước 5: Nạp Vào Phân Vùng `iosroot` & Boot Từ RAM
1. Vào **OrangeFox Recovery**, đẩy ảnh 24 GiB vào `/dev/block/by-name/iosroot`:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tools\windows-wsl\push_root.ps1 `
     -AdbPath ".\platform-tools\adb.exe" `
     -WslImgPath "~/work/out-ipados26/ipados26-root.img" `
     -Partition "iosroot" `
     -TotalGiB 24
   ```
2. Vào **Fastboot** và boot từ RAM (**TUYỆT ĐỐI KHÔNG CHẠY `fastboot flash boot`**):
   ```powershell
   .\platform-tools\fastboot.exe boot .\ipados26-boot-gpu.img
   ```
