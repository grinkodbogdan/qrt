VERSION=8.2.10
fetch() { fetch_git https://github.com/fastfloat/fast_float v$VERSION; }
build() { cmake_build . -DFASTFLOAT_TEST=OFF; }
