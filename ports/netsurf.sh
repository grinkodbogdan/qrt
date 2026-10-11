#!/bin/bash
# ports/netsurf.sh - NetSurf (https://www.netsurf-browser.org), a small independent web
# browser with its own HTML/CSS engine, for QRT on 64-bit ARM: its libraries, then the
# browser with QRT's front-end - QRT_ARCH=x64 for the PC (ports/netsurf/qrt: a libnsfb surface on a QRT window and
# the framebuffer front-end's toolbar along the bottom, Kumo-style).
#   build/netsurf-src/<repo>  the sources (git clones of github.com/netsurf-browser/*,
#                             utf8proc from github.com/JuliaStrings/utf8proc)
#   build/netsurf-arm64       the libraries, installed; bin/netsurf at the end
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/build/netsurf-src
case "${QRT_ARCH:-arm64}" in
arm64) SDK=$ROOT/build/sdk-arm64; CC=$SDK/bin/qrt-a64-cc; HOST=aarch64-linux-musl; FONTS=$ROOT/build/arm64/browser-root ;;
x64)   SDK=$ROOT/build/sdk;       CC=$SDK/bin/qrt-cc;     HOST=x86_64-linux-musl;  FONTS=$ROOT/build/rootfs ;;
*) echo "QRT_ARCH: arm64 or x64" >&2; exit 1 ;;
esac
OUT=$ROOT/build/netsurf-${QRT_ARCH:-arm64}
mkdir -p "$OUT" "$SRC"

