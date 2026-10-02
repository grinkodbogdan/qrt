#!/bin/bash
# ports/ladybird/build.sh [target...] - Ladybird (github.com/LadybirdBrowser/ladybird) built
# as native QRT programs: the commit below plus the patches in this directory, configured
# with the SDK's toolchain file against the libraries ports/build.sh put in the sysroot.
# Targets default to "js"; the programs land in build/ladybird/bin.
#
# Needs: the SDK (sdk/build.sh, sdk/build-cxx.sh, sdk/build-hostlib.sh), the ports
# (ports/build.sh), CMake >= 3.30, Ninja, Python 3, and rustup (Ladybird's
# rust-toolchain.toml picks Rust 1.98) with the x86_64-unknown-linux-musl target.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
COMMIT=965300c7577a0562fcc5f494b2570abaa912a2b6        # October 2026
SRC=${LADYBIRD_SRC:-$root/build/ladybird-src}
OUT=$root/build/ladybird
SDK=$root/build/sdk

[ -f "$SDK/hostlib/libc.a" ] || sh "$root/sdk/build-hostlib.sh" "$SDK"
if [ ! -f "$SRC/.qrt-patched" ]; then
    rm -rf "$SRC"; git init -q "$SRC"
    git -C "$SRC" fetch -q --depth 1 https://github.com/LadybirdBrowser/ladybird "$COMMIT"
    git -C "$SRC" -c advice.detachedHead=false checkout -q FETCH_HEAD
    git -C "$SRC" -c user.name=qrt -c user.email=qrt@localhost am -q "$here"/*.patch
    touch "$SRC/.qrt-patched"
fi
(cd "$SRC" && rustup target add x86_64-unknown-linux-musl >/dev/null)

# static; UI/QRT is the front-end (the browser and its helper processes in one program), Rust crates for QRT's musl target
cmake -G Ninja -S "$SRC" -B "$OUT" -DCMAKE_TOOLCHAIN_FILE="$SDK/qrt.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DENABLE_GUI_TARGETS=ON -DENABLE_LTO_FOR_RELEASE=OFF -DENABLE_LAGOM_CCACHE=OFF \
    -DLADYBIRD_ENABLE_CPPTRACE=OFF -DRUST_TARGET_TRIPLE=x86_64-unknown-linux-musl \
    -DLADYBIRD_CACHE_DIR="$root/build/ladybird-cache" >"$OUT.log" 2>&1 || { tail -30 "$OUT.log"; exit 1; }
ninja -C "$OUT" -k 0 -j"$(nproc)" "${@:-js}"
