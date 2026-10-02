VERSION=10.2.0
fetch() { fetch_git https://github.com/harfbuzz/harfbuzz $VERSION; }
build() { meson_build -Dicu=enabled -Dfreetype=enabled -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dchafa=disabled -Dtests=disabled -Ddocs=disabled -Dutilities=disabled -Dbenchmark=disabled -Dintrospection=disabled; }
