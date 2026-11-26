#!/bin/bash
# build.sh OUTDIR: SapphireSafari.dylib (+ .plist) for /var/jb/usr/lib/TweakInject (ElleKit): Safari's container
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$(find $HOME/tools-dl/ldid -type f -name ldid 2>/dev/null | head -1)}
LDID=${LDID:-ldid}
if [[ "$OSTYPE" == "darwin"* ]]; then
	CC_BIN="${CLANG:-${CC:-$(xcrun -find clang)}}"
	LDFLAGS=""
else
	CC_BIN="${CLANG:-${CC:-/usr/lib/llvm-18/bin/clang}}"
	LDFLAGS="-fuse-ld=lld"
fi
out=${1:-/tmp}
mkdir -p "$out"
"$CC_BIN" -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -fobjc-arc -dynamiclib \
	-install_name /var/jb/usr/lib/TweakInject/SapphireSafari.dylib -framework Foundation -lobjc \
	-o "$out/SapphireSafari.dylib" SapphireSafari.m
"$LDID" -S "$out/SapphireSafari.dylib"
cp SapphireSafari.plist "$out/SapphireSafari.plist"
echo "$out/SapphireSafari.dylib"
