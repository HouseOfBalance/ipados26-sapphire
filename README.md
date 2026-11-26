# ipados26-sapphire

**Apple iPadOS 26.1 (`23B85`) running on the Xiaomi Redmi Note 13 4G (`sapphire`, Snapdragon 685 / SM6225Pro).**

This project ports Apple's open-source **XNU 26.1 kernel (`xnu-12377.41.6`)** and the **iPadOS 26.1 (`iPad11,6` Wi-Fi / `iPad11,7` Cellular, A12 `arm64e`)** operating system to the **Xiaomi Redmi Note 13 4G** (`sapphire` / `sapphiren`, Qualcomm **SM6225Pro "khaje"** / Snapdragon 685, Adreno 610 v2 `0x06010001`), supporting non-destructive RAM booting (`fastboot boot`) and Android HyperOS dual-booting.

---

## 1. Architecture Overview (`arm64e` iPadOS 26.1 on ARMv8.0-A Kryo 260)

iPadOS 26.1 ships exclusively as **`arm64e`** (compiled for Apple A12 ARMv8.3-A with Pointer Authentication, LSE atomics, RCpc acquire loads, `FEAT_FP16`, `FEAT_RDM`, `FJCVTZS`, and `DC CVAP`), while Qualcomm's Kryo 260 Gold/Silver cores implement **ARMv8.0-A**. `ipados26-sapphire` bridges this gap across six integrated layers:

1. **Offline `arm64e` -> ARMv8.0 Binary Rewriter & CodeDirectory Rehasher (`tools/ipados26_depac.py`)**:
   - Rewrites executable instruction sections across `dyld_shared_cache_arm64e*` subcaches and standalone Mach-Os in-place with zero displacement:
     - `RETAA/RETAB` -> `RET`, `BLRAA*` -> `BLR`, `BRAA*` -> `BR`, `ERETAA/ERETAB` -> `ERET`
     - `PAC*` / `AUT*` -> `NOP`, `XPACI` -> `AND #0x0000ffffffffffff`, `XPACLRI` -> `AND X30, X30, #0x0000ffffffffffff`
     - **TBI-safe `XPACD`** -> `AND #0xff00ffffffffffff` (preserving Darwin top-byte tags `[63:56]` used by ObjC `isa` pointers, Swift, and `nanov2` malloc)
     - `LDAPR/LDAPRB/LDAPRH` -> native ARMv8.0 `LDAR/LDARB/LDARH` (zero trap overhead on C11/Swift/ObjC acquire loads)
     - `DC CVAP` / `DC CVADP` -> native ARMv8.0 `DC CVAC`, `SB` -> `ISB`
     - `LDRAA/LDRAB` -> `LDR` / `LDUR`
   - Recomputes modified `CS_CodeDirectory` SHA-256/SHA-384 page hashes in-place (honoring `SC_Scatter` tables and alternate CodeDirectories) while preserving Apple XML/DER entitlements and Launch Constraints.
2. **XNU 26.1 (`xnu-12377.41.6`) Kernel & Complete ISA Trap Emulator (`xnu/patches-ipados26/`)**:
   - Accepts `CPU_SUBTYPE_ARM64E` Mach-Os and shared-cache slide v3/v5 without hardware PAC; bypasses OS 26 Launch Constraints and strips `CS_REQUIRE_LV`/`CS_RESTRICT` in `sweet_amfi.c` for tweak/bridge injection.
   - Emulates remaining runtime/JIT instructions in `osfmk/arm64/sleh.c` & `machine_routines_asm.s`:
     - **LSE Atomics & 64/128-bit `CASP`** (`_sweet_lse_*`, `_sweet_casp_{w,x}` protected inside `copyio_recover_table`).
     - **ARMv8.2 `FEAT_FP16` (Scalar `H0..H31` & Vector `ASIMDHP` `.4H`/`.8H`) + ARMv8.1 `FEAT_RDM` (`SQRDMLAH`/`SQRDMLSH`)**: pure-integer `__int128` single-rounding IEEE-754 RNE emulator (`sweet_emulate_fp16_rdm`, safe under `-mgeneral-regs-only`).
     - **ARMv8.2 `FEAT_DotProd` (`SDOT`/`UDOT`), `FEAT_FHM` (`FMLAL`/`FMLSL`/`FMLAL2`/`FMLSL2`), ARMv8.3 `FEAT_FCMA` (`FCADD`/`FCMLA` `.4H/.8H`/`.2S/.4S`/`.2D`), and `FEAT_FlagM` (`CFINV`/`XAFLAG`/`AXFLAG`)**: pure-integer single-rounding IEEE-754 RNE emulator (`sweet_emulate_dotprod_fcma_fhm` in `zz-0105`).
     - **ARMv8.3 `FJCVTZS`**, signed `LDAPRSB/H/W`, writeback `LDRAA/B`, and `PACGA`.
