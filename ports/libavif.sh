VERSION=1.4.2
fetch() { fetch_git https://github.com/AOMediaCodec/libavif v$VERSION; }
build() { cmake_build . -DAVIF_CODEC_DAV1D=SYSTEM -DAVIF_LIBYUV=OFF -DAVIF_BUILD_APPS=OFF -DAVIF_BUILD_TESTS=OFF -DAVIF_LIBSHARPYUV=OFF -DVCPKG_TARGET_TRIPLET=x64-qrt; }  # the triplet: install its CMake config
