#!/bin/bash
# Build HakiosMetal.bundle (Adreno 610 hardware Metal + CoreAnimation SPI + Mesa AIR->ir3 compiler)
# for iOS / iPadOS 26.1 (ARCH=arm64 or ARCH=arm64e + De-PAC).
#
# Usage:
#   ARCH=arm64e SDK="$SDK_PATH" MESA_ROOT="$HOME/mesa-ios-26" bash metal/build-hakios.sh [OUTDIR]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
OUT="${1:-$HERE/out}"
ARCH="${ARCH:-arm64e}"
IOS_VER="${IOS_VER:-16.0}"
SDK="${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}"
MESA_ROOT="${MESA_ROOT:-$HOME/mesa-ios-26}"
MESA_VER="${MESA_VER:-25.0.7}"
MESA_SRC="${MESA_SRC:-$MESA_ROOT/mesa-$MESA_VER}"
MESA_BUILD="${MESA_BUILD:-}"

if [[ -z "$MESA_BUILD" ]]; then
	MESA_BUILD=$(ls -d "$MESA_ROOT"/build-"${ARCH}"-* 2>/dev/null | head -n 1 || true)
fi
if [[ -z "$MESA_BUILD" || ! -f "$MESA_BUILD/build.ninja" ]]; then
	echo "build-hakios.sh: missing Mesa cross build in $MESA_ROOT (run userland/mk-mesa.sh first)" >&2
	exit 1
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
	CLANG_BIN="${CLANG:-$(xcrun -find clang)}"
	CLANGXX_BIN="${CLANGXX:-$(xcrun -find clang++)}"
	AR_BIN="${AR:-$(xcrun -find ar)}"
	LD_EXTRA=""
else
	LLVM_DIR="${LLVM:-/usr/lib/llvm-18/bin}"
	CLANG_BIN="${CLANG:-$LLVM_DIR/clang}"
	CLANGXX_BIN="${CLANGXX:-$LLVM_DIR/clang++}"
	AR_BIN="${AR:-$LLVM_DIR/llvm-ar}"
	LD_EXTRA="-fuse-ld=lld"
fi

if [[ ! -f "$MESA_BUILD/src/compiler/air/libair.a" ]]; then
	ninja -C "$MESA_BUILD" src/compiler/air/libair.a
fi

MESA_DEFS=$(grep -m1 -A4 '^build src/freedreno/ir3/libfreedreno_ir3.a.p/ir3.c.o:' "$MESA_BUILD/build.ninja" \
	| grep -m1 ' ARGS = ' | tr ' ' '\n' | grep '^-D' | tr '\n' ' ')
if [[ -z "$MESA_DEFS" ]]; then
	echo "build-hakios.sh: failed to extract MESA_DEFS from $MESA_BUILD/build.ninja" >&2
	exit 1
fi

MESA_INC=(
	-isystem "$MESA_SRC/src"
	-isystem "$MESA_SRC/include"
	-isystem "$MESA_SRC/src/freedreno"
	-isystem "$MESA_SRC/src/freedreno/common"
	-isystem "$MESA_SRC/src/compiler"
	-isystem "$MESA_SRC/src/compiler/nir"
	-isystem "$MESA_SRC/src/compiler/air"
	-isystem "$MESA_SRC/src/gallium/include"
	-isystem "$MESA_BUILD/src"
	-isystem "$MESA_BUILD/src/compiler"
	-isystem "$MESA_BUILD/src/compiler/nir"
	-isystem "$MESA_BUILD/src/freedreno"
	-isystem "$MESA_BUILD/src/freedreno/common"
	-isystem "$MESA_BUILD/src/freedreno/isa"
)

MESA_ARCHIVES=(
	"$MESA_BUILD/src/compiler/air/libair.a"
	"$MESA_BUILD/src/freedreno/ir3/libfreedreno_ir3.a"
	"$MESA_BUILD/src/freedreno/isa/libir3encode.a"
	"$MESA_BUILD/src/freedreno/isa/libir3decode.a"
	"$MESA_BUILD/src/compiler/isaspec/libisaspec.a"
	"$MESA_BUILD/src/compiler/nir/libnir.a"
	"$MESA_BUILD/src/compiler/libcompiler.a"
	"$MESA_BUILD/src/freedreno/fdl/libfreedreno_layout.a"
	"$MESA_BUILD/src/freedreno/common/libfreedreno_common.a"
	"$MESA_BUILD/src/util/libmesa_util.a"
	"$MESA_BUILD/src/util/blake3/libblake3.a"
	"$MESA_BUILD/src/c11/impl/libmesa_util_c11.a"
)
[[ -f "$MESA_BUILD/src/util/libmesa_util_sse41.a" ]] && MESA_ARCHIVES+=("$MESA_BUILD/src/util/libmesa_util_sse41.a")

