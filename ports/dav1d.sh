VERSION=1.5.3
fetch() { fetch_git https://github.com/videolan/dav1d $VERSION; }
build() { meson_build -Denable_tools=false -Denable_tests=false -Denable_examples=false; }
