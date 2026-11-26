# Bảng Thông Số Phần Cứng & Ánh Xạ Mã Nguồn Boot XNU / iPadOS 26.1 (`IPADOS26_XNU_SPECS.md`)

Tài liệu tham chiếu gốc cho **Xiaomi Redmi Note 13 4G (`sapphire` / `sapphiren`, Qualcomm Snapdragon 685 — SM6225Pro `"khaje"`)** khi biên dịch và khởi động **XNU 26.1 (`xnu-12377.41.6`) + iPadOS 26.1 (`23B85`, `iPad11,6` / `iPad11,7`)**.

Mọi địa chỉ vật lý (PA), kích thước vùng nhớ, mã ngắt GICv3 (`SPI` / `INTID`), chân `GPIO`, mã `PAS` TrustZone, `SMMU SID`, thuộc tính Apple DeviceTree (`ADT`), `boot-args` và `sysctl` dưới đây đều được trích xuất và đối chiếu trực tiếp từ thiết bị thật + mã nguồn `shim/`, `uefi/`, `tools/ipados26_mkadt.py`, và `xnu/patches-ipados26/`.

---

## 1. Định Danh SoC, CPU & Tập Lệnh (Đã Xác Thực Trực Tiếp Trên Máy)

| Hạng mục | Giá trị phần cứng thực tế (`sapphire`) | Ý nghĩa / Ràng buộc khi boot XNU 26.1 | File & Macro trong code |
| --- | --- | --- | --- |
| **SoC** | Qualcomm **SM6225Pro / Snapdragon 685** (`khaje`) | `/sys/devices/soc0/soc_id = 518` (`0x206`) | `README.md`, `tools/ipados26_mkadt.py` |
| **Cụm CPU Silver (Cores 0–3)** | 4× **Kryo 260 Silver** (Cortex-A53 class)<br>`implementer=0x51, variant=0xa, part=0x801, rev=4`<br>`MPIDR`: `0x0`, `0x1`, `0x2`, `0x3` | **Chỉ có trang 4K/64K, KHÔNG có 16K granule!**<br>- Kernel 16K (`xnu-ipados26`): Shim đánh thức Gold `0x100` rồi park Silver.<br>- Kernel 4K (`xnu-ipados26-4k` + `--amp`): Chạy đủ 8 nhân (`cpus=8`). | `shim/shim.c`, `tools/ipados26_mkadt.py` (`--amp`), `iokit/Kernel/arm/SweetCPU.cpp` |
| **Cụm CPU Gold (Cores 4–7)** | 4× **Kryo 260 Gold** (ARM Cortex-A73)<br>`implementer=0x41, variant=0x1, part=0xd09, rev=0`<br>`MPIDR`: `0x100`, `0x101`, `0x102`, `0x103` | **Hỗ trợ đủ 4K / 16K / 64K translation granule** (`ID_AA64MMFR0_EL1.TGran16 = 1`). Cụm chính gánh XNU 16K (`cpus=4`). | `shim/shim.c`, `osfmk/arm64/start.s`, `tools/ipados26_mkadt.py` |
| **Tập lệnh ISA (ARMv8.0-A)** | `fp asimd evtstrm aes pmull sha1 sha2 crc32 cpuid` | **Thiếu** ARMv8.1 (`LSE`), v8.2 (`FP16`), v8.3 (`PAC`, `LRCPC`, `FJCVTZS`) mà binary `arm64e` của iPadOS 26.1 sử dụng.<br>$\rightarrow$ Xử lý bằng **De-PAC offline** + **trap emulator trong EL1**. | `tools/ipados26_depac.py`, `osfmk/arm64/sleh.c` (`sweet_emulate_armv83_pac`, `sweet_emulate_lse`), `machine_routines_asm.s` |
| **Xung nhịp Timer gốc** | `19,200,000 Hz` (`19.2 MHz` XO) | Tần số `CNTPCT_EL0` / `CNTVCT_EL0`; Virtual Timer PPI `INTID = 19` (`PPI 3`). | `TIMEBASE = 19200000`, `VTIMER_INTID = 19` (`tools/ipados26_mkadt.py:28-30`) |
| **CPUFreq Domains (OSM)** | Silver: `0x0F521000`<br>Gold: `0x0F523000` | Shim đẩy xung nhịp cả 2 cụm lên tối đa trước khi nhảy vào XNU; driver runtime điều chỉnh qua `kern.sweet_cpufreq`. | `shim/shim.c:251-293`, `bsd/dev/sweet_cpufreq.c` (`CPUFREQ_SILVER_PA`, `CPUFREQ_GOLD_PA`) |

