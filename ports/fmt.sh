VERSION=12.2.0
fetch() { fetch_git https://github.com/fmtlib/fmt $VERSION; }
build() { cmake_build . -DFMT_DOC=OFF -DFMT_TEST=OFF; }
