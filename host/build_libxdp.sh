#!/bin/bash
# build_libxdp.sh -- one-time build of libxdp + a modern libbpf for the AF_XDP recorder.
#
# Why this is needed: Ubuntu jammy has no libxdp-dev, and its system libbpf (0.5) cannot
# set the XDP frags flag that MTU-9000 multi-buffer capture requires. We build xdp-tools
# v1.5.8 with its BUNDLED modern libbpf, static, under ./xdpbuild. FORCE_SUBDIR_LIBBPF=1
# is ESSENTIAL (otherwise configure links the crippled system libbpf and frags break).
#
# After this succeeds, `make` auto-detects xdpbuild/xdp-tools-1.5.8/lib/libxdp/libxdp.a
# and compiles the recorder WITH the kernel-bypass backend (-DHAVE_XDP).
#
# Run from host/:  ./build_libxdp.sh     (uses sudo for the apt build-deps only)
set -eu

VER=1.5.8
DIR="xdpbuild"
SRC="$DIR/xdp-tools-$VER"
TARBALL="xdp-tools-$VER.tar.gz"
URL="https://github.com/xdp-project/xdp-tools/releases/download/v$VER/$TARBALL"

cd "$(dirname "$0")"

if [ -f "$SRC/lib/libxdp/libxdp.a" ]; then
    echo "libxdp already built at $SRC/lib/libxdp/libxdp.a -- nothing to do."
    exit 0
fi

echo "== installing build dependencies (sudo apt) =="
sudo apt-get update -y
# dpkg can be left half-configured on some hosts; make apt resilient
sudo dpkg --configure -a || true
sudo apt-get install -y clang llvm gcc make m4 pkg-config \
     libelf-dev zlib1g-dev libbpf-dev libpcap-dev

mkdir -p "$DIR"
if [ ! -f "$DIR/$TARBALL" ]; then
    echo "== downloading $URL =="
    curl -fL "$URL" -o "$DIR/$TARBALL"
fi
if [ ! -d "$SRC" ]; then
    echo "== extracting $TARBALL =="
    tar -xzf "$DIR/$TARBALL" -C "$DIR"
fi

echo "== configuring + building (FORCE_SUBDIR_LIBBPF=1) =="
cd "$SRC"
FORCE_SUBDIR_LIBBPF=1 ./configure
FORCE_SUBDIR_LIBBPF=1 make -j"$(nproc)" libxdp xdp-loader

echo
if [ -f lib/libxdp/libxdp.a ]; then
    echo "OK: libxdp.a built. Now run 'make' in host/ to build vdif_recorder with AF_XDP."
else
    echo "ERROR: libxdp.a not produced -- check the build output above." >&2
    exit 1
fi
