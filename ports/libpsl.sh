VERSION=0.21.5
fetch() { fetch_tar https://github.com/rockdaboot/libpsl/releases/download/$VERSION/libpsl-$VERSION.tar.gz; }
build() { autotools_build --disable-runtime --enable-builtin --disable-man --disable-gtk-doc; }
