#!/bin/bash
# Setup CoreEntitlements V2 headers for XNU 26.1 (xnu-12377.41.6)
set -euo pipefail

EXTERNAL_HEADERS="./EXTERNAL_HEADERS"
KDKROOT_CLEAN="${KDKROOT:-}"
KDKROOT_CLEAN="${KDKROOT_CLEAN%/}"
KDK_CE_PATH=""

if [ -n "${KDKROOT_CLEAN}" ] && [ -d "${KDKROOT_CLEAN}/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform/CoreEntitlements" ]; then
    KDK_CE_PATH="${KDKROOT_CLEAN}/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform/CoreEntitlements"
fi
if [ -z "${KDK_CE_PATH}" ]; then
    for KDK in /Library/Developer/KDKs/KDK_26.*.kdk; do
        if [ -d "${KDK}/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform/CoreEntitlements" ]; then
            KDK_CE_PATH="${KDK}/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform/CoreEntitlements"
        fi
    done
fi

mkdir -p "${EXTERNAL_HEADERS}/CoreEntitlements/V2"
if [ -n "${KDK_CE_PATH}" ]; then
    cp "${KDK_CE_PATH}/V2/"*.h "${EXTERNAL_HEADERS}/CoreEntitlements/V2/"
fi

# Kernel.h is in KDK 26.4+; provide exact ABI fallback when building with KDK 26.1 on SWEET
# (SWEET uses built-in sweet_amfi.c and never invokes libCoreEntitlements)
KERNEL_H_REL="System/Library/KernelSupport/CoreEntitlements/V2/Kernel.h"
KERNEL_H=""
for KDK in "${KDKROOT_CLEAN}" /Library/Developer/KDKs/KDK_26.*.kdk; do
    if [ -n "${KDK}" ] && [ -f "${KDK}/${KERNEL_H_REL}" ]; then
        KERNEL_H="${KDK}/${KERNEL_H_REL}"
        break
    fi
done

if [ -n "${KERNEL_H}" ]; then
    cp "${KERNEL_H}" "${EXTERNAL_HEADERS}/CoreEntitlements/V2/Kernel.h"
else
    cat > "${EXTERNAL_HEADERS}/CoreEntitlements/V2/Kernel.h" <<'EOF'
#ifndef CORE_ENTITLEMENTS_V2_KERNEL_H
#define CORE_ENTITLEMENTS_V2_KERNEL_H
#include <CoreEntitlements/V2/API.h>
typedef struct CEKernelAPI {
    uint32_t version;
} CEKernelAPI_t;
#endif
EOF
fi
echo "CoreEntitlements V2 headers staged for XNU 26.1."
