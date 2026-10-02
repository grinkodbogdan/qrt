VERSION=chromium_7258
# ANGLE as Ladybird sees it: its EGL/GLES headers (WebGL's constants and types) and a stub
# libEGL/libGLESv2 in which there is no display, so EGL initialisation fails and Ladybird
# paints with Skia's CPU rasteriser.  QRT has no GPU driver yet (L7); the stub is replaced
# by a real ANGLE on top of one.
fetch() {
    [ -f "$S/.fetched" ] && return 0
    rm -rf "$S"
    git clone -q --depth 1 --branch chromium/7258 --filter=blob:none --sparse https://github.com/google/angle "$S"
    git -C "$S" sparse-checkout set include
    touch "$S/.fetched"
}
build() {
    mkdir -p "$B" "$SYS/usr/include"
    for d in EGL GLES GLES2 GLES3 KHR; do cp -r "$S/include/$d" "$SYS/usr/include/"; done
    python3 "$here/angle-stub.py" "$SYS/usr/include" > "$B/stub.c"
    $CC $CFLAGS -std=c23 -w -c "$B/stub.c" -o "$B/stub.o"
    $AR rcs "$SYS/usr/lib/libEGL.a" "$B/stub.o"
    $AR rcs "$SYS/usr/lib/libGLESv2.a"            # the stub has both
    cat > "$SYS/usr/lib/pkgconfig/angle.pc" <<PC
prefix=/usr
Name: angle
Description: ANGLE's headers and a no-GPU stub (QRT)
Version: 7258
Cflags: -I\${prefix}/include
Libs: -L\${prefix}/lib -lEGL
PC
}