---

## 2. Bản Đồ Bộ Nhớ Vật Lý (DDR Banks, Carveouts & Pools Của XNU)

Trên bản RAM 6 GB LPDDR4X (Linux báo ~`5.5 GiB` sau carveout) và 8 GB LPDDR4X, ABL khai báo 3 dải DDR chính (`0x40000000`+`0x3CC00000`, `0x80000000`+`1 GiB`, `0xC0000000`+`4..6 GiB`). **Tuyệt đối không cấp phát đè lên các vùng TrustZone/PIL/Splash dưới đây:**

| Địa chỉ bắt đầu (PA) | Kích thước | Tên vùng / Mục đích | Thuộc tính cache / Bảo vệ | File & Hằng số ánh xạ |
| --- | --- | --- | --- | --- |
| `0x45700000` | `0x00600000` (6 MiB) | `HYP` (Qualcomm Hypervisor EL2) | Reserved (Cấm ghi từ EL1) | `MemoryMapLib.c` |
| `0x45E00000` | `0x00140000` (1.25 MiB) | `AOP` / RPM Always-On Processor | Reserved Uncached | `MemoryMapLib.c` |
| `0x45FFF000` | `0x00001000` (4 KiB) | `SEC APPS` | Reserved Uncached | `MemoryMapLib.c` |
| `0x46000000` | `0x00200000` (2 MiB) | **`SMEM`** (Qualcomm Shared Memory: GLINK/SMP2P/QRTR/IPA) | Write-Combine / Uncached | `TopazModem.cpp` (`SMEM_PA = 0x46000000`, `SMEM_SIZE = 0x200000`) |
| `0x46200000` | `0x01E00000` (30 MiB) | `CDSP Secure` | Reserved Uncached | `MemoryMapLib.c` |
| `0x4AB00000` | `0x06900000` (105 MiB) | **Modem PIL Carveout** (`PAS 4`, firmware từ `modem_a`) | Reserved / PIL | `README.md:234`, `TopazModem.cpp` (`PAS_ID_MODEM = 4`) |
| `0x51900000` | `0x00100000` (1 MiB) | **WLAN MSA Region** (WCN3988/WCN3990 firmware memory) | Reserved / QMI WLFW | `TopazModem.cpp` (`WLFW_MSA_BASE = 0x51900000`, `WLFW_MSA_SIZE = 0x100000`) |
| `0x53800000` | `0x02200000` (34 MiB) | **ADSP PIL Carveout** (`PAS 1`, AudioReach SPF + Sensors SEE) | Reserved / PIL | `TopazModem.cpp` (`ADSP_CARVEOUT = 0x53800000`, `ADSP_CARVEOUT_SIZE = 0x2200000`) |
| `0x55A00000` | `0x00010000` (64 KiB) | **IPA Firmware Region** (`PAS 15`, `ipa_fws.mdt`/`.elf`) | Reserved / PIL | `TopazModem.cpp` (`IPA_FW_PA = 0x55A00000`, `IPA_PAS_ID = 15`) |
| `0x55A15000` | `0x00002000` (8 KiB) | **GPU Zap Shader Region** (`PAS 13`, `a610_zap.mdt`/`.b02`) | Reserved / PIL | `SapphireGpu.cpp` (`ZAP_PA = 0x55A15000`, `GPU_PAS_ID = 13`) |
| `0x5C000000` | `0x00F00000` (15 MiB) | **`cont_splash_region` Framebuffer** (`1080×2400×4`, stride `4320`) | Write-Combine / Framebuffer | `shim/console.h` (`FB_BASE`), `ipados26_mkadt.py` (`FB = 0x5C000000`), `start.s`, `sweet_early.c` |
| `0xA0000000` | `0x04000000` (64 MiB) | `XNU Payload` (khi boot qua UEFI) | Write-Back | `MemoryMapLib.c`, `uefi/mkuefi-v4.py` |
| `0xB0000000` | `0x4D000000` (~1.2 GiB) | XNU Low-RAM Mode (mặc định khi **không** truyền `--highmem`) | XNU Managed RAM | `ipados26_mkadt.py` (`PHYS_BASE = 0xB0000000`) |
| `0xD7000000` | `0x09000000` (144 MiB) | **Camera ISP DMA Pool** (frame buffers RAW10/YUV cho `sweet_cam`) | Uncached / DMA | `bsd/dev/sweet_cam.c` (`CAM_POOL_PA = 0xD7000000`, `CAM_POOL_SIZE = 0x09000000`) |
| `0xE0000000` | `0x01000000` (16 MiB) | **Modem / WLAN / RMTFS DMA Pool** (`rmtfs` 3 MB, CE rings, WMI) | Uncached / SMMU S1 | `TopazModem.cpp` (`MODEM_POOL_BASE = 0xE0000000`, `MODEM_POOL_SIZE = 0x01000000`) |
| `0xE1000000` | `0x00500000` (5 MiB) | **GPU Ring, SQE, Kernel Bounce & SMMU Page Tables** (`POOL_PA` + `PT2_PA`) | Write-Combine / Uncached | `SapphireGpu.cpp` (`POOL_PA = 0xE1000000`, `PT2_PA = 0xE1100000`, `PT2_SIZE = 0x400000`) |
| `0xE2000000` | `0x03000000` (48 MiB) | **Display Flip Pool (`SweetLowBuf`)** — 3× backbuffers cho MDP5 | Write-Combine | `SweetFramebuffer.cpp` (`SWEET_FB_LOW_PA = 0xE2000000`, `SWEET_FB_LOW_SIZE = 0x03000000`) |
| `0xE8000000` | `0x01000000` (16 MiB) | **QSEE TrustZone Shared Pool** (Goodix FOD TA 12 MB + cmd 2 MB + listeners) | Uncached / SMC | `SweetQsee.cpp` (`QSEE_POOL_PA = 0xE8000000`, `QSEE_POOL_SIZE = 0x01000000`) |
| `0x1_00000000` | `0x40000000` (1 GiB) | **GPU User Buffer Pool (`GPOOL_PA`)** cho `hakios-metal` & Mesa Freedreno | Write-Combine (`16 KiB` aligned) | `SapphireGpu.cpp` (`GPOOL_PA = 0x100000000`, `GPOOL_SIZE = 0x40000000`) |
| `0x1_40000000` | Tới đỉnh DDR (2–4 GiB) | **XNU Main RAM (`--highmem`, mặc định của `ipados26-boot.sh`)** | Write-Back (`shim/loader.c` tự cắt khớp dung lượng RAM 6GB/8GB từ ABL DTB!) | `ipados26_mkadt.py` (`HIGH_PHYS_BASE = 0x140000000`), `shim/loader.c` |

