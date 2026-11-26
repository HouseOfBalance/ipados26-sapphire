#!/bin/bash
# build.sh OUTDIR: arplay for the phone, and the voice call scripts from the AudioReach harness logs
# (AR_OUT, default ~/ar-harness/out)
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
AR_OUT=${AR_OUT:-$HOME/ar-harness/out}
out=${1:-/tmp}
mkdir -p "$out"
"$CLANG" -target arm64-apple-ios16.0 -isysroot "$SDK" $LDEXTRA -O2 -Wall -o "$out/arplay" arplay.c
"$LDID" -S "$out/arplay"
if [ -d "$AR_OUT" ]; then
	python3 -I arconv.py "$AR_OUT/voice_call_handset_full.log" "$out/voice_handset.arp" --rx-intf 2 --rx-mask 1
	python3 -I arconv.py "$AR_OUT/voice_call_speaker_full.log" "$out/voice_speaker.arp"
	python3 -I arconv.py "$AR_OUT/record_handset_mic_48k.log" "$out/record_mic.arp" --record
fi