3. **Hardware Metal Driver for Adreno 610 (`hakios-metal/`, `userland/src/HakiosMetalCA.m`, `userland/mesa/air/`)**:
   - `HakiosMetal.bundle` (`sweetmetal=2`) compiles Apple Metal AIR bitcode -> NIR -> Freedreno `ir3` machine code in-process for Adreno 610 (`0x06010001`), backed by a **Fence-Aware Userspace BO Recycle Pool** (0-syscall transient buffer reuse) and persistent shader cache (`/private/var/db/hakios-metal-cache`).
   - `SapphireGpu.cpp` (`zz-0102`) maps the 1 GiB Write-Combined GPU DDR pool (`0x1_0000_0000`) persistently in kernel VA (zero per-BO `pmap`/TLB shootdowns), uses inline stack submit arrays, and batches SMMU TLB flushes.
4. **120Hz Display & SWAR Multi-Layer Compositor (`SweetFramebuffer.cpp`, `zz-0102`)**:
   - Drives the 1080x2400 AMOLED DSI panel at 120Hz (`sweetfb_native=1 sweetfb_hz=120`), using Adreno 610 2D hardware blits (`sweetfb_gpucopy=1`) and a 64-bit SWAR premultiplied `BGRA8888` alpha compositor for multi-layer iPadOS overlays without unnecessary Write-Combined cache flushes.
5. **Dynamic Qualcomm OSM DVFS & `TSENS0` Silicon Thermal Management (`sweet_cpufreq.c`, `zz-0103`, `userland/thermal/thermald.c`)**:
   - Scales Kryo 260 Gold (`300 MHz`..`2.80 GHz`, `0x0f523000`) & Silver (`300 MHz`..`1.90 GHz`, `0x0f521000`) clusters dynamically, drops to 300 MHz Eco floor on panel blank, boosts on touch/wake, and exports live silicon die temperatures from Qualcomm `TSENS0` (`0x04410000`/`0x04411000`) via `kern.sweet_tsens`.
6. **Full Peripherals, Audio, Biometrics & Native IPA Installer (`userland/`)**:
   - UFS 2.2 root storage (`ufscache=1`, `ufswait=1`), FocalTech FT3519T 120Hz touch (fast 21-byte I2C reads), DWC3 USB CDC-ECM networking (`192.168.7.2`), WCN3988 Wi-Fi & Bluetooth (`btshim`/`btmonitor`), AudioReach `RedwoodAudio.driver` + `arplay`, 108MP S5KHM6 camera ISP (`camd`), Goodix optical FOD (`fpd` + `Huella.app`), front-camera Face unlock (`faced`), and native CoreTrust-signing `.ipa` installer (`/redwood/bin/sapphire-ipa-helper` + `Tienda.app`).

---

## 2. Subsystem Status Matrix

| Subsystem | Driver / Component | Status on iPadOS 26.1 |
| --- | --- | --- |
| **Boot & SMP** | `shim/` (boot image v4) + `uefi/` (Mu-Silicium) + XNU 26.1 | 4 Gold cores (16K kernel) or all 8 Gold+Silver cores (`--amp` 4K kernel) |
| **Root Storage** | `sweet_ufs.c` (`zz-0103`) + journaled HFSX `iPadOSRoot` (`iosroot`) | Read/Write with `ufscache=1` & `ufswait=1` yield |
| **Display (120Hz)** | `SweetFramebuffer.cpp` (`zz-0102`) + `SapphireDisplayLink.dylib` | 1080x2400 @ 120Hz (1:1 `@2x` iPad Retina or 1620x2160 `--ipad-canvas`) |
| **Metal & OpenGLES** | `HakiosMetal.bundle` (`sweetmetal=2`) + `SapphireGpu.cpp` + `eagl.dylib` | Hardware Adreno 610 3D/Compute/2D + Mesa Freedreno GLES bridge |
| **CPU DVFS & Thermal** | `sweet_cpufreq.c` (`zz-0103`) + `TSENS0` + `thermald` | Dynamic 300 MHz..2.80 GHz OSM scaling, panel-off Eco, 4-tier thermal cap |
| **Touch & Buttons** | `sweet_touch.c` (FT3519T QUP0 SE2) + `sweet_keys.c` (PM6125 SPMI) | 250Hz active polling, 21-byte fast I2C read, double-tap wake, Home bar |
| **USB & Debug** | `sweet_usb.c` (`sweetusb=2`) + `SWEETLOG` black-box ring | `192.168.7.2` SSH (`22`/`2222`), emergency shell (`2323`), UDP `5140` log |
| **Audio / Wi-Fi / BT** | `RedwoodAudio.driver` + `arplay`, `sapphire_wlan.c`, `sweet_bt.c` | Speaker/headset playback, WCN3988 station `en0`, BLE/HCI shim |
| **Camera / Biometrics** | `sweet_cam.c` + `camd`, `SweetQsee.cpp` + `fpd` + `Huella.app`, `faced` | 108MP S5KHM6 / OV16A1Q ISP, QSEE optical fingerprint, Face unlock |
| **Apps & Jailbreak** | Procursus + Sileo + ElleKit + `SapphireTools`, `Tienda`, `Android` | Rootless `/var/jb`, Universal iPad UI (`UIDeviceFamily=[1,2]`), direct IPA install |

