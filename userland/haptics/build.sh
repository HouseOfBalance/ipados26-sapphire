#!/bin/bash
# vibd: vibration motor daemon for iOS on Sapphire (ERM motor via PM7250B LDO)
# Build: ./build.sh [OUTDIR]
#   vibd -> /redwood/bin/vibd (launchd job com.sapphire.vibd)

set -e
cd "$(dirname "$0")"
if [ "$(uname -s)" = "Darwin" ]; then
	SDK=${SDK:-$(xcrun --sdk iphoneos --show-sdk-path)}
	CLANG=${CLANG:-$(xcrun -sdk iphoneos -find clang)}
	LDEXTRA=""
else
	SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
	CLANG=${CLANG:-/usr/lib/llvm-18/bin/clang}
	LDEXTRA="-fuse-ld=lld"
fi
LDID=${LDID:-$(command -v ldid 2>/dev/null || echo "$HOME/tools-dl/ldid/ldid")}
out=${1:-../obj}
mkdir -p "$out"
"$CLANG" -target arm64-apple-ios15.0 -isysroot "$SDK" $LDEXTRA -O2 -Wall -Wextra -o "$out/vibd" vibd.c
"$LDID" -S "$out/vibd"
echo "$out/vibd"