#!/bin/bash
# ports/build.sh [name...] - third-party libraries for native QRT programs (Ladybird's
# dependencies), cross-built static into the SDK's sysroot.  Each ports/<name>.sh is a
# recipe: it sets VERSION and SRC (a GitHub repository and tag, a GitHub release asset or
# an Ubuntu .orig tarball - the hosts the build machine can reach) and defines build().
# The order is ports/ORDER.  A port that is built is not built again (build/ports/done/).
#
# Needs the C and C++ SDK (sdk/build.sh, sdk/build-cxx.sh).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
SDK=${SDK:-$root/build/sdk}
SYS="$SDK/sysroot"
W="$root/build/ports"
mkdir -p "$W/src" "$W/build" "$W/done" "$W/dl"
JOBS=$(nproc)

# ---- the cross toolchain ----------------------------------------------------------------
TARGET=x86_64-linux-musl
LDSTATIC="-static -fuse-ld=lld -rtlib=compiler-rt -unwindlib=libunwind"
export CC="clang-20 --target=$TARGET --sysroot=$SYS -D__QRT__"
export CXX="clang++-20 --target=$TARGET --sysroot=$SYS -D__QRT__ -stdlib=libc++"
export AR=llvm-ar-20 RANLIB=llvm-ranlib-20 NM=llvm-nm-20 STRIP=llvm-strip-20
export CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC" LDFLAGS="$LDSTATIC -stdlib=libc++"
export PKG_CONFIG_LIBDIR="$SYS/usr/lib/pkgconfig:$SYS/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYS"
export PKG_CONFIG_PATH=""

cat > "$SDK/qrt-meson.ini" <<EOF
[binaries]
c = ['clang-20', '--target=$TARGET', '--sysroot=$SYS', '-D__QRT__']
cpp = ['clang++-20', '--target=$TARGET', '--sysroot=$SYS', '-D__QRT__', '-stdlib=libc++']
ar = 'llvm-ar-20'
strip = 'llvm-strip-20'
pkg-config = 'pkg-config'
[built-in options]
c_link_args = ['-static', '-fuse-ld=lld', '-rtlib=compiler-rt', '-unwindlib=libunwind']
cpp_link_args = ['-static', '-fuse-ld=lld', '-rtlib=compiler-rt', '-unwindlib=libunwind', '-stdlib=libc++']
default_library = 'static'
prefix = '/usr'
libdir = 'lib'
[properties]
sys_root = '$SYS'
pkg_config_libdir = '$SYS/usr/lib/pkgconfig:$SYS/usr/share/pkgconfig'
[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

# ---- helpers recipes use ---------------------------------------------------------------
# fetch_git URL TAG: a shallow clone into $S
fetch_git() {
    [ -d "$S/.git" ] || [ -f "$S/.fetched" ] && return 0
    rm -rf "$S"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$1" "$S"
    touch "$S/.fetched"
}
# fetch_tar URL [strip]: download and unpack into $S
fetch_tar() {
    [ -f "$S/.fetched" ] && return 0
    local f="$W/dl/$(basename "$1")"
    [ -f "$f" ] || curl -sSfL -o "$f" "$1"
    rm -rf "$S"; mkdir -p "$S"
    tar -xf "$f" -C "$S" --strip-components="${2:-1}"
    touch "$S/.fetched"
}
# cmake_build [source subdir] -- args...
cmake_build() {
    local sub="${1:-.}"; shift || true
    rm -rf "$B"
    cmake -G Ninja -S "$S/$sub" -B "$B" -DCMAKE_TOOLCHAIN_FILE="$SDK/qrt.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_TESTING=OFF "$@" >"$B.log" 2>&1 || { tail -30 "$B.log"; return 1; }
    ninja -C "$B" -j"$JOBS" >>"$B.log" 2>&1 || { grep -E "error|FAILED" "$B.log" | head -20; return 1; }
    DESTDIR="$SYS" ninja -C "$B" install >>"$B.log" 2>&1
}
meson_build() {
    rm -rf "$B"
    meson setup "$B" "$S" --cross-file "$SDK/qrt-meson.ini" --buildtype=release "$@" >"$B.log" 2>&1 || { tail -30 "$B.log"; return 1; }
    ninja -C "$B" -j"$JOBS" >>"$B.log" 2>&1 || { grep -E "error|FAILED" "$B.log" | head -20; return 1; }
    DESTDIR="$SYS" ninja -C "$B" install >>"$B.log" 2>&1
}
# autotools_build -- configure args (run in a separate build dir)
autotools_build() {
    rm -rf "$B"; mkdir -p "$B"
    (cd "$B" && "$S/configure" --host=$TARGET --build=x86_64-linux-gnu --prefix=/usr --libdir=/usr/lib \
        --enable-static --disable-shared "$@") >"$B.log" 2>&1 || { tail -30 "$B.log"; return 1; }
    make -C "$B" -j"$JOBS" >>"$B.log" 2>&1 || { grep -E "error" "$B.log" | head -20; return 1; }
    make -C "$B" DESTDIR="$SYS" install >>"$B.log" 2>&1
    find "$SYS/usr/lib" -name '*.la' -delete
}

build_port() {
    local name=$1
    [ -f "$W/done/$name" ] && return 0
    S="$W/src/$name"; B="$W/build/$name"
    unset -f build; VERSION=""
    # shellcheck disable=SC1090
    source "$here/$name.sh"
    printf 'port %-16s %-12s ' "$name" "$VERSION"
    local t0=$SECONDS
    fetch
    set +e
    ( set -e; cd "$W"; build ); local rc=$?
    set -e
    if [ $rc = 0 ]; then
        echo "$VERSION" > "$W/done/$name"
        echo "ok ($((SECONDS - t0)) s)"
        rm -rf "$B"                                   # the build tree is not kept (disk)
    else
        echo "FAILED (log: $B.log)"; exit 1
    fi
}

if [ $# -gt 0 ]; then names="$*"; else names=$(grep -v '^#' "$here/ORDER"); fi
for n in $names; do build_port "$n"; done
