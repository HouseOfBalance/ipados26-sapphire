# Installing iPadOS 26.1 Dual-Boot on Redmi Note 13 4G (`sapphire`)

`ipados26-sapphire` runs **Apple iPadOS 26.1 (`23B85`)** alongside **Android (HyperOS / AOSP)** on the **Xiaomi Redmi Note 13 4G** (`sapphire` / `sapphiren`, Snapdragon 685 / SM6225Pro). Android remains intact on slot `a`, while iPadOS 26.1 resides in a dedicated `iosroot` GPT partition (24+ GiB).

---

## 1. Prerequisites

| Requirement | Details |
| --- | --- |
| **Device** | Xiaomi Redmi Note 13 4G (`sapphire` / `sapphiren`) with **unlocked bootloader** |
| **Recovery** | **OrangeFox Recovery** (with root `adb` shell) |
| **Host PC** | Linux or Windows 10/11 with **WSL2 Ubuntu** (`apfs-fuse`, `hfsprogs`, `ipsw`, `aarch64-linux-gnu-gcc`) |
| **Apple IPSW** | `iPad_10.2_2020_26.1_23B85_Restore.ipsw` (downloaded directly from Apple; never redistributed here) |
| **Storage Split** | Minimum **24 GiB** for `iosroot` (32–40 GiB recommended) + 16+ GiB for Android `userdata` |

---

## 2. Partitioning (`userdata` + `iosroot`)

If your device does not yet have an `iosroot` partition on UFS LUN 0 (`sda`), use `installer/part/sapphire-part.c` inside OrangeFox Recovery after backing up all Android data (shrinking `userdata` requires formatting `userdata` once). Subsequent reinstalls of `ipados26-root.img` into `iosroot` leave Android completely untouched.

---

## 3. Building & Writing `ipados26-root.img` and Booting

1. Build or download CI artifacts from `.github/workflows/xnu-ipados26.yml` (`ipados26-xnu-kernel`) and `.github/workflows/userland-ipados26.yml` (`ipados26-userland-binaries`).
2. Generate `ipados26-root.img` (24 GiB) via `sudo bash tools/ipados26-root.sh --with-jb --userland-dir userland/obj-ipados26 <23B85.ipsw> <OUTDIR> [hfsplus.ko]`.
3. Generate `ipados26-boot-gpu.img` via `./tools/ipados26-boot.sh --gpu --gpu-fw <fwdir> --ipa-fw <fwdir> <kernel> ipados26-boot-gpu.img`.
4. Write `ipados26-root.img` to `/dev/block/by-name/iosroot` from OrangeFox Recovery (`tools/windows-wsl/push_root.ps1`), then boot in Fastboot:
   ```bash
   fastboot boot ipados26-boot-gpu.img
   ```
5. Install `SapphireDualBoot.apk` (`android/dualboot/`) on Android to inspect the HFS+ volume, push `.ipa` packages into the shared inbox (installed automatically on iPadOS 26.1 by `sapphire-ipa-helper` / `Tienda.app`), and read `SWEETLOG` kernel logs.
