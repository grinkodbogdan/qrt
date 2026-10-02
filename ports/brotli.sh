VERSION=1.2.0
fetch() { fetch_git https://github.com/google/brotli v$VERSION; }
build() { cmake_build . -DBROTLI_DISABLE_TESTS=ON; }
