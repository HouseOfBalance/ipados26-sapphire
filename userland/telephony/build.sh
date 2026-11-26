#!/bin/bash
# qmitool (QMI to the modem over /dev/redwood-qmiN, kernel 0220) + CommCenter / BlastDoor bridges.
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
LDID=${LDID:-$(command -v ldid || ls "$HOME"/tools-dl/ldid/ldid 2>/dev/null || true)}
out=${1:-../obj}
mkdir -p "$out"

$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -Wextra -Wno-unused-parameter \
	-o "$out/qmitool" qmitool.c
echo "$out/qmitool"

# ccbridge.dylib: loaded into CommCenter (DYLD_INSERT_LIBRARIES), installed as /redwood/lib/ccbridge.dylib
$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -Wextra -Wno-unused-parameter \
	-dynamiclib -install_name /redwood/lib/ccbridge.dylib -framework CoreFoundation -framework IOKit \
	-o "$out/ccbridge.dylib" ccbridge.c
echo "$out/ccbridge.dylib"

# ccstart: CommCenter's launchd Program (/redwood/bin/ccstart), sets DYLD_INSERT_LIBRARIES and execs it
$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -o "$out/ccstart" ccstart.c
echo "$out/ccstart"

# pdpnet: the data call's IPv4 + DNS published to configd (/redwood/bin/pdpnet, run by qmitool datacall)
$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -Wextra -Wno-unused-parameter \
	-framework CoreFoundation -framework SystemConfiguration -framework CoreTelephony -o "$out/pdpnet" pdpnet.c
echo "$out/pdpnet"

# ctdial: a call straight through CommCenter (CTCallDial), for testing the dial path without the Phone app
$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -Wextra -Wno-unused-parameter \
	-framework CoreFoundation -o "$out/ctdial" ctdial.c
[ -n "$LDID" ] && "$LDID" -Sctdial.entitlements "$out/ctdial" 2>/dev/null || echo "sign $out/ctdial with ctdial.entitlements"
echo "$out/ctdial"

# libsapphire-fakebb.dylib + bbstart: the fake AppleBaseband for locationd and abm-helper
$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -Wextra -Wno-unused-parameter \
	-dynamiclib -install_name /redwood/lib/libsapphire-fakebb.dylib -framework CoreFoundation -framework IOKit \
	-o "$out/libsapphire-fakebb.dylib" fakebb.c
$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -o "$out/bbstart" bbstart.c
echo "$out/libsapphire-fakebb.dylib $out/bbstart"

# libsapphire-bdsandbox.dylib: BlastDoor sandbox bypass for incoming SMS
$CC_BIN -target arm64-apple-ios16.0 -isysroot "$SDK" $LDFLAGS -O2 -Wall -Wextra -Wno-unused-parameter \
	-dynamiclib -install_name /redwood/lib/libsapphire-bdsandbox.dylib \
	-o "$out/libsapphire-bdsandbox.dylib" bdsandbox.c
echo "$out/libsapphire-bdsandbox.dylib"

if [ -n "$LDID" ]; then
	for f in qmitool ccbridge.dylib ccstart pdpnet libsapphire-fakebb.dylib bbstart libsapphire-bdsandbox.dylib; do
		"$LDID" -S "$out/$f" 2>/dev/null || true
	done
fi
