VERSION=1.5.7
fetch() { fetch_git https://github.com/facebook/zstd v$VERSION; }
build() { cmake_build build/cmake -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF; }
