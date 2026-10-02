VERSION=2.6.4
fetch() { fetch_git https://github.com/libexpat/libexpat R_${VERSION//./_}; }
build() { cmake_build expat -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_SHARED_LIBS=OFF; }
