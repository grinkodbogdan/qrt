VERSION=3.1-20260512
fetch() { fetch_tar "http://archive.ubuntu.com/ubuntu/pool/main/libe/libedit/libedit_${VERSION}.orig.tar.gz"; }
build() { CPPFLAGS="-I$SYS/usr/include/ncursesw -D__STDC_ISO_10646__=201706L" autotools_build --disable-examples; }
