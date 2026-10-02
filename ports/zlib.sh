VERSION=1.3.1
fetch() { fetch_git https://github.com/madler/zlib v$VERSION; }
build() { cmake_build . -DZLIB_BUILD_EXAMPLES=OFF && rm -f "$SYS"/usr/lib/libz.so*; }
