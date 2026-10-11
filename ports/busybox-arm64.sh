#!/bin/sh
# ports/busybox-arm64.sh OUT - BusyBox 1.36.1 for QRT on ARM: built from the unmodified
# upstream source (Ubuntu's orig tarball) with the ARM SDK's musl, as a static PIE (a
# program at a fixed address, as distributions build it, cannot load at QRT's ARM user
# addresses).  defconfig, without tc (it needs kernel headers musl lacks), applets run by name.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
out=$1
case $out in /*) ;; *) out=$PWD/$out ;; esac
src=$root/build/arm64/bb
tar=$root/build/ports/dl/busybox_1.36.1.orig.tar.bz2
mkdir -p "$src" "$(dirname "$tar")"
[ -f "$tar" ] || curl -sSfL -o "$tar" http://archive.ubuntu.com/ubuntu/pool/main/b/busybox/busybox_1.36.1.orig.tar.bz2
[ -d "$src/busybox-1.36.1" ] || tar xjf "$tar" -C "$src"
cd "$src/busybox-1.36.1"
cc="$root/build/sdk-arm64/bin/qrt-a64-cc"
[ -f .config ] || {
    make -s HOSTCC=gcc CC="$cc" defconfig >/dev/null
    # tc off; the shell runs applets without links in /bin (as Ubuntu's busybox-static)
    sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/; s/^CONFIG_FEATURE_TC_INGRESS=y//;
            s/^# CONFIG_FEATURE_PREFER_APPLETS is not set/CONFIG_FEATURE_PREFER_APPLETS=y/;
            s/^# CONFIG_FEATURE_SH_STANDALONE is not set/CONFIG_FEATURE_SH_STANDALONE=y/' .config
}
make -s -j"$(nproc)" HOSTCC=gcc CC="$cc" AR=llvm-ar STRIP=llvm-strip busybox 2>/dev/null
mkdir -p "$(dirname "$out")"
cp busybox "$out"
