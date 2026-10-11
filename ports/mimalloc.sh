VERSION=2.2.7
# MI_OPT_ARCH off: on ARM it would compile for ARMv8.1 (LSE atomics); the Mi A1's Cortex-A53 is ARMv8.0
fetch() { fetch_git https://github.com/microsoft/mimalloc v$VERSION; }
build() { cmake_build . -DMI_SECURE=ON -DMI_BUILD_SHARED=OFF -DMI_BUILD_TESTS=OFF -DMI_BUILD_OBJECT=OFF -DMI_OVERRIDE=OFF -DMI_LIBC_MUSL=ON -DMI_INSTALL_TOPLEVEL=ON -DMI_NO_OPT_ARCH=ON; ln -sf libmimalloc-secure.a "$SYS/usr/lib/libmimalloc.a"; }