---

## 3. Bản Đồ Thanh Ghi MMIO, Ngắt GICv3 (`SPI`/`INTID`), `SMMU SID` & Chân `GPIO`

> **Lưu ý cực kỳ quan trọng về `/arm-io` trong Apple DeviceTree (`ipados26_mkadt.py`):**
> Bus `/arm-io` khai báo `SOC_BASE = 0x01000000` (`ranges = <0x0, 0x01000000, 0xFF000000>`). Mọi node con bên dưới `/arm-io` phải ghi thuộc tính `reg = <PA - SOC_BASE, SIZE>`! Ngoài ra, trên GICv3: **`INTID = SPI + 32`**.

| Khối phần cứng | Địa chỉ vật lý (PA) & Size | IRQ / `INTID` / `SMMU SID` / `GPIO` | Chi tiết giao thức & Thanh ghi quan trọng | File nguồn XNU / Shim |
| --- | --- | --- | --- | --- |
| **GICv3 Interrupt Controller** | `GICD`: `0x0F200000` (`0x10000`)<br>`GICR`: `0x0F300000` (`0x100000`, stride `0x20000`/CPU) | VTimer `INTID 19` | GIC-500. Shim mở `IGROUPR0 = 0xFFFFFFFF` (Group 1 NS) trên mọi core trước khi vào XNU. | `shim/shim.c:21-22`, `ipados26_mkadt.py:23-24` |
| **PS_HOLD & Reboot Cookie** | `PSHOLD`: `0x0440B000`<br>`IMEM`: `0x0C125000` (`+0x65C` restart reason) | Cookie Fastboot: `0x77665500`<br>Cookie Recovery: `0x77665502`<br>PSCI Reset: `0x84000009` | Hạ `PSHOLD = 0` hoặc gọi `PSCI_SYSTEM_RESET` / `RESET2` để tắt máy / vào Recovery / Bootloader. | `shim/shim.c:17-19`, `shim/splash.c:36-38`, `bsd/dev/sweet_reboot.c` |
| **TLMM GPIO Tiles** | `WEST`: `0x00500000` (`0x300000`)<br>`SOUTH`: `0x00900000` (`0x300000`)<br>`EAST`: `0x00D00000` (`0x300000`) | Stride mỗi chân GPIO: `0x1000 * pin`<br>`CFG = +0x0`, `IN_OUT = +0x4` | Lưu ý cờ `TLMM_EGPIO_ENABLE` (`bit 12`) trên các chân có thể thuộc đảo LPASS (như FOD `GPIO 83, 104`). | `sweet_touch.c`, `SweetQsee.cpp:314-316`, `MemoryMapLib.c` |
| **Clock Controllers & RPMh** | `GCC`: `0x01400000` (`0x200000`)<br>`GPUCC`: `0x05990000` (`0x9000`)<br>`DISPCC`: `0x05F00000` (`0x20000`)<br>`RPMh RSC`: `0x18220000` (`TCS +0xD00`), `CMD-DB`: `0x0C3F000C` | Magic CMD-DB: `0x0C0330DB` | Bật GDSC & clock cho USB (`+0x1A004`), GPU, QUPv3, Camera, UFS; RPMh điều khiển nguồn PMIC. | `bsd/dev/sweet_rpmh.c`, `SapphireGpu.cpp` |
| **SPMI / PMIC (Phím cứng, Pin, Rung, RTC)** | `CORE`: `0x01C40000`<br>`CNFG`: `0x01C0A000`<br>`CHNLS`: `0x01E00000`<br>`OBS`: `0x03E00000` | Volume Up: `GPIO 5` / PMIC PON<br>Power Key: PMIC PON<br>Vibrator: `SID = 3` (`0x5340`/`0x5341`) | Quản lý phím nguồn/âm lượng (`sweet_keys.c`), pin & sạc (`SweetBattery.cpp`), mô-tơ rung (`sweet_vibrate.c`), thời gian thực (`rtcd`). | `bsd/dev/sweet_spmi.c`, `bsd/dev/sweet_keys.c`, `bsd/dev/sweet_vibrate.c`, `SweetBattery.cpp` |
| **UFS Host Storage** | `UFSHCI`: `0x04804000` (`0x3000`) | LUN0: `userdata` / `iosroot` (4K block)<br>LUN4: `boot_a` / `boot_b` (chuyển slot Dual-Boot) | Cuối phân vùng gốc lưu vòng log đen `SWEETLOG` (đọc khi panic bằng `pull_klog.ps1`) và hộp thư Android `SPHIPAQ1` (`kern.sweet_inbox`). | `bsd/dev/sweet_ufs.c`, `shim/ufs.c` |
| **Display MDSS / MDP5 / DSI0** | `MDSS`: `0x05E00000` (`0x200000`)<br>Panel: `1080×2400` @ `120 Hz` AMOLED (DSC) | Backlight: MIPI DCS `0x51` (max `500 nits`)<br>Splash magic: `"SPL2"` (`0x324C5053`) | Hỗ trợ 1:1 native (`sweetfb_native=1`), GPU copy bất đồng bộ (`sweetfb_gpucopy=1`), triple flip buffer ở `0xE2000000`. | `iokit/Kernel/arm/SweetFramebuffer.cpp`, `SweetSurface.cpp`, `video_console.c` |
| **Cảm ứng FocalTech FT3519T (`54:52`)** | `QUP0 SE2` (I2C/SPI): `0x04A88000` | Địa chỉ I2C: `0x38`<br>`GPIO 80` (IRQ), `GPIO 86` (Reset)<br>Raw range: `8640 × 9600` (`2160×2400 << 2`) | Tốc độ quét 4 ms khi chạm / 8 ms nghỉ; hỗ trợ chạm đúp sáng màn (`kern.sweet_tap_wake`) và vùng cảm biến vân tay FOD (`kern.sweet_fod_press`). | `bsd/dev/sweet_touch.c` |
| **GPU Adreno 610 v2** | `GPU MMIO`: `0x05900000` (`0x40000`)<br>`GPU SMMU-500`: `0x059A0000` (`0x10000`)<br>`GMEM`: `128 KiB` tại `0x00100000` | **`kChipId = 0x06010001`**<br>GIC `SPI 177` $\rightarrow$ **`INTID 209`**<br>SMMU `SID = 0`, `mask = 1`<br>TZ Zap `PAS = 13` | Không có GMU (dùng GMU wrapper).<br>- GPU VA `0x01000000..0x10000000`: `'SGPU'` (`SapphireMetal`)<br>- GPU VA `0x20000000`+: Kernel frame-copy windows<br>- GPU VA `0x60000000..0xE0000000`: `'MSM'` (`HakiosMetal` & Mesa Freedreno) | `iokit/Kernel/arm/SapphireGpu.cpp`, `hakios-metal/include/hakios-metal/a6xx.hpp` |
| **USB DWC3 (Mạng RNDIS/CDC-ECM)** | `DWC3`: `0x04E00000` (`0xD000`)<br>`QSCRATCH`: `0x04EF8800`<br>`HS PHY`: `0x01613000`<br>`GDSC`: `0x0141A004` | IP máy: `192.168.7.2` (PC: `192.168.7.1`)<br>Log UDP: cổng `5140`<br>SSH: cổng `22` / `2222`<br>Root shell cứu hộ: cổng `2323` | Bật bằng `sweetusb=2`. Cực kỳ hữu ích để SSH và xem kernel log trực tiếp khi chưa cấu hình Wi-Fi. | `bsd/dev/sweet_usb.c`, `tools/ipados26-jb-install.sh` |
| **Apps SMMU-500 (Chung SoC)** | `SMMU`: `0x0C600000` (`0x80000`) | WLAN `SID = 0x1A0` (`mask 1`)<br>IPA `SID = 0x140`<br>Camera TFE `SID = 0x400` | Quản lý ánh xạ DMA cho Wi-Fi SNOC, IPA cellular data, ADSP (`IOVA 0x10000000`) và Camera TFE. | `TopazModem.cpp`, `bsd/dev/sweet_cam.c` |
| **Modem, Wi-Fi (WCN3988) & Audio (ADSP)** | `TCSR Mutex`: `0x00340000`<br>`APCS IPC`: `0x0F111000`<br>`IPA`: `0x05840000`, `GSI`: `0x05804000`<br>`LPASS`: `RXM 0x0A600000`, `TXM 0x0A620000`, `VAM 0x0A730000`, `LPI 0x0A7C0000`, Codec Reset `GPIO 92` | Modem `PAS = 4`<br>ADSP `PAS = 1`<br>IPA `PAS = 15`<br>WCN3988 `chip_id = 150` | - Wi-Fi tạo card `en0` chuẩn Apple80211 (`wifi=1`).<br>- Audio xuất `/dev/redwood-audio` (cho `RedwoodAudio.driver`) & `/dev/redwood-gpr` (cho `arplay`).<br>- Cảm biến SEE qua `/dev/redwood-qmi*` (`SVC 0x190`, `sensord`). | `iokit/Kernel/arm/TopazModem.cpp`, `bsd/dev/sapphire_wlan.c`, `bsd/dev/sapphire_apple80211.c` |
| **Bluetooth (WCN3990 UART)** | `QUP0 SE3`: `0x04A8C000`<br>`QUP0 Wrap`: `0x04AC0000` | Thiết bị `/dev/cu.bluetooth` @ `3 Mbps` H4 | `btshim` nối `/dev/cu.bluetooth` sang `/var/run/btshim/hci` cho `bluetoothd` + `btbridge.dylib`. | `bsd/dev/sweet_bt.c`, `userland/bluetooth/btshim.c` |
| **Camera (CSIPHY / CSID / TFE / CCI)** | `CPAS`: `0x05C11000`<br>`CCI`: `0x05C1B000`<br>`CSID0`: `0x05C6E000` (`TFE0 +0x1000`)<br>`PPI`: `0x05CB3000` | `GPIO_VIO = 54`, `GPIO_CSI_MUX = 34`<br>`GPIO_MAIN_VANA = 26`, `VDIG = 85`, `VAF = 101`<br>AF GT9764 I2C `0x0C` | Cảm biến chính Samsung **S5KHM6** (108MP) + trước **OV16A1Q** (16MP). `camd` xử lý RAW $\rightarrow$ BGRX qua `/var/tmp/camd-preview.bin`. | `bsd/dev/sweet_cam.c`, `userland/camera/camd/` |
| **Vân Tay Dưới Màn Hình (Goodix FOD)** | Qua TrustZone SMC (`SweetQsee`) + SPI | `GPIO_PWR = 65`<br>`GPIO_RST = 104`<br>`GPIO_IRQ = 83` | Nạp Goodix TA vào `QSEE_POOL_PA` (`0xE8000000`); daemon `fpd` + `Huella.app` quản lý đăng ký & mở khóa. | `iokit/Kernel/arm/SweetQsee.cpp`, `userland/fingerprint/fpd.c` |

