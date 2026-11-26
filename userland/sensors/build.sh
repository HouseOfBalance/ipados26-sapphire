#!/bin/bash
# sensord: ADSP QMI SNS_CLIENT sensor daemon for iOS on Sapphire
# Build: ./build.sh [OUTDIR]
#   sensord -> /redwood/bin/sensord (launchd job com.sapphire.sensord)

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
CC=("$CLANG" -target arm64-apple-ios15.0 -isysroot "$SDK" $LDEXTRA -O2 -Wall -Wextra)
out=${1:-../obj}
mkdir -p "$out"
"${CC[@]}" -o "$out/sensord" sensord.c -lm
"${CC[@]}" -o "$out/snsprobe" snsprobe.c -lm
"${CC[@]}" -o "$out/sensload" sensload.c
"$LDID" -S "$out/sensord" "$out/snsprobe" "$out/sensload"
echo "$out/sensord $out/snsprobe $out/sensload"