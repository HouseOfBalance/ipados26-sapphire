#!/bin/bash
# build.sh OUTDIR: SapphireCallButtons.dylib (+ .plist) for /var/jb/usr/lib/TweakInject (ElleKit)
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
if [[ "$OSTYPE" == "darwin"* ]]; then
	CC_BIN="${CLANG:-${CC:-$(xcrun -find clang)}}"
	LDFLAGS=""
else
	CC_BIN="${CLANG:-${CC:-/usr/lib/llvm-18/bin/clang}}"
	LDFLAGS="-fuse-ld=lld"
fi
out=${1:-/tmp}
mkdir -p "$out"
"$CC_BIN" -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -dynamiclib \
	-install_name /var/jb/usr/lib/TweakInject/SapphireCallButtons.dylib -lobjc -framework Foundation \
	-o "$out/SapphireCallButtons.dylib" SapphireCallButtons.m
"$LDID" -S "$out/SapphireCallButtons.dylib"
cp SapphireCallButtons.plist "$out/SapphireCallButtons.plist"
echo "$out/SapphireCallButtons.dylib"
