VERSION=1.6.58
# with the APNG patch (libpng-apng on SourceForge), which Ladybird's image decoders need
fetch() {
    fetch_git https://github.com/pnggroup/libpng v$VERSION
    if [ ! -f "$S/.apng" ]; then
        local f="$W/dl/libpng-$VERSION-apng.patch.gz"
        [ -f "$f" ] || curl -sSfL -o "$f" "https://downloads.sourceforge.net/project/libpng-apng/libpng16/$VERSION/libpng-$VERSION-apng.patch.gz"
        gzip -dc "$f" | patch -s -d "$S" -p1
        touch "$S/.apng"
    fi
}
build() { cmake_build . -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF; }
