#!/bin/bash
# thermald: Qualcomm Thermal Manager for iOS on Sapphire
# Build: ./build.sh [OUTDIR]
#   thermald -> /redwood/bin/thermald (launchd job com.sapphire.thermald)

set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$(command -v ldid || ls "$HOME"/tools-dl/ldid/ldid 2>/dev/null || true)}
if [[ "$OSTYPE" == "darwin"* ]]; then
	CC_BIN="${CLANG:-${CC:-$(xcrun -find clang)}}"
	LDFLAGS=""
else
	CC_BIN="${CLANG:-${CC:-/usr/lib/llvm-18/bin/clang}}"
	LDFLAGS="-fuse-ld=lld"
fi
out=${1:-../obj}
mkdir -p "$out"
$CC_BIN -target arm64-apple-ios15.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -Wextra -o "$out/thermald" thermald.c
[ -n "$LDID" ] && "$LDID" -S "$out/thermald"
echo "$out/thermald"
