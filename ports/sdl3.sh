VERSION=3.2.28
# SDL3, which Ladybird uses for gamepads only: no video, audio, rendering or GPU, and no
# udev/dbus.  Its joystick code looks for evdev devices; QRT has none yet, so a page sees
# no gamepads.
fetch() { fetch_git https://github.com/libsdl-org/SDL release-$VERSION; }
build() {
    cmake_build . -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_VIDEO=OFF -DSDL_AUDIO=OFF -DSDL_RENDER=OFF -DSDL_CAMERA=OFF -DSDL_GPU=OFF \
        -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_HIDAPI=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF \
        -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF -DSDL_ALSA=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_OSS=OFF \
        -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF \
        -DSDL_LIBURING=OFF -DSDL_DISABLE_INSTALL_DOCS=ON
}
