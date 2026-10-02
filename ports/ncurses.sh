VERSION=6.6+20260608
fetch() { fetch_tar "http://archive.ubuntu.com/ubuntu/pool/main/n/ncurses/ncurses_${VERSION}.orig.tar.gz"; }
build() { autotools_build --without-progs --without-tests --without-manpages --without-ada --without-cxx-binding --without-debug --with-build-cc=gcc --enable-widec --disable-stripping --with-termlib --with-fallbacks=xterm-256color,vt100,dumb --disable-database --enable-pc-files --with-pkg-config-libdir=/usr/lib/pkgconfig && ln -sf libtinfow.a "$SYS/usr/lib/libtinfo.a" && ln -sf libncursesw.a "$SYS/usr/lib/libncurses.a"; }
