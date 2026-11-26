# Cẩm Nang Kiến Trúc & Bàn Giao Đánh Giá Dự Án `ipados26-sapphire` (iPadOS 26.1 trên Redmi Note 13 4G)

Tài liệu này tổng hợp toàn bộ kiến trúc kỹ thuật, quy trình biên dịch/đóng gói và các quy tắc bất biến của dự án port **Apple iPadOS 26.1 (`23B85` — `iPad11,6` Wi-Fi / `iPad11,7` Cellular, chip A12 `arm64e`)** lên **Xiaomi Redmi Note 13 4G (`sapphire` — Qualcomm Snapdragon 685 / SM6225Pro `"khaje"`, GPU Adreno 610 v2 `0x06010001`)**.

---

## 1. Bản Đồ Kiến Trúc Cho AI Agent Đánh Giá / Tiếp Quản

1. **Kernel XNU 26.1 (`xnu-12377.41.6`) — `xnu/patches-ipados26/`**:
   - `00_setup_coreentitlements.sh`, `01-stage-libpthread-ipados26.sh` (`libpthread-539`), `02-stage-bearssl.sh`, `03-stage-hfs-ipados26.sh` (`hfs-704.40.4.0.1` hỗ trợ mount RW trên dirty journal & chặn spam hardlink).
   - `zz-0100-ipados26-xnu-12377-bringup.patch`: Toàn bộ hạ tầng phần cứng SM6225Pro (GICv3 Group 1 IRQ, SMP Gold/Silver, SMMU-500, UFS, DWC3 USB, QUP0 SE2 Touch, SPMI PM6125/PM7250B, PAS TrustZone Modem/WLAN/GPU, QSEE Fingerprint, Camera TFE/CSID, AMFI/Sandbox/LaunchConstraints bypass cho OS 26, `dyld` shared cache slide v3/v5).
   - `zz-0101-ipados26-audit-fixes.patch`: Bản vá 120 lỗi bảo mật, ổn định, race condition và kiểm tra biên (early FB `0x5c000000`, TBI masking trên LSE/LDRAA, signed `LDAPRS*`, 16 KiB `MSM_UPAGE` GPU alignment, `thread_call_cancel_wait` UAF fix).
   - `zz-0102-ipados26-gpu-display-opt.patch`: Tối ưu `SapphireGpu.cpp` (persistent 1 GiB WC `gpool_kva` loại bỏ 100% `pmap`/TLB shootdown mỗi lần cấp phát BO, `MSM_BO_NOZERO`, stack submit arrays, `fLeftUsed` $O(1)$ + gom `tlb_flush()`) và `SweetFramebuffer.cpp` (bỏ 19.4M `dc civac`/s trên nhánh native 120Hz, thêm bộ ghép kênh alpha 64-bit SWAR an toàn với `-mgeneral-regs-only`).
   - `zz-0103-ipados26-cpufreq-tsens-ufs-touch.patch`: Driver DVFS động Qualcomm OSM (`0x0f521000`/`0x0f523000`) + cảm biến nhiệt silicon `TSENS0` (`0x04410000`/`0x04411000`) đồng bộ 100% với `thermald`, bật mặc định `ufscache=1`/`ufswait=1` trong `sweet_ufs.c`, và đọc nhanh I2C 21-byte + touch boost trong `sweet_touch.c`.
   - `zz-0104-ipados26-armv82-fp16-rdm-emu.patch`: Bộ giả lập số nguyên thuần `sweet_emulate_fp16_rdm()` (`__int128` làm tròn đơn chuẩn IEEE-754 RNE) cho `FEAT_FP16` vô hướng, `ASIMDHP` vector `.4H`/`.8H`, và `FEAT_RDM` (`SQRDMLAH`/`SQRDMLSH`) trong `osfmk/arm64/sleh.c`.
   - `zz-0105-ipados26-armv82-dotprod-fcma-fhm-emu.patch`: Bộ giả lập số nguyên thuần `sweet_emulate_dotprod_fcma_fhm()` (an toàn tuyệt đối với `-mgeneral-regs-only`) cho `FEAT_DotProd` (`SDOT`/`UDOT`), `FEAT_FCMA` (`FCADD`/`FCMLA` `.4H/.8H`/`.2S/.4S`/`.2D`), `FEAT_FHM` (`FMLAL`/`FMLSL`/`FMLAL2`/`FMLSL2`), `FEAT_FlagM` (`CFINV`/`XAFLAG`/`AXFLAG`) và sửa chuẩn hóa `ASIMDHP` `.4H/.8H`.

