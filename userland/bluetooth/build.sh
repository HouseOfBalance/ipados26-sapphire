#!/bin/bash
# btshim: Apple's bluetoothd on the sapphire's WCN3988 (research/bluetooth.md). Install as
# /redwood/bin/btshim; jobs: com.sapphire.btshim ("btshim daemon", root) and com.apple.BlueTool
# replaced by "btshim bluetool" (see research/bluetooth.md, "iOS integration").
set -e
cd "$(dirname "$0")"
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
if [[ "$OSTYPE" == "darwin"* ]]; then
	CC_BIN="${CLANG:-${CC:-$(xcrun -find clang)}}"
	LDFLAGS=""
else
	L=${LLVM:-/usr/lib/llvm-18/bin}
	CC_BIN="${CLANG:-${CC:-$L/clang}}"
	LDFLAGS="-fuse-ld=lld"
fi
out=${1:-../obj}
mkdir -p "$out"
$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -Wextra -Wno-unused-parameter \
	-fblocks -o "$out/btshim" btshim.c
$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -Wextra -Wno-unused-parameter \
	-o "$out/btmonitor" btmonitor.c
# qcabtfw: the standalone Qualcomm bring-up (firmware download, BD address) for testing by hand
$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -Wextra -Wno-unused-parameter \
	-framework CoreFoundation -framework IOKit -o "$out/qcabtfw" qcabtfw.c
# btbridge.dylib: inserted into bluetoothd (btshim bdstart), answers its local-mac-address lookup
$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -Wextra -Wno-unused-parameter \
	-dynamiclib -install_name /redwood/lib/btbridge.dylib -framework CoreFoundation -framework IOKit \
	-o "$out/btbridge.dylib" btbridge.c
LDID=${LDID:-$(command -v ldid || ls "$HOME"/tools-dl/ldid/ldid 2>/dev/null || true)}
for f in btshim btmonitor qcabtfw btbridge.dylib; do
	[ -n "$LDID" ] && "$LDID" -S "$out/$f" || echo "sign $out/$f with ldid -S"
	echo "$out/$f"
done
