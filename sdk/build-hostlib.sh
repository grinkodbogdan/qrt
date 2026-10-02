#!/bin/sh
# sdk/build-hostlib.sh [SDK] - musl 1.2.6 as it comes, with Linux's system-call numbers,
# into SDK/hostlib (crt1.o, crti.o, crtn.o, libc.a).  A program compiled for QRT and linked
# with these instead of the sysroot's (QRT_HOST_LINK_OPTIONS in qrt.cmake: -B and -L in
# front) is a static Linux program: build tools that a project compiles and then runs during
# its own build (Ladybird's generate_interpreter_layout) run on the build machine this way,
# with the target's exact layouts, since the headers and libc++ are the same.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
sdk=${1:-$root/build/sdk}
mkdir -p "$sdk/src" "$sdk/hostlib"
sdk=$(cd "$sdk" && pwd)
src="$sdk/src/musl-host"
rm -rf "$src"; mkdir -p "$src"
tar -xzf "$here/musl-1.2.6.tar.gz" -C "$src" --strip-components=1
( cd "$src" &&
  CC="clang --target=x86_64-linux-musl" AR=llvm-ar RANLIB=llvm-ranlib ./configure --target=x86_64 \
      --prefix=/usr --disable-shared CFLAGS="-O2 -g0" >/dev/null &&
  make -j"$(nproc)" lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o >/dev/null &&
  cp lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o "$sdk/hostlib/" )
rm -rf "$src"
echo "QRT SDK host libc: $sdk/hostlib"
