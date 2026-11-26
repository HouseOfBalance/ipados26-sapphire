#!/bin/sh
# Build SapphireMetal.bundle for iOS 16 (arm64) on Linux: clang + ld64.lld against the
# hand-written link stubs in tbd/, ad-hoc signed. No Apple SDK needed.
#   metal/build.sh [OUTDIR]      (default metal/out)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
ARCH=${ARCH:-arm64}
IOS_VER=${IOS_VER:-16.0}
b=$out/SapphireMetal.bundle
mkdir -p "$b"
CLANG_BIN="${CLANG:-clang}"
"$CLANG_BIN" -target "${ARCH}-apple-ios${IOS_VER}" -nostdinc -fobjc-runtime=ios-16.0 -fno-objc-arc -O2 -Wall -Wno-objc-root-class \
    -c "$here/SapphireMetal/SapphireMetal.m" -o "$out/SapphireMetal.o"
for c in sgpu sgpu_selftest; do
    "$CLANG_BIN" -target "${ARCH}-apple-ios${IOS_VER}" -nostdinc -O2 -Wall -c "$here/SapphireMetal/$c.c" -o "$out/$c.o"
done
cat > "$b/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>SapphireMetal</string>
	<key>CFBundleIdentifier</key><string>dev.angelpro09.sapphire.metal</string>
	<key>CFBundleName</key><string>SapphireMetal</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleShortVersionString</key><string>0.1</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>NSPrincipalClass</key><string>SapphireMTLDevice</string>
</dict>
</plist>
PLIST
if [ "$(uname -s)" = "Darwin" ]; then
    "$CLANG_BIN" -target "${ARCH}-apple-ios${IOS_VER}" -nostdlib -bundle -o "$b/SapphireMetal" \
        "$out/SapphireMetal.o" "$out/sgpu.o" "$out/sgpu_selftest.o" \
        "$here/tbd/libobjc.tbd" "$here/tbd/Metal.tbd" "$here/tbd/libSystem.tbd" "$here/tbd/IOKit.tbd"
    codesign --force --sign - "$b"
else
    "${LD64:-$(command -v ld64.lld || ls /usr/lib/llvm-*/bin/ld64.lld | tail -1)}" -arch "$ARCH" -platform_version ios "$IOS_VER" "$IOS_VER" -bundle -o "$b/SapphireMetal" "$out/SapphireMetal.o" "$out/sgpu.o" "$out/sgpu_selftest.o" \
        "$here/tbd/libobjc.tbd" "$here/tbd/Metal.tbd" "$here/tbd/libSystem.tbd" "$here/tbd/IOKit.tbd" -adhoc_codesign
fi
if [ "$ARCH" = "arm64e" ] && [ -f "$here/../tools/ipados26_depac.py" ]; then
    python3 "$here/../tools/ipados26_depac.py" --rewrite "$b/SapphireMetal"
fi
echo "$b"
