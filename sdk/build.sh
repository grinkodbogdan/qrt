#!/bin/sh
# sdk/build.sh [OUT] - the QRT SDK: C library, libqrt and the qrt-cc compiler driver.
#
#   OUT/sysroot/usr/include, usr/lib   musl 1.2.6 built for QRT's native system calls,
#                                      libqrt (windows, input, drawing), qrt.h
#   OUT/bin/qrt-cc                     clang for native QRT programs: qrt-cc -O2 -o app app.c -lqrt
#
# A program built this way carries a "QRT" ELF note (put into crt1.o here), so the
# kernel runs it as a native QRT program with QRT's own system-call numbers
# (sdk/syscalls.txt) - not as a Linux binary.  Needs clang, ld.lld, llvm-ar, python3 (PIL).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
out=${1:-$root/build/sdk}
mkdir -p "$out/src" "$out/bin"
out=$(cd "$out" && pwd)
sys="$out/sysroot"
CC="clang --target=x86_64-linux-musl"

# 1. musl, renumbered
rm -rf "$out/src/musl"
tar -xzf "$here/musl-1.2.6.tar.gz" -C "$out/src"
python3 "$here/patch_musl.py" "$here/syscalls.txt" "$out/src/musl"
( cd "$out/src/musl" &&
  CC="$CC" AR=llvm-ar RANLIB=llvm-ranlib ./configure --target=x86_64 --prefix=/usr --syslibdir=/lib \
      --disable-shared CFLAGS="-O2 -g0" >/dev/null &&
  make -j"$(nproc)" >/dev/null &&
  make DESTDIR="$sys" install >/dev/null &&
  cp syscall.public.h "$sys/usr/include/bits/syscall.h" )

# 2. the QRT note, linked into crt1.o so that every program has it
cat > "$out/src/qrtnote.S" <<'EOF'
    .section .note.qrt, "a", @note
    .p2align 2
    .long 4             /* name size */
    .long 4             /* descriptor size */
    .long 1             /* type: QRT native program */
    .asciz "QRT"
    .long 1             /* ABI version */
EOF
$CC -c "$out/src/qrtnote.S" -o "$out/src/qrtnote.o"
for crt in crt1.o rcrt1.o Scrt1.o; do
    [ -f "$sys/usr/lib/$crt" ] || continue
    ld.lld -r "$sys/usr/lib/$crt" "$out/src/qrtnote.o" -o "$out/src/$crt"
    cp "$out/src/$crt" "$sys/usr/lib/$crt"
done

# 3. compiler runtime: libgcc's builtins (no unwinder needed for C)
cp "$(gcc -print-libgcc-file-name)" "$sys/usr/lib/libgcc.a"
llvm-ar rcs "$sys/usr/lib/libgcc_eh.a"

# 4. libqrt
python3 "$here/libqrt/genfont.py" "$root/assets/Inter-Regular.ttf" "$out/src/qrt_font.c"
cp "$here/libqrt/qrt.h" "$sys/usr/include/qrt.h"
$CC --sysroot="$sys" -O2 -Wall -c "$here/libqrt/qrt.c" -o "$out/src/qrt.o"
$CC --sysroot="$sys" -O2 -c "$out/src/qrt_font.c" -o "$out/src/qrt_font.o"
llvm-ar rcs "$sys/usr/lib/libqrt.a" "$out/src/qrt.o" "$out/src/qrt_font.o"

# Rust (optional): the x86_64-unknown-linux-musl target, linked by qrt-cc against this
# musl instead of Rust's own copy; its libunwind comes along
rlib="$(rustc --print sysroot 2>/dev/null)/lib/rustlib/x86_64-unknown-linux-musl/lib/self-contained"
[ -f "$rlib/libunwind.a" ] && cp "$rlib/libunwind.a" "$sys/usr/lib/"

# 5. the compiler driver
cat > "$out/bin/qrt-cc" <<EOF
#!/bin/sh
# qrt-cc: clang for native QRT programs (static, musl for QRT, libqrt with -lqrt)
exec clang --target=x86_64-linux-musl --sysroot="$sys" -static -fuse-ld=lld --rtlib=libgcc -L"$sys/usr/lib" "\$@"
EOF
chmod +x "$out/bin/qrt-cc"
cat > "$out/bin/qrt-cargo" <<EOF
#!/bin/sh
# qrt-cargo: cargo for native QRT programs (rustup target add x86_64-unknown-linux-musl)
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER="$out/bin/qrt-cc"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_RUSTFLAGS="-C link-self-contained=no -C target-feature=+crt-static"
exec cargo "\$@" --target x86_64-unknown-linux-musl
EOF
chmod +x "$out/bin/qrt-cargo"
echo "QRT SDK in $out (qrt-cc: $out/bin/qrt-cc)"
