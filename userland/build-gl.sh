#!/bin/bash
# The EAGL bridge (HackiOS's eagl.c, userland/hackios/) over our Mesa: obj/libredwood-gl.dylib.
set -e
cd "$(dirname "$0")"
M=${M:-$HOME/mesa-ios/out}
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
ARCH=${ARCH:-arm64}
IOS_VER=${IOS_VER:-16.0}
OUT=${OUT:-obj}
if [ "$(uname -s)" = "Darwin" ]; then
	CLANG_BIN=${CLANG:-$(xcrun -find clang)}
	LLD_FLAG=""
else
	L=${LLVM:-/usr/lib/llvm-18/bin}
	export PATH=$L:$PATH
	CLANG_BIN=${CLANG:-clang}
	LLD_FLAG="-fuse-ld=lld"
fi
mkdir -p "$OUT"
python3 mkeagl.py src/eagl_slots.txt "$M/gl.xml" "$M/exported-gl.txt" src/eagl.c > "$OUT/eagl_gen.h"
"$CLANG_BIN" -target "${ARCH}-apple-ios${IOS_VER}" -isysroot "$SDK" ${EXTRA_FLAGS:-} $LLD_FLAG -O2 -Wall -Wno-unused-function -Wno-frame-address \
	-dynamiclib -Wl,-undefined,dynamic_lookup -install_name /redwood/lib/libredwood-gl.dylib -I"$OUT" -Isrc -framework CoreFoundation -framework IOKit \
	-framework IOSurface -lobjc "$M/libOSMesa.8.dylib" -o "$OUT/libredwood-gl.dylib" src/eagl.c src/gl_osmesa.c
"$CLANG_BIN" -target "${ARCH}-apple-ios${IOS_VER}" -isysroot "$SDK" ${EXTRA_FLAGS:-} $LLD_FLAG -O2 -Wall -dynamiclib \
	-install_name /redwood/lib/libredwood-glload.dylib -framework CoreFoundation -o "$OUT/libredwood-glload.dylib" src/glload.c
if [ "$ARCH" = "arm64e" ] && [ -f ../tools/ipados26_depac.py ]; then
	python3 ../tools/ipados26_depac.py --rewrite "$OUT/libredwood-gl.dylib" "$OUT/libredwood-glload.dylib"
fi
echo "$OUT/libredwood-gl.dylib $OUT/libredwood-glload.dylib"
