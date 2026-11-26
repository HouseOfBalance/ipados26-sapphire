#!/bin/bash
# build.sh [OUTDIR]: Tienda.ipa, the app store (AltStore-format sources, installs through /redwood/bin/sapphire-ipa-helper)
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
CC="$CC_BIN -target arm64-apple-ios15.0 -isysroot $SDK $LDFLAGS -O2 -Wall"
out=${1:-/tmp}
mkdir -p "$out"
app=$(mktemp -d)
trap 'rm -rf "$app"' EXIT
A="$app/Payload/Tienda.app"
mkdir -p "$A"
cp app/Info.plist "$A/Info.plist"
python3 -I app/mkicon.py 120 "$A/AppIcon60x60@2x.png"
python3 -I app/mkicon.py 180 "$A/AppIcon60x60@3x.png"
$CC -fobjc-arc -framework Foundation -framework UIKit -framework WebKit -framework CoreGraphics -o "$A/Tienda" app/Tienda.m
"$LDID" -Sapp/Tienda.entitlements "$A/Tienda"
rm -f "$out/Tienda.ipa"
python3 -I - "$app" "$out/Tienda.ipa" <<'EOF'
import os, sys, zipfile
root, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(os.path.join(root, 'Payload')):
        for f in sorted(files):
            p = os.path.join(d, f)
            info = zipfile.ZipInfo(os.path.relpath(p, root))
            info.external_attr = (0o100755 if f == 'Tienda' else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, open(p, 'rb').read())
EOF
echo "$out/Tienda.ipa"
