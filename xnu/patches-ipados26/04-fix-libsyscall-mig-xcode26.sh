#!/usr/bin/env bash
# Fix Libsyscall MIG header generation under Xcode 26.1+ (which rejects '-arch armv7'
# in clang preprocessor invocations) and ensure bootstrap_cmds mig is used from FAKEROOT_DIR.
set -euo pipefail

python3 - <<'PY'
import pathlib

p = pathlib.Path("libsyscall/xcodescripts/mach_install_mig.sh")
if p.is_file():
    s = p.read_text(encoding="utf-8")
    # Xcode 26.1+ exports MACOSX/IPHONEOS/TVOS/WATCHOS/XROS/DRIVERKIT_DEPLOYMENT_TARGET simultaneously;
    # unset all of them before running mig so clang -E does not fail with "conflicting deployment targets".
    unset_block = (
        "unset MACOSX_DEPLOYMENT_TARGET IPHONEOS_DEPLOYMENT_TARGET TVOS_DEPLOYMENT_TARGET "
        "WATCHOS_DEPLOYMENT_TARGET XROS_DEPLOYMENT_TARGET DRIVERKIT_DEPLOYMENT_TARGET "
        "BRIDGEOS_DEPLOYMENT_TARGET\n"
    )
    # Xcode 26.1+ clang rejects '-arch armv7'; armv7k is 32-bit ARM ILP32 (preserves vm_map_internal.h struct layout)
    s = s.replace("MACHINE_ARCH=armv7\n", "MACHINE_ARCH=armv7k\n")
    old_mig = 'MIG=`xcrun -sdk "$SDKROOT" -find mig`'
    new_mig = (
        unset_block +
        'MIG_FB=$(find "${FAKEROOT_DIR:-/nonexistent}" -name mig -type f 2>/dev/null | head -n1 || true)\n'
        'MIG=${MIG_FB:-$(xcrun -sdk "$SDKROOT" -find mig)}'
    )
    s = s.replace(old_mig, new_mig)
    p.write_bytes(s.encode("utf-8"))
    print("Patched libsyscall/xcodescripts/mach_install_mig.sh for Xcode 26.1+ (unset deployment targets + armv7k + fakeroot mig)")
PY
