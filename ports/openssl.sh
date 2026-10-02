VERSION=3.6.3
fetch() { fetch_git https://github.com/openssl/openssl openssl-$VERSION; }
build() {
    mkdir -p "$B" && cd "$B"
    # no -static in LDFLAGS here: with it, Configure turns threads off altogether
    LDFLAGS="-fuse-ld=lld" "$S/Configure" linux-x86_64 no-shared no-tests no-docs no-apps enable-thread-pool enable-default-thread-pool --prefix=/usr --libdir=lib --openssldir=/etc/ssl >"$B.log" 2>&1
    make -j"$JOBS" build_libs >>"$B.log" 2>&1
    make DESTDIR="$SYS" install_dev >>"$B.log" 2>&1
}
