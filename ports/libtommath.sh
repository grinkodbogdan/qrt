VERSION=1.3.0
fetch() { fetch_git https://github.com/libtom/libtommath v$VERSION; }
build() { cmake_build . -DBUILD_TESTING=OFF; }
