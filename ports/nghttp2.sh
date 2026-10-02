VERSION=1.69.0
fetch() { fetch_git https://github.com/nghttp2/nghttp2 v$VERSION; }
build() { cmake_build . -DENABLE_LIB_ONLY=ON -DBUILD_STATIC_LIBS=ON -DENABLE_DOC=OFF -DBUILD_TESTING=OFF; }
