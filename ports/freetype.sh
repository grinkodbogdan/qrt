VERSION=2.13.3
fetch() { fetch_git https://github.com/freetype/freetype VER-${VERSION//./-}; }
build() { cmake_build . -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BZIP2=ON -DFT_REQUIRE_PNG=ON -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_BROTLI=ON; }
