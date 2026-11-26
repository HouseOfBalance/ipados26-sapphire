# Partition, HFSX & Plist Utilities (`installer/`)

Standalone C utilities used for dual-boot UFS partitioning, offline HFSX volume construction, and binary/XML plist manipulation for **iPadOS 26.1 (`ipados26-sapphire`)**:

| Directory | Purpose |
| --- | --- |
| `part/` (`sapphire-part.c`) | Splits UFS LUN 0 (`sda`) GPT between Android `userdata` and iPadOS 26.1 `iosroot` (24+ GiB) safely |
| `mkhfs/` (`mkhfs.c`) | Builds a complete journaled case-sensitive HFSX (`iPadOSRoot`) image directly from layered directories with TN1150 Unicode NFD normalization and hard-link deduplication |
| `plistop/` (`plistop.c`) | Zero-dependency binary (`bplist00`) and XML plist reader/mutator |
| `tools-arm64/` | Static `aarch64` build scripts for `apfs-fuse`, `hfsprogs`, and `libheif`/`libde265` |

For the full iPadOS 26.1 (`23B85`) RootFS & Boot image build pipeline (including AEA decryption and `arm64e` -> ARMv8.0 De-PAC), see [`docs/IPADOS26_PORT_VI.md`](../docs/IPADOS26_PORT_VI.md) and `tools/ipados26-root.sh`.
