#!/bin/bash
# ports/ladybird/build.sh [target...] - Ladybird (github.com/LadybirdBrowser/ladybird) built
# as native QRT programs: the commit below plus the patches in this directory, configured
# with the SDK's toolchain file against the libraries ports/build.sh put in the sysroot.
# Targets default to "js"; the programs land in build/ladybird/bin.
#
# Needs: the SDK (sdk/build.sh, sdk/build-cxx.sh, sdk/build-hostlib.sh), the ports
# (ports/build.sh), CMake >= 3.30, Ninja, Python 3, and rustup (Ladybird's
# rust-toolchain.toml picks Rust 1.98) with the x86_64-unknown-linux-musl target.
#
# QRT_ARCH=arm64: for 64-bit ARM (the Mi A1) - the ARM SDK (sdk/build-arm64.sh,
# sdk/build-arm64-cxx.sh), its ports (QRT_ARCH=arm64 ports/build.sh), Rust's
# aarch64-unknown-linux-musl target, into build/ladybird-arm64.  The one program the build
# runs that is built for the target (LibJS's layout generator) runs under qemu-aarch64.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
COMMIT=965300c7577a0562fcc5f494b2570abaa912a2b6        # October 2026
SRC=${LADYBIRD_SRC:-$root/build/ladybird-src}
if [ "${QRT_ARCH:-x64}" = arm64 ]; then
    OUT=$root/build/ladybird-arm64
    SDK=$root/build/sdk-arm64
    TOOLCHAIN=$SDK/qrt-a64.cmake
    RUST_TARGET=aarch64-unknown-linux-musl
    EXTRA="-DCMAKE_CROSSCOMPILING_EMULATOR=$(command -v qemu-aarch64-static || command -v qemu-aarch64)"
    # Rust programs the build runs (flapc, the bytecode generator) are built for ARM too:
    # the kernel runs them through qemu-aarch64 (binfmt_misc, from qemu-user-static)
    if [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
        [ -e /proc/sys/fs/binfmt_misc/register ] || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
        cat /usr/lib/binfmt.d/qemu-aarch64.conf > /proc/sys/fs/binfmt_misc/register
    fi
    # rustc links ARM programs with clang (CMake's C compiler): lld, not the host's ld
    export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_RUSTFLAGS="-C link-arg=-fuse-ld=lld"
else
    OUT=$root/build/ladybird
    SDK=$root/build/sdk
    TOOLCHAIN=$SDK/qrt.cmake
    RUST_TARGET=x86_64-unknown-linux-musl
    EXTRA=""
    [ -f "$SDK/hostlib/libc.a" ] || sh "$root/sdk/build-hostlib.sh" "$SDK"
fi
if [ ! -f "$SRC/.qrt-patched" ]; then
    rm -rf "$SRC"; git init -q "$SRC"
    git -C "$SRC" fetch -q --depth 1 https://github.com/LadybirdBrowser/ladybird "$COMMIT"
    git -C "$SRC" -c advice.detachedHead=false checkout -q FETCH_HEAD
    git -C "$SRC" -c user.name=qrt -c user.email=qrt@localhost am -q "$here"/*.patch
    touch "$SRC/.qrt-patched"
fi
(cd "$SRC" && rustup target add $RUST_TARGET >/dev/null)

# static; UI/QRT is the front-end (the browser and its helper processes in one program), Rust crates for QRT's musl target
cmake -G Ninja -S "$SRC" -B "$OUT" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release $EXTRA \
    -DBUILD_SHARED_LIBS=OFF -DENABLE_GUI_TARGETS=ON -DENABLE_LTO_FOR_RELEASE=OFF -DENABLE_LAGOM_CCACHE=OFF \
    -DLADYBIRD_ENABLE_CPPTRACE=OFF -DRUST_TARGET_TRIPLE=$RUST_TARGET \
    -DLADYBIRD_CACHE_DIR="$root/build/ladybird-cache" >"$OUT.log" 2>&1 || { tail -30 "$OUT.log"; exit 1; }
ninja -C "$OUT" -k 0 -j"$(nproc)" "${@:-js}"