---

## 4. Thông Số Định Danh iPadOS 26.1 (`23B85`) & Giải Mã IPSW

| Thông số | Giá trị chuẩn trong dự án | File sử dụng |
| --- | --- | --- |
| **IPSW gốc** | `iPad_10.2_2020_26.1_23B85_Restore.ipsw` (iPad Gen 8 10.2", chip A12 `arm64e`) | `tools/ipados26-root.sh` |
| **Model / Board / Chip giả lập trong ADT** | Wi-Fi (mặc định): **`iPad11,6`** / `J171aAP` / `J171a`<br>Cellular (`--cellular`): **`iPad11,7`** / `J172aAP` / `J172a`<br>`chip-id = 0x8020` (Apple T8020 / A12), `SWEET_ECID = 0x5357454554323631` (`"SWEET261"`) | `tools/ipados26_mkadt.py`, `research/ipsw-ipados26/j171aap.adt` |
| **AEA Key — OS DMG (`043-54294-118.dmg.aea`)** | `base64:NxXyEsTtpTiVIW4bj5haBWUrcLAdjWbDbDzSjommmNk=` | `tools/ipados26-root.sh:40` |
| **AEA Key — SystemOS (`043-54733-126.dmg.aea`)** | `base64:IQSeCzXPyPvu4Iyf8V6VdR98z2oKTgdDxqLED+f74f0=` | `tools/ipados26-root.sh:42` |
| **AppOS DMG (`043-54062-129.dmg`)** | Raw APFS không mã hóa | `tools/ipados26-root.sh:43` |
| **CoreAnimation Shader Fallback** | `/redwood/share/ca-share/QuartzCore-default.metallib` (trích tự động từ `default.pipelinelib/functions` của iPadOS 26.1) | `tools/ipados26_ca_metallib.py`, `userland/src/HakiosMetalCA.m` |
| **Shared Cache Offsets** | `/redwood/etc/sapphire-offsets.conf` (quét tự động từ `dyld_shared_cache_arm64e*` + `.symbols` offset `0x48`) | `tools/ipados26_dsc_offsets.py`, `userland/src/eagl.c` |

---

## 5. Tra Cứu Nhanh Các `boot-args` & `sysctl` Quan Trọng Nhất Khi Sửa Code / Debug

### A. Các `boot-args` cốt lõi (`tools/ipados26-boot.sh`)
- **Hệ thống & RootFS:**
  - `rd=ufsroot0 ufsroot=iosroot ufsrw=1 rootrw=1`: Mount phân vùng `iosroot` (hoặc `userdata` nếu truyền `--ufsroot userdata`) ở chế độ đọc/ghi.
  - `ignition_level=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 amfi_get_out_of_my_way=1 cs_enforcement_disable=1`: Cho phép `launchd` của iPadOS 26.1 khởi động từ HFS+ với binary đã De-PAC và nạp dylib/tweak không vướng AMFI / Launch Constraints.
  - `cpus=4` (kernel 16K chỉ chạy 4 nhân Gold) hoặc `cpus=8` (`--amp` với kernel 4K chạy đủ 8 nhân).
  - `wdt=-1 ufslogsecs=1`: Tắt watchdog cứng và ghi log `SWEETLOG` xuống cuối phân vùng mỗi 1 giây.
- **Hiển thị & GPU Adreno 610:**
  - **Chế độ an toàn CPU (mặc định khi test boot lần đầu):** `sweetfb_native=1 sweetfb_hz=120 sweetmetal=0 sweetgl=0 sweet_glload=0`
  - **Chế độ GPU phần cứng đầy đủ (`--gpu`):** `gpu=3 gpu_delay=20 gpu_mmu=1 gpu_service=1 sweetfb_gpucopy=1 sweetmetal=2 sweetgl=1 sweet_glload=1 gpu_div=1 sweetfb_direct=0 gpu_mhz=1260 glwait=0`
    *(Lưu ý: **`sweetmetal=2`** kích hoạt **`HakiosMetal.bundle`** — driver Metal phần cứng thật cho iPadOS 26.1; `sweetmetal=1` chỉ nạp stub `SapphireMetal.bundle`).*
- **Ngoại vi:**
  - `sweettouch_rx=8640 sweettouch_ry=9600`: Chuẩn hóa tọa độ cảm ứng FT3519T của `sapphire`.
  - `sweetusb=2`: Bật mạng USB CDC-ECM (`192.168.7.2`) + stream log UDP cổng `5140`.
  - `wifi=1 modem=0` (mặc định bản Wi-Fi `iPad11,6`; đổi `modem=1` khi test Cellular `iPad11,7` hoặc khi cần PIL modem cấp nguồn WCN3988 tùy firmware).

### B. Các `sysctl` điều khiển trực tiếp lúc máy đang chạy (`sysctl kern.<tên>`)
| Tên `sysctl` | Chức năng | File kernel xử lý |
| --- | --- | --- |
| `kern.sweet_metal` | `0`: tắt Metal, `1`: `SapphireMetal`, **`2`: `HakiosMetal` (Adreno 610 thật)** | `bsd/dev/sweet_keys.c`, `SweetFramebuffer.cpp` |
| `kern.sweet_fb_hz` | Đổi tần số quét màn hình (`60` / `120`) | `SweetFramebuffer.cpp` |
| `kern.sweet_fb_rscale` | Tỷ lệ render động để giảm tải GPU | `SweetFramebuffer.cpp` |
| `kern.sweet_tap_wake` | `1`: Bật chạm đúp sáng màn hình (Double-tap to wake) | `bsd/dev/sweet_touch.c` |
| `kern.sweet_boot_android` | Ghi `1` để đánh dấu active cho slot `a` (Android) trên UFS LUN4 trước khi reboot | `bsd/dev/sweet_ufs.c` (`Android.app`, `SapphireTools.app`) |
| `kern.sweet_cpufreq` | Xem trạng thái hoặc ghi `"max"` / `"eco"` / `"balance"` / kHz để đổi mức xung nhịp cụm CPU Silver & Gold (Qualcomm OSM `0x0f521000` / `0x0f523000`) | `bsd/dev/sweet_cpufreq.c` (`zz-0103`) |
| `kern.sweet_cpu_max_freq` | Trần xung nhịp CPU động theo kHz (`300000`..`2803200`) dùng bởi `thermald` và panel sleep | `bsd/dev/sweet_cpufreq.c` (`zz-0103`) |
| `kern.sweet_gpu_max_freq` | Trần xung nhịp GPU Adreno 610 theo Hz (`310000000`..`1113600000`), trả về qua `MSM_PARAM_MAX_FREQ` | `bsd/dev/sweet_cpufreq.c`, `SapphireGpu.cpp` (`zz-0102`, `zz-0103`) |
| `kern.sweet_tsens` / `kern.sweet_tsens_sensors` | Đọc nhiệt độ nhân silicon trực tiếp từ phần cứng Qualcomm **`TSENS0`** (`SROT 0x04410000`, `TM 0x04411000 + 0xa0 + 4*i`) theo `m°C` | `bsd/dev/sweet_cpufreq.c`, `userland/thermal/thermald.c` (`zz-0103`) |
| `kern.sweet_thermal_zone0..3` / `sweet_thermal_state` / `sweet_thermal_policy` / `sweet_deep_sleep` | Telemetry 4 vùng nhiệt (SoC, GPU, Pin, Vỏ) và trạng thái ngủ sâu màn hình AMOLED đồng bộ với `thermald` | `bsd/dev/sweet_cpufreq.c`, `userland/thermal/thermald.c` (`zz-0103`) |
| `kern.sweet_touch_boost_enabled` / `sweet_launch_boost_enabled` | Bật/tắt tự động đẩy xung CPU lên trần ngay khi chạm màn hình hoặc mở app | `bsd/dev/sweet_cpufreq.c`, `bsd/dev/sweet_touch.c` (`zz-0103`) |
| `kern.sweet_inbox` | Đọc hộp thư `.ipa` / `.supdate` đẩy sang từ app Android DualBoot | `bsd/dev/sweet_ufs.c`, `userland/update/inboxcat.c` |

---

## 6. Bảng Phân Hệ Userland, Metal, Audio & Ứng Dụng Nội Bộ Trên iPadOS 26.1

| Thành phần hệ thống | Trạng thái trên iPadOS 26.1 | Cách đã tích hợp / điều chỉnh |
| --- | --- | --- |
| **`hakios-metal/` + `HakiosMetalCA.m` + `mesa/air/`** | **Dùng 100% (Cốt lõi)** | Build thành `HakiosMetal.bundle` (`arm64e` + De-PAC) qua `metal/build-hakios.sh`, hỗ trợ đường dẫn Cryptex OS 26 và tự tạo `QuartzCore-default.metallib` qua `tools/ipados26_ca_metallib.py`. Kích hoạt bằng `sweetmetal=2`. |
| **Mesa Freedreno + `eagl.c` + `glload.c` + `inject_sb.c`** | **Dùng 100%** | Build `libOSMesa.8.dylib`, `libredwood-gl.dylib`, `libredwood-glload.dylib`, `inject_sb.dylib` (`arm64e` + De-PAC) cho các game/app OpenGLES và tối ưu SpringBoard Darwin 25+. |
| **Audio (`RedwoodAudio.driver` + `arplay`)** | **Dùng 100%** | Build `arm64e` + De-PAC và cài vào `/Library/Audio/Plug-Ins/HAL/` & `/System/Library/Audio/Plug-Ins/HAL/` cùng daemon `arplay`. |
| **Daemons phần cứng (`camd`, `fpd`, `faced`, `sensord`, `vibd`, `thermald`, `rtcd`, `sapphire-gpsd`, `btshim`, `btmonitor`, `sapphire-toolsd`)** | **Dùng 100%** | Giao tiếp trực tiếp với kernel SM6225 qua `/dev/*`, QMI và `sysctl`; biên dịch `arm64e` + De-PAC và đăng ký sẵn vào `launchd.plist`. |
| **4 App nội bộ (`SapphireTools`, `Huella`, `Tienda`, `Android`)** | **Đã nâng cấp lên Universal iPad** | Cập nhật `UIDeviceFamily = [1, 2]` + xoay màn hình iPad; bổ sung binary gốc `/redwood/bin/sapphire-ipa-helper` để `Tienda.app` và hộp thư Android cài `.ipa` trực tiếp trên iPadOS 26.1 **qua trình cài gốc không cần helper bên thứ ba**. |
| **Tweaks (`SapphireDisplayLink`, `SapphireHEIF`, `SapphireSafari`, `SapphireButtonRegenerator`, `SapphireAVCamera`, `SapphireFigShim`, `SapphireNoCamera`, `SapphireHaptics`, `SapphireBTCapability`, `SapphireBiometry`, `SapphireFaceID`)** | **Dùng 100%** | Biên dịch `arm64e` + De-PAC (kèm `libheif`/`libde265` static cho giải mã ảnh HEIC phần mềm và mở khóa 120Hz UI). |
