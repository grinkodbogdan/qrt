VERSION=4.6.4
fetch() { fetch_git https://github.com/simdjson/simdjson v$VERSION; }
build() { cmake_build . -DSIMDJSON_DEVELOPER_MODE=OFF -DSIMDJSON_ENABLE_THREADS=ON; }
