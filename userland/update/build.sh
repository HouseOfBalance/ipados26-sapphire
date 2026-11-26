#!/bin/bash
# build.sh OUTDIR: the iOS side of updates -> /redwood/bin/
#   sapphire-inboxcat    reads the inbox through kern.sweet_inbox (kernel 0310)
#   sapphire-update.sh   applies its *.supdate entries (run by sapphire-firstboot.sh at every boot)
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
LDID=${LDID:-$(command -v ldid 2>/dev/null || find "$HOME/tools-dl/ldid" -type f -name ldid 2>/dev/null | head -1)}
out=${1:-/tmp}
mkdir -p "$out"
"$CLANG" -target arm64-apple-ios15.0 -isysroot "$SDK" $LDEXTRA -O2 -Wall -o "$out/sapphire-inboxcat" inboxcat.c
"$LDID" -S "$out/sapphire-inboxcat"
cp sapphire-update.sh sapphire-appwatch.sh sapphire-ipa-install.sh "$out/"
chmod 755 "$out/sapphire-update.sh" "$out/sapphire-appwatch.sh" "$out/sapphire-ipa-install.sh"
ls -l "$out/sapphire-inboxcat" "$out/sapphire-update.sh" "$out/sapphire-ipa-install.sh"
