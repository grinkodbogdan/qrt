VERSION=2.17.1
fetch() { fetch_tar "http://archive.ubuntu.com/ubuntu/pool/main/f/fontconfig/fontconfig_${VERSION}.orig.tar.gz"; }
build() { meson_build -Dtests=disabled -Dtools=disabled -Ddoc=disabled -Dcache-build=disabled -Dnls=disabled -Dxml-backend=expat --sysconfdir=/etc --localstatedir=/var; }