2. **Tương Thích `arm64e` Offline & Đóng Gói iPadOS 26.1 — `tools/` & `tests/`**:
   - `ipados26_depac.py`: Chuyển đổi tĩnh các lệnh `arm64e` (`RETAA/B`, `BLRAA/B*`, `BRAA/B*`, `PAC*`, `AUT*`, `XPACI`, TBI-safe `XPACD`, `XPACLRI`, `LDAPR*` -> `LDAR*`, `DC CVAP/CVADP` -> `DC CVAC`, `SB` -> `ISB`, `LDRAA/B`) trong `dyld_shared_cache_arm64e*` và Mach-O sang ARMv8.0-A với tốc độ gốc (0 trap), tự động băm lại `CS_CodeDirectory` (kể cả `SC_Scatter` & alternate CodeDirectories) kèm thống kê `--audit`.
   - `tests/run_unit_tests.py`: Bộ kiểm thử tự động kiểm tra toàn diện De-PAC, `CS_CodeDirectory`, DeviceTree `mkadt`, Metallib v2/v3, DSC slide v3/v5 và tính toàn vẹn chuỗi patch XNU `zz-0100..zz-0105`.
   - `ipados26_im4p_aea.py`: Giải nén IM4P (`lzfse`/`lzvn`/`pbzx`/`pbze`) và mở khóa khóa giải mã HPKE AEA của IPSW `23B85`.
   - `ipados26_dsc_offsets.py` & `ipados26_ca_metallib.py`: Trích xuất tự động offset vtable `QuartzCore` và dựng `/redwood/share/ca-share/QuartzCore-default.metallib` từ `default.pipelinelib`.
   - `ipados26-root.sh`, `ipados26-root-tweaks.py`, `ipados26-jb-install.sh`, `ipados26-boot.sh`, `ipados26_mkadt.py`: Pipeline tự động hóa từ IPSW `23B85` -> `ipados26-root.img` (24 GiB) & `ipados26-boot.img`.

3. **Đồ Họa Metal Adreno 610 & Userland `arm64e` — `hakios-metal/`, `metal/`, `userland/`**:
   - `HakiosMetal.bundle` (`sweetmetal=2`): Driver Metal phần cứng trực tiếp trên `SapphireGPU` (kèm Fence-Aware BO Recycle Pool và persistent shader cache `/private/var/db/hakios-metal-cache`).
   - Cài đặt `.ipa` gốc trực tiếp qua `/redwood/bin/sapphire-ipa-helper` + `Tienda.app` (Universal iPad `UIDeviceFamily = [1, 2]`).

---

## 2. Năm Nguyên Tắc Bất Biến Khi Vận Hành

1. **Bảo vệ tuyệt đối Android gốc**: Slot `a` giữ nguyên Android HyperOS & OrangeFox Recovery; iPadOS 26.1 nằm ở phân vùng riêng `iosroot` (`ufsroot=iosroot`).
2. **Không bao giờ chạy `fastboot flash boot`**: Luôn boot từ RAM bằng `fastboot boot ipados26-boot.img`.
3. **Chuẩn biên dịch Userland**: Mọi binary/dylib/tweak cho iPadOS 26.1 phải biên dịch với `-target arm64e-apple-ios16.0`, ký `ldid -S` / `codesign`, và chạy qua `python3 tools/ipados26_depac.py --rewrite`.
4. **Không truyền nhị phân qua stdin của `adb.exe shell` trên Windows**: Dùng `tools/windows-wsl/push_root.ps1` (chia chunk 1 GiB + kiểm tra MD5 từng chunk).
5. **Kết nối Debug sau khi boot**: SSH `root@192.168.7.2` (cổng `22` / `2222`, mật khẩu `1234`), emergency shell `nc 192.168.7.2 2323`, live log `nc -ukl 5140`, hoặc kéo `SWEETLOG` qua `tools/windows-wsl/pull_klog.ps1` / `tools/ipados26-klog.py`.
