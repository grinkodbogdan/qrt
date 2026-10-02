VERSION=1.0.2
fetch() { fetch_git https://github.com/google/woff2 v$VERSION; }
build() {
    cmake_build . -DCANONICAL_PREFIXES=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_STANDARD_LIBRARIES=-lbrotlicommon
    # static: the decoder needs the common library too
    sed -i "s/^Libs: \(.*\) -lwoff2dec$/Libs: \1 -lwoff2dec -lwoff2common/" "$SYS/usr/lib/pkgconfig/libwoff2dec.pc"
}
