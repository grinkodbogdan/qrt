VERSION=3.6.3
fetch() { fetch_git https://github.com/openssl/openssl openssl-$VERSION; }
build() {
    mkdir -p "$B" && cd "$B"
    "$S/Configure" linux-x86_64 no-shared no-tests no-docs no-apps --prefix=/usr --libdir=lib --openssldir=/etc/ssl >"$B.log" 2>&1
    make -j"$JOBS" build_libs >>"$B.log" 2>&1
    make DESTDIR="$SYS" install_dev >>"$B.log" 2>&1
}
