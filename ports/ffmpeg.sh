VERSION=7.1.1
fetch() { fetch_git https://github.com/FFmpeg/FFmpeg n$VERSION; }
# Ladybird's component set (its vcpkg overlay port, "ladybird-components"), with its library suffix
build() {
    mkdir -p "$B" && cd "$B"
    "$S/configure" --prefix=/usr --libdir=/usr/lib --enable-cross-compile --target-os=linux --arch=x86_64 \
        --cc="$CC" --cxx="$CXX" --ar=$AR --ranlib=$RANLIB --nm=$NM --strip=$STRIP --pkg-config=pkg-config \
        --extra-ldflags="$LDSTATIC" --enable-static --disable-shared --enable-pic --disable-doc --disable-programs \
        --enable-runtime-cpudetect --disable-autodetect --enable-pthreads --build-suffix=-ladybird \
        --disable-everything --disable-network --enable-libdav1d \
        --enable-decoder=vp8,vp9,libdav1d,theora,vorbis,opus,flac,mp3float,pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_alaw,pcm_mulaw \
        --enable-parser=vp8,vp9,av1,flac,mpegaudio --enable-demuxer=ogg,mov,flac,wav \
        --enable-swresample --disable-swscale --disable-avdevice --disable-avfilter >"$B.log" 2>&1
    make -j"$JOBS" >>"$B.log" 2>&1
    make DESTDIR="$SYS" install >>"$B.log" 2>&1
}
