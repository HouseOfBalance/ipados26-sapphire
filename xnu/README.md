# XNU 26.1 (`xnu-12377.41.6`) for iPadOS 26.1 on `sapphire` (SM6225Pro)

Builds Apple's open-source **XNU 26.1 (`xnu-12377.41.6`)** kernel (`MACHINE_CONFIG=SWEET`) for Xiaomi Redmi Note 13 4G (`sapphire`) using [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build):

- **GitHub Actions**: `.github/workflows/xnu-ipados26.yml` (`TOPAZ_4K=0` 16 KiB 4-core Gold or `TOPAZ_4K=1` 4 KiB 8-core Gold+Silver AMP) -> artifact `ipados26-xnu-kernel`.
- **Codemagic**: `codemagic.yaml` (`xnu-ipados26` and `xnu-ipados26-4k`).
- **CircleCI**: `.circleci/config.yml` (`xnu-ipados26-4k`).

## Patch Series (`xnu/patches-ipados26/`)
1. `00_setup_coreentitlements.sh`, `01-stage-libpthread-ipados26.sh`, `02-stage-bearssl.sh`, `03-stage-hfs-ipados26.sh` — Stage XNU 26.1 kernel dependencies and journaled HFS+ RW support.
2. `zz-0100-ipados26-xnu-12377-bringup.patch` — Full SM6225Pro SoC bring-up, drivers, `arm64e` shared-cache slide v3/v5, and OS 26 AMFI/Sandbox/LaunchConstraints support.
3. `zz-0101-ipados26-audit-fixes.patch` — Security, concurrency, TBI masking, and bounds-checking fixes.
4. `zz-0102-ipados26-gpu-display-opt.patch` — Persistent 1 GiB WC `SapphireGpu` pool, batched SMMU TLB flushes, and 120Hz `SweetFramebuffer` 64-bit SWAR alpha compositing.
5. `zz-0103-ipados26-cpufreq-tsens-ufs-touch.patch` — Dynamic Qualcomm OSM CPUFreq + `TSENS0` silicon thermal telemetry, `sweet_ufs` DMA/sleep tuning, and 21-byte fast FocalTech I2C touch reads.
6. `zz-0104-ipados26-armv82-fp16-rdm-emu.patch` — Pure-integer IEEE-754 `FEAT_FP16` (scalar + `ASIMDHP` `.4H`/`.8H`) and `FEAT_RDM` (`SQRDMLAH`/`SQRDMLSH`) instruction trap emulator in `osfmk/arm64/sleh.c`.
