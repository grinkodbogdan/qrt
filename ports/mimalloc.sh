VERSION=2.2.7
fetch() { fetch_git https://github.com/microsoft/mimalloc v$VERSION; }
build() { cmake_build . -DMI_SECURE=ON -DMI_BUILD_SHARED=OFF -DMI_BUILD_TESTS=OFF -DMI_BUILD_OBJECT=OFF -DMI_OVERRIDE=OFF -DMI_LIBC_MUSL=ON -DMI_INSTALL_TOPLEVEL=ON; ln -sf libmimalloc-secure.a "$SYS/usr/lib/libmimalloc.a"; }
