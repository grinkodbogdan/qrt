#!/bin/sh
# ports/fastfetch-arm64.sh OUT - fastfetch 2.66.0 (MIT) for QRT on ARM, with yyjson 0.12.0
# (MIT), from Ubuntu's orig tarballs (which leave out fastfetch's bundled third-party
# code: yyjson is built on its own, the wide-character table is off).  Static PIE, the
# ARM SDK's toolchain file; the optional libraries fastfetch can use are all off.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
out=$1
case $out in /*) ;; *) out=$PWD/$out ;; esac
sdk=$root/build/sdk-arm64
src=$root/build/arm64/ff
dl=$root/build/ports/dl
mkdir -p "$src" "$dl"
get() { [ -f "$dl/$1" ] || curl -sSfL -o "$dl/$1" "$2/$1"; }
get "yyjson_0.12.0+ds.orig.tar.xz" http://archive.ubuntu.com/ubuntu/pool/universe/y/yyjson
get "fastfetch_2.66.0+dfsg.orig.tar.xz" http://archive.ubuntu.com/ubuntu/pool/universe/f/fastfetch
mkdir -p "$src/yy"
[ -n "$(ls "$src/yy")" ] || tar xJf "$dl/yyjson_0.12.0+ds.orig.tar.xz" -C "$src/yy"
[ -d "$src/fastfetch-2.66.0" ] || tar xJf "$dl/fastfetch_2.66.0+dfsg.orig.tar.xz" -C "$src"
yy=$(ls -d "$src"/yy/*/ | head -1)
cmake -G Ninja -S "$yy" -B "$yy/b" -DCMAKE_TOOLCHAIN_FILE="$sdk/qrt-a64.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr -DYYJSON_BUILD_TESTS=OFF >/dev/null
ninja -C "$yy/b" >/dev/null
DESTDIR="$sdk/sysroot" ninja -C "$yy/b" install >/dev/null
ff="$src/fastfetch-2.66.0"
cmake -G Ninja -S "$ff" -B "$ff/b" -DCMAKE_TOOLCHAIN_FILE="$sdk/qrt-a64.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DIS_MUSL=ON -DENABLE_SYSTEM_YYJSON=ON -DENABLE_WCWIDTH=OFF -DENABLE_LTO=OFF -DBUILD_FLASHFETCH=OFF \
    -DENABLE_LUA=OFF -DENABLE_QUICKJS=OFF -DINSTALL_LICENSE=OFF >/dev/null
ninja -C "$ff/b" fastfetch >/dev/null
mkdir -p "$(dirname "$out")"
llvm-strip -o "$out" "$ff/b/fastfetch"
