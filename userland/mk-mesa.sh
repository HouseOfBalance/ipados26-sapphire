#!/bin/bash
# Mesa for sapphire's iOS: the OpenGL ES library behind the EAGL bridge, with freedreno for the
# Adreno 610 (through the SapphireGPU service, xnu patch 0149) and softpipe as a fallback.
# HackiOS's tools/mk-mesa.sh (userland/hackios/README.md), made to build on Linux with LLVM 18:
# clang -target arm64-apple-ios16.0 and ld64.lld against an iPhoneOS 16.5 SDK (theos/sdks, kept
# outside the repository: SDK=...). Out: $B/out/libOSMesa.8.dylib, exported-gl.txt, gl.xml.
set -e
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
B=${B:-$HOME/mesa-ios}
ARCH=${ARCH:-arm64}
IOS_VER=${IOS_VER:-16.0}
if [ "$(uname -s)" = "Darwin" ]; then
	CLANG_BIN=${CLANG:-$(xcrun -find clang)}
	CLANGXX_BIN=${CLANGXX:-$(xcrun -find clang++)}
	AR_BIN=${AR:-$(xcrun -find ar)}
	RANLIB_BIN=${RANLIB:-$(xcrun -find ranlib)}
	STRIP_BIN=${STRIP:-$(xcrun -find strip)}
	INTOOL_BIN=${INSTALL_NAME_TOOL:-$(xcrun -find install_name_tool)}
	NM_BIN=${NM:-$(xcrun -find nm)}
	LINK_ARGS="[]"
else
	LLVM=${LLVM:-/usr/lib/llvm-18/bin}
	export PATH=$LLVM:$HOME/.local/bin:$PATH
	CLANG_BIN=${CLANG:-$LLVM/clang}
	CLANGXX_BIN=${CLANGXX:-$LLVM/clang++}
	AR_BIN=${AR:-$LLVM/llvm-ar}
	RANLIB_BIN=${RANLIB:-$LLVM/llvm-ranlib}
	STRIP_BIN=${STRIP:-$LLVM/llvm-strip}
	INTOOL_BIN=${INSTALL_NAME_TOOL:-$LLVM/llvm-install-name-tool}
	NM_BIN=${NM:-$LLVM/llvm-nm}
	LINK_ARGS="['-fuse-ld=lld']"
fi
mkdir -p "$B/out" && cd "$B"
PATCHES=$(cat "$HERE"/mesa/*.patch | sha256sum | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.sapphire-patches 2>/dev/null)" != "$PATCHES" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf mesa-$VERSION build-*
	tar xJf mesa-$VERSION.tar.xz
	for p in "$HERE"/mesa/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done
	echo "$PATCHES" > mesa-$VERSION/.sapphire-patches
fi
rm -rf mesa-$VERSION/src/redwood-drm mesa-$VERSION/src/compiler/air
cp -r "$HERE/mesa/sapphire-drm" mesa-$VERSION/src/redwood-drm
cp -r "$HERE/mesa/air" mesa-$VERSION/src/compiler/air
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH
T="'-target', '${ARCH}-apple-ios${IOS_VER}', '-isysroot', '$SDK'"
cat > cross-${ARCH}.ini <<INI
[binaries]
c = ['$CLANG_BIN', $T]
cpp = ['$CLANGXX_BIN', $T, '-fuse-cxa-atexit']
objc = ['$CLANG_BIN', $T]
ar = '$AR_BIN'
ranlib = '$RANLIB_BIN'
strip = '$STRIP_BIN'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = '$ARCH'
endian = 'little'

[built-in options]
c_link_args = $LINK_ARGS
cpp_link_args = $LINK_ARGS
objc_link_args = $LINK_ARGS

[properties]
needs_exe_wrapper = true
INI
DRIVERS=${DRIVERS:-softpipe,freedreno}
BUILD=build-${ARCH}-${DRIVERS//,/-}
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross-${ARCH}.ini --wrap-mode=nofallback \
	--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C $BUILD > ninja.log
ninja -C $BUILD src/compiler/air/libair.a >> ninja.log
"$STRIP_BIN" -x -o out/libOSMesa.8.dylib $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
"$INTOOL_BIN" -id /redwood/lib/libOSMesa.8.dylib out/libOSMesa.8.dylib
if [ "$ARCH" = "arm64e" ] && [ -f "$HERE/../tools/ipados26_depac.py" ]; then
	python3 "$HERE/../tools/ipados26_depac.py" --rewrite out/libOSMesa.8.dylib
fi
"$NM_BIN" -gU out/libOSMesa.8.dylib | awk '$NF ~ /^_gl/ { print substr($NF, 2) }' > out/exported-gl.txt
cp mesa-$VERSION/src/mapi/glapi/registry/gl.xml out/gl.xml
echo "mk-mesa.sh: Mesa $VERSION ($ARCH, $DRIVERS): $B/out/libOSMesa.8.dylib, $(wc -c < out/libOSMesa.8.dylib | tr -d ' ') bytes, $(wc -l < out/exported-gl.txt | tr -d ' ') gl* functions"
