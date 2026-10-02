VERSION=1.3.0
fetch() { fetch_git https://github.com/libtom/libtommath v$VERSION; }
build() {
    # mp_set_double needs __STDC_IEC_559__, which only glibc predefines; musl on x86-64 is IEC 559 too
    CFLAGS="$CFLAGS -D__STDC_IEC_559__=1" cmake_build . -DBUILD_TESTING=OFF
    # its CMake build installs no .pc for a static build; Ladybird looks for one
    cat > "$SYS/usr/lib/pkgconfig/libtommath.pc" <<PC
prefix=/usr
Name: libtommath
Description: multiple-precision integer library
Version: $VERSION
Cflags: -I\${prefix}/include
Libs: -L\${prefix}/lib -ltommath
PC
}
