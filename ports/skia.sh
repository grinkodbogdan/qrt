VERSION=148
# Skia at the commit vcpkg's skia port (version 148) pins, which is the one Ladybird builds
# against, with vcpkg's patches.  Built with GN as Ladybird's features ask for on Linux
# (FreeType, fontconfig, HarfBuzz, ICU) minus Vulkan: Skia's CPU rasteriser only, compiled
# for the tablet's CPU.  The
# libraries it uses are the sysroot's ("system"); wuffs is fetched from its GitHub mirror.
SKIA_COMMIT=e7c90ecca9444fe09598f1630ab7cee2c0ee027a
WUFFS_COMMIT=e3f919ccfe3ef542cfc983a82146070258fb57f8
fetch_commit() {  # URL COMMIT DIR
    [ -f "$3/.fetched" ] && return 0
    rm -rf "$3"; git init -q "$3"
    git -C "$3" fetch -q --depth 1 "$1" "$2"
    git -C "$3" -c advice.detachedHead=false checkout -q FETCH_HEAD
    touch "$3/.fetched"
}
fetch() {
    fetch_commit https://github.com/google/skia $SKIA_COMMIT "$S"
    fetch_commit https://github.com/google/wuffs-mirror-release-c $WUFFS_COMMIT "$S/third_party/externals/wuffs"
    if [ ! -f "$S/.patched" ]; then
        for p in "$here"/skia/*.patch; do git -C "$S" apply "$p"; done
        touch "$S/.patched"
    fi
}
build() {
    # GN only treats the compiler as clang when it is called exactly "clang"
    mkdir -p "$B/bin"
    printf '#!/bin/sh\nexec clang-20 "$@"\n' > "$B/bin/clang"
    printf '#!/bin/sh\nexec clang++-20 "$@"\n' > "$B/bin/clang++"
    chmod +x "$B/bin/clang" "$B/bin/clang++"
    export PATH="$B/bin:$PATH"
    sed -i "s|\"/usr/include/harfbuzz\"|\"$SYS/usr/include/harfbuzz\"|" "$S/third_party/harfbuzz/BUILD.gn"
    # -march: the tablet's Atom (Airmont: SSE4.2, SSSE3, no AVX); Skia's raster pipeline
    # picks its SSE4.1 code at compile time, so the x86-64 baseline (SSE2) leaves it out
    local common="\"--target=$TARGET\", \"--sysroot=$SYS\", \"-D__QRT__\", \"-fPIC\", \"-march=silvermont\", \"-I$SYS/usr/include/freetype2\""
    gn gen "$B/out" --root="$S" --args="
        target_cpu=\"x64\" target_os=\"linux\" is_official_build=true is_component_build=false
        cc=\"clang\" cxx=\"clang++\" ar=\"llvm-ar-20\"
        extra_cflags=[$common] extra_cflags_cc=[\"-stdlib=libc++\"] extra_ldflags=[\"-fuse-ld=lld\", \"-stdlib=libc++\"]
        skia_enable_android_utils=false skia_enable_spirv_validation=false skia_enable_tools=false
        skia_enable_gpu_debug_layers=false skia_use_jpeg_gainmaps=false skia_use_lua=false
        skia_vcpkg_has_lib_dl=true skia_use_dng_sdk=false skia_use_piex=false
        skia_use_fontconfig=true skia_use_freetype=true skia_system_freetype2_include_path=\"$SYS/usr/include/freetype2\"
        skia_use_harfbuzz=true skia_use_icu=true skia_use_system_icu=true
        skia_use_gl=false skia_use_vulkan=false skia_enable_graphite=false skia_enable_pdf=false
        skia_use_libjpeg_turbo_decode=false skia_use_libjpeg_turbo_encode=false skia_use_no_jpeg_encode=true
        skia_use_libpng_decode=false skia_use_libpng_encode=false skia_use_no_png_encode=true
        skia_use_libwebp_decode=false skia_use_libwebp_encode=false skia_use_no_webp_encode=true
        skia_use_libavif=false" >"$B.log" 2>&1 || { tail -30 "$B.log"; return 1; }
    ninja -C "$B/out" -j"$JOBS" skia modules >>"$B.log" 2>&1 || { grep -E "error|FAILED" "$B.log" | head -20; return 1; }
    # the libraries, Skia's header layout (include/skia/{include,modules,src}, and the
    # include/ part again at include/skia/ as vcpkg does), and a skia.pc for Ladybird
    install -m644 "$B"/out/*.a "$SYS/usr/lib/"
    local inc="$SYS/usr/include/skia"
    rm -rf "$inc"; mkdir -p "$inc"
    (cd "$S" && find include modules src -name '*.h' -exec cp --parents {} "$inc/" \;)
    cp -r "$inc/include/." "$inc/"
    local defs; defs=$(gn desc "$B/out" //:skia defines --root="$S" | grep -E "^SK_" | sed "s/^/-D/" | tr '\n' ' ')
    local libs; libs=$(cd "$B/out" && ls *.a | sed 's/^lib//;s/\.a$//' | grep -v '^skia$' | sed 's/^/-l/' | tr '\n' ' ')
    cat > "$SYS/usr/lib/pkgconfig/skia.pc" <<EOF
prefix=/usr
includedir=\${prefix}/include/skia
libdir=\${prefix}/lib
Name: skia
Description: Skia 2D graphics (CPU rasteriser), built for QRT
Version: $VERSION
Requires: freetype2 fontconfig harfbuzz icu-uc expat zlib
Cflags: -I\${includedir} $defs
Libs: -L\${libdir} $libs -lskia
EOF
}
