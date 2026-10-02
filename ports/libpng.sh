VERSION=1.6.58
fetch() { fetch_git https://github.com/pnggroup/libpng v$VERSION; }
build() { cmake_build . -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF; }
