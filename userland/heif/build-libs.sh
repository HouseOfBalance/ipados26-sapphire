#!/bin/bash
# userland/heif/build-libs.sh: libde265 + libheif (decoding only) as static libraries for iOS/iPadOS
# Supports both macOS (Xcode clang) and Linux (LLVM clang + lld), and ARCH=arm64 or ARCH=arm64e.
# Out: $P/lib/{libde265,libheif}.a, $P/include.
set -euo pipefail
TOP=$(cd "$(dirname "$0")" && pwd)
ARCH=${ARCH:-arm64}
IOS_VER=${IOS_VER:-16.0}
SRCS=${SRCS:-$TOP/../../installer/tools-arm64/heic/dl}
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
B=${BUILD_DIR:-$TOP/build-$ARCH}
P=${PREFIX_DIR:-$TOP/prefix-$ARCH}
mkdir -p "$B" "$P" "$SRCS"

DE265_TAR="$SRCS/libde265-1.0.15.tar.gz"
HEIF_TAR="$SRCS/libheif-1.18.2.tar.gz"
if [[ ! -s "$DE265_TAR" ]]; then
	curl -sL --fail -o "$DE265_TAR" \
		"https://github.com/strukturag/libde265/releases/download/v1.0.15/libde265-1.0.15.tar.gz"
fi
if [[ ! -s "$HEIF_TAR" ]]; then
	curl -sL --fail -o "$HEIF_TAR" \
		"https://github.com/strukturag/libheif/releases/download/v1.18.2/libheif-1.18.2.tar.gz"
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
	CC_BIN="${CLANG:-$(xcrun -find clang)}"
	CXX_BIN="${CLANGXX:-$(xcrun -find clang++)}"
	AR_BIN="${AR:-$(xcrun -find ar)}"
	RANLIB_BIN="${RANLIB:-$(xcrun -find ranlib)}"
	LD_FLAGS_INIT=""
else
	L="${LLVM:-/usr/lib/llvm-18/bin}"
	CC_BIN="${CLANG:-$L/clang}"
	CXX_BIN="${CLANGXX:-$L/clang++}"
	AR_BIN="${AR:-$L/llvm-ar}"
	RANLIB_BIN="${RANLIB:-$L/llvm-ranlib}"
	LD_FLAGS_INIT="-fuse-ld=lld"
fi

TC="$B/ios.cmake"
cat > "$TC" <<EOF
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_OSX_SYSROOT $SDK)
set(CMAKE_OSX_ARCHITECTURES $ARCH)
set(CMAKE_C_COMPILER $CC_BIN)
set(CMAKE_CXX_COMPILER $CXX_BIN)
set(CMAKE_AR $AR_BIN)
set(CMAKE_RANLIB $RANLIB_BIN)
set(CMAKE_C_COMPILER_TARGET ${ARCH}-apple-ios${IOS_VER})
set(CMAKE_CXX_COMPILER_TARGET ${ARCH}-apple-ios${IOS_VER})
set(CMAKE_C_FLAGS_INIT "-target ${ARCH}-apple-ios${IOS_VER} -isysroot $SDK -O2 -fvisibility=hidden")
set(CMAKE_CXX_FLAGS_INIT "-target ${ARCH}-apple-ios${IOS_VER} -isysroot $SDK -O2 -fvisibility=hidden -fvisibility-inlines-hidden -stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "$LD_FLAGS_INIT")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "$LD_FLAGS_INIT")
set(CMAKE_FIND_ROOT_PATH $P)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
EOF

rm -rf "$B/src" && mkdir -p "$B/src"
tar -C "$B/src" -xf "$DE265_TAR"
tar -C "$B/src" -xf "$HEIF_TAR"

cmake -S "$B/src/libde265-1.0.15" -B "$B/de265" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$P" -DBUILD_SHARED_LIBS=OFF -DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON > "$B/de265.log" 2>&1 || { cat "$B/de265.log" >&2; exit 1; }
ninja -C "$B/de265" install >> "$B/de265.log" 2>&1 || { tail -n 80 "$B/de265.log" >&2; exit 1; }

cmake -S "$B/src/libheif-1.18.2" -B "$B/heif" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$P" -DBUILD_SHARED_LIBS=OFF -DWITH_LIBDE265=ON -DWITH_X265=OFF -DWITH_AOM_DECODER=OFF \
	-DWITH_AOM_ENCODER=OFF -DWITH_DAV1D=OFF -DWITH_RAV1E=OFF -DWITH_SvtEnc=OFF -DWITH_JPEG_DECODER=OFF \
	-DWITH_JPEG_ENCODER=OFF -DWITH_OpenJPEG_DECODER=OFF -DWITH_OpenJPEG_ENCODER=OFF -DWITH_FFMPEG_DECODER=OFF \
	-DWITH_KVAZAAR=OFF -DWITH_UVG266=OFF -DWITH_VVDEC=OFF -DWITH_VVENC=OFF -DWITH_EXAMPLES=OFF -DWITH_GDK_PIXBUF=OFF \
	-DWITH_LIBSHARPYUV=OFF -DENABLE_PLUGIN_LOADING=OFF -DBUILD_TESTING=OFF -DWITH_UNCOMPRESSED_CODEC=OFF \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON -DLIBDE265_INCLUDE_DIR="$P/include" -DLIBDE265_LIBRARY="$P/lib/libde265.a" \
	> "$B/heif.log" 2>&1 || { cat "$B/heif.log" >&2; exit 1; }
ninja -C "$B/heif" install >> "$B/heif.log" 2>&1 || { tail -n 80 "$B/heif.log" >&2; exit 1; }

# Keep compatibility symlink at $TOP/prefix when building default
if [[ "$P" != "$TOP/prefix" && ! -e "$TOP/prefix" ]]; then
	ln -s "$(basename "$P")" "$TOP/prefix" || true
fi
ls -la "$P"/lib/*.a