# the sources, at the commits QRT's port was made with
for r in buildsystem:0005ae3 libwapcaplet:c7c128d libparserutils:6b0cbf0 libhubbub:6651b8c libdom:f69781e \
         libcss:499f1c4 libnsgif:22e99eb libnsbmp:ea063c9 libnsutils:0bd3906 libnslog:bedff21 libnspsl:82815c2 \
         libsvgtiny:073283b libnsfb:b701cdc nsgenbind:44c6736 netsurf:39da3c3 libutf8proc:4bfe012; do
    name=${r%%:*}; rev=${r##*:}
    [ -d "$SRC/$name" ] && continue
    url=https://github.com/netsurf-browser/$name
    [ $name = libutf8proc ] && url=https://github.com/JuliaStrings/utf8proc
    git clone -q "$url" "$SRC/$name"
    git -C "$SRC/$name" checkout -q "$rev"
done

# pkg-config: what the SDK's sysroot lacks or names by the host's paths (zlib, libpng)
P=$OUT/lib/pkgconfig
mkdir -p "$P"
printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: zlib\nDescription: zlib (the QRT SDK'"'"'s)\nVersion: 1.3\nLibs: -L${libdir} -lz\nCflags: -I${includedir}\n' "$SDK/sysroot/usr" > "$P/zlib.pc"
for f in libpng16 libpng; do sed "s|^prefix=.*|prefix=$SDK/sysroot/usr|" "$SDK/sysroot/usr/lib/pkgconfig/libpng16.pc" > "$P/$f.pc"; done
printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: libutf8proc\nDescription: utf8proc\nVersion: 2.9.0\nLibs: -L${libdir} -lutf8proc\nCflags: -I${includedir} -DUTF8PROC_STATIC\n' "$OUT" > "$P/libutf8proc.pc"
export PKG_CONFIG_LIBDIR="$OUT/lib/pkgconfig:$SDK/sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=
MK="HOST=$HOST CC=$CC AR=llvm-ar-20 BUILD_CC=cc PREFIX=$OUT NSSHARED=$SRC/buildsystem NSBUILD=$SRC/buildsystem/makefiles Q= COMPONENT_TYPE=lib-static"
CF="-O2 -fPIE -I$OUT/include -Wno-error -Wno-unused-but-set-variable -Wno-incompatible-pointer-types"

# the build system's own tools first (host)
make -C "$SRC/buildsystem" install PREFIX="$OUT/host" Q= >/dev/null
# libnsfb with QRT's surface: a QRT window (ports/netsurf/qrt.c)
cp "$ROOT/ports/netsurf/qrt.c" "$SRC/libnsfb/src/surface/qrt.c"
grep -q "qrt.c" "$SRC/libnsfb/src/surface/Makefile" || sed -i 's/^SURFACE_HANDLER_yes := surface.c ram.c$/SURFACE_HANDLER_yes := surface.c ram.c qrt.c/' "$SRC/libnsfb/src/surface/Makefile"
for lib in libwapcaplet libparserutils libhubbub libdom libcss libnsgif libnsbmp libnsutils libnslog libnspsl libsvgtiny libnsfb; do
    [ -f "$OUT/.done-$lib" ] && continue
    echo "== $lib"
    CFLAGS="$CF" make -C "$SRC/$lib" $MK install
    touch "$OUT/.done-$lib"
done
if [ ! -f "$OUT/.done-utf8proc" ]; then
    mkdir -p "$OUT/lib"
    $CC -O2 -fPIE -DUTF8PROC_STATIC -c "$SRC/libutf8proc/utf8proc.c" -o "$OUT/utf8proc.o"
    rm -f "$OUT/lib/libutf8proc.a"; llvm-ar-20 rcs "$OUT/lib/libutf8proc.a" "$OUT/utf8proc.o"
    install -D -m644 "$SRC/libutf8proc/utf8proc.h" "$OUT/include/libutf8proc/utf8proc.h"
    install -D -m644 "$SRC/libutf8proc/utf8proc.h" "$OUT/include/utf8proc.h"
    touch "$OUT/.done-utf8proc"
fi
# nsgenbind: a host tool that writes the JavaScript bindings
if [ ! -x "$OUT/host/bin/nsgenbind" ]; then
    make -C "$SRC/nsgenbind" PREFIX="$OUT/host" NSSHARED=$SRC/buildsystem NSBUILD=$SRC/buildsystem/makefiles Q= install
fi
# libnsfb's surface draws through libqrt (QRT's windows)
grep -q -- "-lqrt" "$P/libnsfb.pc" || sed -i 's/^Libs: \(.*-lnsfb\)$/Libs: \1 -lqrt/' "$P/libnsfb.pc"

# NetSurf: QRT's front-end changes (the toolbar along the bottom, touch, the on-screen
# keyboard), its icons, the build's choices
N=$SRC/netsurf
if ! grep -q qrt_keyboard "$N/frontends/framebuffer/gui.c"; then
    git -C "$N" apply "$ROOT/ports/netsurf/0001-framebuffer-QRT-Kumo-style-bar-along-the-bottom-touch.patch"
fi
python3 "$ROOT/ports/netsurf/icons.py" "$N/frontends/framebuffer/res/icons"
cp "$ROOT/ports/netsurf/Makefile.config" "$N/Makefile.config"
rm -f "$N/nsfb"                                    # one tree, two machines: always relink
PATH=$OUT/host/bin:$PATH CFLAGS="-I$OUT/include -Wno-error -Wno-incompatible-pointer-types" \
    make -C "$N" -j"$(nproc)" TARGET=framebuffer HOST=$HOST CC=$CC AR=llvm-ar-20 \
         BUILD_CC=cc PREFIX=$OUT NSSHARED=$SRC/buildsystem Q=

# staged as the rootfs wants it: /bin/netsurf, /share/netsurf (resources, Choices), and
# the fonts and CA bundle it shares with Ladybird (build/arm64/browser-root, build/rootfs)
R=$OUT/root
rm -rf "$R"; mkdir -p "$R/bin" "$R/share/netsurf" "$R/share/fonts" "$R/etc/ssl/certs"
llvm-strip-20 -o "$R/bin/netsurf" "$N/nsfb"
cp -rL "$N"/frontends/framebuffer/res/Messages "$N"/frontends/framebuffer/res/*.css "$N"/frontends/framebuffer/res/*.html \
       "$N"/frontends/framebuffer/res/*.png "$N"/frontends/framebuffer/res/throbber "$N"/frontends/framebuffer/res/pointers "$R/share/netsurf/" 2>/dev/null || true
cp "$ROOT/ports/netsurf/Choices" "$R/share/netsurf/Choices"
B=$FONTS
for f in DejaVuSans DejaVuSans-Bold DejaVuSansMono DejaVuSansMono-Bold LiberationSans-Italic LiberationSans-BoldItalic \
         LiberationSerif-Regular LiberationSerif-Bold LiberationSerif-Italic; do cp "$B/share/fonts/$f.ttf" "$R/share/fonts/"; done
cp "$B/etc/ssl/certs/ca-certificates.crt" "$R/etc/ssl/certs/"
echo "netsurf: $R/bin/netsurf"
