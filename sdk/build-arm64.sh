#!/bin/sh
# sdk/build-arm64.sh [OUT] - the SDK for 64-bit ARM programs (the Mi A1): musl 1.2.6
# for AArch64 and the qrt-a64-cc compiler driver.
#
#   OUT/sysroot/usr/include, usr/lib   musl, Linux's UAPI headers
#   OUT/bin/qrt-a64-cc                 clang: static PIE programs for QRT on ARM
#
# Programs use Linux's AArch64 system calls; the kernel's Linux layer runs them (src/user,
# src/arch/arm64/uentry.c).  Needs clang, ld.lld, llvm-ar and an AArch64 libgcc
# (gcc-aarch64-linux-gnu) for the compiler runtime.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
out=${1:-$root/build/sdk-arm64}
mkdir -p "$out/src" "$out/bin"
out=$(cd "$out" && pwd)
sys="$out/sysroot"
CC="clang --target=aarch64-linux-musl"

rm -rf "$out/src/musl"
tar -xzf "$here/musl-1.2.6.tar.gz" -C "$out/src"
( cd "$out/src/musl" &&
  CC="$CC" AR=llvm-ar RANLIB=llvm-ranlib ./configure --target=aarch64 --prefix=/usr --syslibdir=/lib \
      --disable-shared CFLAGS="-O2 -g0" >/dev/null &&
  make -j"$(nproc)" >/dev/null &&
  make DESTDIR="$sys" install >/dev/null )
for d in linux asm-generic mtd; do cp -r "/usr/include/$d" "$sys/usr/include/"; done
cp -r /usr/aarch64-linux-gnu/include/asm "$sys/usr/include/asm"
gcclib=$(dirname "$(aarch64-linux-gnu-gcc -print-libgcc-file-name)")
cp "$gcclib/libgcc.a" "$sys/usr/lib/libgcc.a"
cp "$gcclib/crtbeginS.o" "$gcclib/crtendS.o" "$gcclib/crtbegin.o" "$gcclib/crtend.o" "$gcclib/crtbeginT.o" "$sys/usr/lib/"
mkdir -p "$out/nogcc"                     # clang must not take the host's (glibc) AArch64 GCC start files
llvm-ar rcs "$sys/usr/lib/libgcc_eh.a"
cat > "$out/bin/qrt-a64-cc" <<EOS
#!/bin/sh
# qrt-a64-cc: clang for 64-bit ARM programs on QRT (static PIE, musl)
link="-static-pie"
for a in "\$@"; do [ "\$a" = "-r" ] && link=""; done       # a partial link (-r) is not a program
exec clang --target=aarch64-linux-musl --sysroot="$sys" --gcc-toolchain="$out/nogcc" -nostdlibinc -isystem "$sys/usr/include" \\
    \$link -fPIE -fuse-ld=lld --rtlib=libgcc -L"$sys/usr/lib" "\$@"
EOS
chmod +x "$out/bin/qrt-a64-cc"
touch "$out/.stamp"
echo "QRT SDK for ARM in $out (qrt-a64-cc: $out/bin/qrt-a64-cc)"
