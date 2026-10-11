#!/bin/sh
# sdk/build-arm64-cxx.sh [SDK] - C++ for 64-bit ARM programs on QRT: LLVM 20's compiler-rt
# (builtins, crtbegin/crtend), libunwind, libc++abi and libc++, static, for the ARM SDK's
# musl (as sdk/build-cxx.sh does for x86-64); then qrt-a64-c++, and the toolchain file
# (SDK/qrt-a64.cmake) switched to compiler-rt and libc++.
#
# Needs: the ARM C SDK (sdk/build-arm64.sh), clang-20/lld-20, CMake, ninja; LLVM's runtime
# sources (shared with the x86-64 SDK: build/sdk/src/llvm, fetched by sdk/build-cxx.sh).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
sdk=${1:-$root/build/sdk-arm64}
sdk=$(cd "$sdk" && pwd)
sys="$sdk/sysroot"
src="$root/build/sdk/src/llvm"
T=aarch64-linux-musl
[ -d "$src/runtimes" ] || { echo "LLVM's runtimes are not in $src (run sdk/build-cxx.sh once)"; exit 1; }

b="$sdk/rt"
rm -rf "$b"
cmake -G Ninja -S "$src/runtimes" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$sdk/qrt-a64.cmake" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_CXX_FLAGS_INIT= -DCMAKE_EXE_LINKER_FLAGS_INIT= \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$sys/usr" \
    -DLLVM_ENABLE_RUNTIMES="compiler-rt;libunwind;libcxxabi;libcxx" \
    -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_CRT=ON -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
    -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF -DCOMPILER_RT_BUILD_PROFILE=OFF \
    -DCOMPILER_RT_BUILD_MEMPROF=OFF -DCOMPILER_RT_BUILD_ORC=OFF -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF \
    -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_INCLUDE_TESTS=OFF \
    -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_USE_COMPILER_RT=ON -DLIBUNWIND_INCLUDE_TESTS=OFF \
    -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_USE_COMPILER_RT=ON \
    -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXX_CXX_ABI=libcxxabi \
    -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF >"$b.log" 2>&1 \
    || { tail -30 "$b.log"; exit 1; }
ninja -C "$b" >>"$b.log" 2>&1 || { grep -E "error|FAILED" "$b.log" | head; exit 1; }
ninja -C "$b" install >>"$b.log" 2>&1

# clang-20 finds a target's compiler-rt in its own resource directory
rtdir="$(clang-20 -print-resource-dir)/lib/aarch64-unknown-linux-musl"
mkdir -p "$rtdir"
for f in "$sys"/usr/lib/linux/libclang_rt.builtins-aarch64.a "$sys"/usr/lib/linux/clang_rt.crtbegin-aarch64.o "$sys"/usr/lib/linux/clang_rt.crtend-aarch64.o; do
    [ -f "$f" ] || continue
    cp "$f" "$rtdir/$(basename "$f" | sed 's/-aarch64//')"
done

# the toolchain file: compiler-rt, libunwind and libc++ from here on
sed -i 's|^set(CMAKE_EXE_LINKER_FLAGS_INIT .*|set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-pie -fuse-ld=lld -rtlib=compiler-rt -unwindlib=libunwind -stdlib=libc++")|' "$sdk/qrt-a64.cmake"
cat > "$sdk/bin/qrt-a64-c++" <<EOS
#!/bin/sh
# qrt-a64-c++: clang++ for 64-bit ARM programs on QRT (C++23, libc++, static PIE)
link="-static-pie"
for a in "\$@"; do [ "\$a" = "-r" ] && link=""; done
exec clang++-20 --target=$T --sysroot="$sys" --gcc-toolchain="$sdk/nogcc" -D__QRT__ -fPIE \$link -fuse-ld=lld \\
    -rtlib=compiler-rt -unwindlib=libunwind -stdlib=libc++ "\$@"
EOS
chmod +x "$sdk/bin/qrt-a64-c++"
touch "$sdk/.cxxstamp"
echo "QRT C++ SDK for ARM: $sdk/bin/qrt-a64-c++, toolchain file $sdk/qrt-a64.cmake"
