VERSION=3.2.0
fetch() { fetch_git https://github.com/libjpeg-turbo/libjpeg-turbo $VERSION; }
build() { cmake_build . -DENABLE_SHARED=OFF -DWITH_TURBOJPEG=ON -DWITH_SIMD=ON; }