---

## 3. Quick Build & Boot Pipeline

### Step 1 — Build Kernel & Userland via CI
- **Kernel (`xnu-12377.41.6`)**: Run `.github/workflows/xnu-ipados26.yml` on GitHub Actions (or `xnu-ipados26` / `xnu-ipados26-4k` on Codemagic) -> download `ipados26-xnu-kernel` to `xnu/out-ipados26/`.
- **Userland (`arm64e` + De-PAC)**: Run `.github/workflows/userland-ipados26.yml` -> download `ipados26-userland-binaries` to `userland/obj-ipados26/`.

### Step 2 — Build 24 GiB iPadOS 26.1 RootFS Image (Linux / WSL2)
Download Apple's official `iPad_10.2_2020_26.1_23B85_Restore.ipsw` and run:
```bash
sudo bash tools/ipados26-root.sh \
  --with-jb \
  --root-pass 1234 \
  --userland-dir userland/obj-ipados26 \
  ~/work/iPad_10.2_2020_26.1_23B85_Restore.ipsw \
  ~/work/out-ipados26 \
  ~/modules/hfsplus.ko
```
Produces `~/work/out-ipados26/ipados26-root.img` (AEA decrypted, HFSX merged, `dyld_shared_cache_arm64e` + Mach-O De-PAC'd, QuartzCore metallib & DSC offsets generated, Procursus + OpenSSH + `HakiosMetal.bundle` installed).

### Step 3 — Build RAM Boot Image
```bash
# Safe first boot (CPU renderer + on-screen console log):
./tools/ipados26-boot.sh --console \
  --gpu-fw ~/work/gpu-fw --ipa-fw ~/work/gpu-fw \
  xnu/out-ipados26/kernel.development.sweet \
  ~/work/out-ipados26/ipados26-boot.img

# Full Adreno 610 Hardware Metal (sweetmetal=2) + 120Hz + ElleKit tweaks:
./tools/ipados26-boot.sh --gpu \
  --gpu-fw ~/work/gpu-fw --ipa-fw ~/work/gpu-fw \
  xnu/out-ipados26/kernel.development.sweet \
  ~/work/out-ipados26/ipados26-boot-gpu.img
```

### Step 4 — Flash `iosroot` & Boot from RAM
1. In OrangeFox Recovery, write `ipados26-root.img` to `/dev/block/by-name/iosroot` (`tools/windows-wsl/push_root.ps1 -Partition iosroot -TotalGiB 24`).
2. Reboot to Fastboot and boot in RAM (**never `fastboot flash boot`**):
   ```bash
   fastboot boot ipados26-boot-gpu.img
   ```

---

## 4. Repository Layout

| Directory / File | Description |
| --- | --- |
| `IPADOS26_XNU_SPECS.md` | Complete SM6225Pro hardware register/IRQ/GPIO/SMMU/DDR map, `boot-args`, and `sysctl` reference |
| `IPADOS26_HANDOVER_VI.md` | Engineering & AI-agent evaluation handover guide (Vietnamese) |
| `NOTES.md` | Structured engineering changelog for the iPadOS 26.1 port & driver optimizations |
| `xnu/patches-ipados26/` | Modular XNU 26.1 (`xnu-12377.41.6`) patch series (`zz-0100` bring-up .. `zz-0105` `DotProd`/`FCMA`/`FHM`/`FlagM`) |
| `hakios-metal/` | C++17 Adreno 610 Metal driver engine (PM4/A6XX state, AIR->NIR->ir3 compiler, BO Recycle Pool) |
| `metal/` | `HakiosMetal.bundle` (`build-hakios.sh`) & diagnostic `SapphireMetal.bundle` (`build.sh`) |
| `shim/` & `uefi/` | Bare-metal ARM64 boot image v4 shim (Silver->Gold SMP handoff) & Mu-Silicium UEFI package |
| `userland/` | All `arm64e` daemons, Mesa Freedreno + AIR front-end, CoreAudio HAL, ElleKit tweaks, and Universal iPad apps |
| `tools/` | `ipados26-root.sh`, `ipados26-boot.sh`, `ipados26-jb-install.sh`, `ipados26_depac.py`, `ipados26_im4p_aea.py`, `ipados26_dsc_offsets.py`, `ipados26_mkadt.py`, `ipados26_ca_metallib.py`, `ipados26-klog.py` |
| `tests/` | Automated cross-platform unit test suite (`run_unit_tests.py` covering De-PAC, CodeDirectory, ADT, Metallib v3, DSC, and XNU patches) |
| `installer/` | Standalone C utilities (`mkhfs`, `sapphire-part` GPT splitter, `plistop`, static `tools-arm64`) |
| `android/dualboot/` | Companion Android DualBoot manager app (`SapphireDualBoot.apk`, HFS+ reader, `.ipa` inbox, slot switcher) |
| `docs/` | Detailed Vietnamese & English installation, WSL2 build, and bring-up guides |
