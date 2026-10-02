VERSION=0.3.4
fetch() { fetch_git https://github.com/google/wuffs v$VERSION; }
build() { mkdir -p "$SYS/usr/include/wuffs" && cp "$S/release/c/wuffs-v0.3.c" "$SYS/usr/include/wuffs/"; }