CACHE_BUILD=$(cat "$REPO/hakios-metal/src/compiler.cpp" "$REPO"/hakios-metal/include/hakios-metal/*.hpp "${MESA_ARCHIVES[@]}" 2>/dev/null \
	| (sha256sum 2>/dev/null || shasum -a 256) | cut -c1-24)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

HM_SRCS=(
	pm4.cpp gpu.cpp command.cpp compute.cpp texture.cpp render.cpp
	link.cpp compiler.cpp metallib.cpp c_api.cpp fake_gpu.cpp iokit_transport.cpp
)

echo "[HakiosMetal] Compiling hakios-metal C++17 core ($ARCH)..."
for s in "${HM_SRCS[@]}"; do
	# shellcheck disable=SC2086
	"$CLANGXX_BIN" -target "${ARCH}-apple-ios${IOS_VER}" -isysroot "$SDK" ${EXTRA_FLAGS:-} \
		-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-invalid-constexpr \
		-D_DARWIN_C_SOURCE -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
		-DHAKIOS_METAL_MESA -DHAKIOS_METAL_CACHE_BUILD="\"$CACHE_BUILD\"" \
		$MESA_DEFS "${MESA_INC[@]}" \
		-I"$REPO/hakios-metal/include" \
		-c "$REPO/hakios-metal/src/$s" -o "$WORK/${s%.cpp}.o"
done

if [[ "$(uname -s)" == "Darwin" ]]; then
	"$AR_BIN" rcs "$WORK/libhakios-metal.a" "$WORK"/*.o
else
	"$AR_BIN" --format=darwin rcs "$WORK/libhakios-metal.a" "$WORK"/*.o
fi

echo "[HakiosMetal] Compiling HakiosMetalCA.m ($ARCH, non-ARC)..."
# shellcheck disable=SC2086
"$CLANG_BIN" -target "${ARCH}-apple-ios${IOS_VER}" -isysroot "$SDK" ${EXTRA_FLAGS:-} \
	-O2 -Wall -Wno-deprecated-declarations -Wno-unused-parameter -Wno-unused-function \
	-DHAKIOS_METAL_GPU -I"$REPO/hakios-metal/include" \
	-c "$REPO/userland/src/HakiosMetalCA.m" -o "$WORK/HakiosMetalCA.o"

BUNDLE="$OUT/HakiosMetal.bundle"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"

cat > "$BUNDLE/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key>
	<string>com.hakios.metal</string>
	<key>CFBundleName</key>
	<string>HakiosMetal</string>
	<key>CFBundleExecutable</key>
	<string>HakiosMetal</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleVersion</key>
	<string>2.3.4</string>
	<key>CFBundleSupportedPlatforms</key>
	<array>
		<string>iPhoneOS</string>
	</array>
	<key>NSPrincipalClass</key>
	<string>HakiosMetalDevice</string>
</dict>
</plist>
PLIST

echo "[HakiosMetal] Linking HakiosMetal.bundle..."
# Put metal/tbd/Metal.tbd alongside SDK frameworks so private _MTL* class stubs resolve cleanly even on public SDKs
# shellcheck disable=SC2086
"$CLANGXX_BIN" -target "${ARCH}-apple-ios${IOS_VER}" -isysroot "$SDK" ${EXTRA_FLAGS:-} $LD_EXTRA \
	-bundle -Wl,-undefined,dynamic_lookup -o "$BUNDLE/HakiosMetal" \
	"$WORK/HakiosMetalCA.o" "$WORK/libhakios-metal.a" "${MESA_ARCHIVES[@]}" \
	"$HERE/tbd/Metal.tbd" \
	-framework CoreFoundation -framework Foundation -framework IOKit -framework IOSurface -framework Metal \
	-lobjc -lz -lm

if command -v codesign >/dev/null 2>&1; then
	codesign --force --sign - "$BUNDLE"
elif command -v ldid >/dev/null 2>&1; then
	ldid -S "$BUNDLE/HakiosMetal"
fi

if [[ "$ARCH" == "arm64e" && -f "$REPO/tools/ipados26_depac.py" ]]; then
	python3 "$REPO/tools/ipados26_depac.py" --rewrite "$BUNDLE/HakiosMetal"
fi

ls -lh "$BUNDLE/HakiosMetal" "$BUNDLE/Info.plist"
