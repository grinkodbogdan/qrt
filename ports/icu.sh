VERSION=78.3
fetch() { fetch_tar https://github.com/unicode-org/icu/releases/download/release-$VERSION/icu4c-$VERSION-sources.tgz; }
build() {
    # ICU's data tools run on the build machine: a host build first, then the cross build
    mkdir -p "$B/host" "$B/cross"
    (cd "$B/host" && CC=gcc CXX=g++ CFLAGS=-O2 CXXFLAGS=-O2 LDFLAGS= "$S/source/configure" --disable-tests --disable-samples --disable-extras >/dev/null && make -j"$JOBS" >/dev/null)
    cd "$B/cross"
    "$S/source/configure" --host=$TARGET --build=x86_64-linux-gnu --prefix=/usr --libdir=/usr/lib \
        --with-cross-build="$B/host" --enable-static --disable-shared --disable-tests --disable-samples --disable-extras \
        --disable-tools --with-data-packaging=static >/dev/null
    make -j"$JOBS" >/dev/null
    make DESTDIR="$SYS" install >/dev/null
}
