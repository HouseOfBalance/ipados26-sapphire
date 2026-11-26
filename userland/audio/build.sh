#!/bin/bash
# RedwoodAudio.driver (HackiOS's HAL plug-in, from ios-topaz userland/audio, MIT): the output device
# of iOS, writing to /dev/redwood-audio (kernel 0200). Install on the phone as
# /System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver/{RedwoodAudio,Info.plist}, then restart mediaserverd.
# Build: ./build.sh [OUTDIR]
#   RedwoodAudio.driver -> /System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver

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
mkdir -p "$out/RedwoodAudio.driver"
cp audio_plugin.plist "$out/RedwoodAudio.driver/Info.plist"
$CC_BIN -target arm64-apple-ios15.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -bundle -framework CoreFoundation \
	-o "$out/RedwoodAudio.driver/RedwoodAudio" audio_plugin.c
[ -n "$LDID" ] && "$LDID" -S "$out/RedwoodAudio.driver/RedwoodAudio"
echo "$out/RedwoodAudio.driver"
