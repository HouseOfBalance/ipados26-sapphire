# Working on `ipados26-sapphire` (iPadOS 26.1 on Xiaomi Redmi Note 13 4G)

- **Target OS & Hardware**: Apple **iPadOS 26.1** (`iPad_10.2_2020_26.1_23B85_Restore.ipsw`, `iPad11,6` `J171aAP` Wi-Fi / `iPad11,7` `J172aAP` Cellular, Apple A12 `arm64e` userland) running on **Xiaomi Redmi Note 13 4G** (`sapphire` / `sapphiren`, Qualcomm Snapdragon 685 / SM6225Pro `"khaje"`, 4x Kryo 260 Gold Cortex-A73 + 4x Kryo 260 Silver Cortex-A53 ARMv8.0-A, Adreno 610 v2 `0x06010001`).
- **Read First**:
  1. `README.md` — System architecture, subsystem matrix, build & boot pipeline.
  2. `IPADOS26_XNU_SPECS.md` — Verified hardware MMIO/IRQ/GPIO/PAS/SMMU/DDR maps, ADT `/arm-io` rules, `boot-args`, and `kern.sweet_*` sysctls.
  3. `IPADOS26_HANDOVER_VI.md` & newest entries of `NOTES.md` — Operational invariants and engineering changelog.
- **Safety Invariants**:
  - Always boot kernels from RAM via `fastboot boot ipados26-boot.img`; **never** run `fastboot flash boot` on Android's active slot (recovery uses the `boot` partition kernel; see `RECOVERY.md`).
  - Dual-boot root filesystem lives in `iosroot` (24+ GiB journaled case-sensitive HFS+, `ufsroot=iosroot`). Never touch `persist`, `modemst1/2`, `fsg`, `fsc`, or Android's slot `a` partitions.
  - All userland Mach-O binaries/dylibs/bundles targeting iPadOS 26.1 must be compiled as `-target arm64e-apple-ios16.0`, signed with `ldid -S` / `codesign`, and processed by `tools/ipados26_depac.py --rewrite` so non-NOP PAC/ARMv8.3 instructions are rewritten to native ARMv8.0 instructions with rehashed `CS_CodeDirectory` SHA-256 pages.
  - Keep device dumps, serial numbers, boot logs, Qualcomm firmware blobs, and Apple IPSW/DMG/ADT files out of git.
- **CI Workflows**:
  - Kernel (`xnu-12377.41.6`): `.github/workflows/xnu-ipados26.yml`, `codemagic.yaml` (`xnu-ipados26` / `xnu-ipados26-4k`), `.circleci/config.yml`.
  - Userland + `HakiosMetal.bundle` + Mesa AIR + Tweaks + Universal Apps: `.github/workflows/userland-ipados26.yml`.
