VERSION=3.53.4
fetch() { fetch_tar http://archive.ubuntu.com/ubuntu/pool/main/s/sqlite3/sqlite3_$VERSION.orig.tar.xz; }
build() {
    # the amalgamation (sqlite3.c) is generated on the host, then cross-compiled
    mkdir -p "$B/host" && cd "$B/host"
    CC=gcc CFLAGS= LDFLAGS= "$S/configure" >/dev/null 2>&1
    CC=gcc CFLAGS= LDFLAGS= make sqlite3.c >/dev/null 2>&1
    cd "$B"
    # exclusive locking by default: WAL's index then lives in the process's memory instead
    # of a shared mapping of the -shm file, which QRT's file mmap does not share
    $CC $CFLAGS -DSQLITE_DEFAULT_LOCKING_MODE=1 -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5 -DSQLITE_OMIT_LOAD_EXTENSION -c host/sqlite3.c -o sqlite3.o
    $AR rcs libsqlite3.a sqlite3.o
    cp libsqlite3.a "$SYS/usr/lib/" && cp host/sqlite3.h "$S/src/sqlite3ext.h" "$SYS/usr/include/" 2>/dev/null || cp host/sqlite3.h "$SYS/usr/include/"
    mkdir -p "$SYS/usr/lib/pkgconfig"
    cat > "$SYS/usr/lib/pkgconfig/sqlite3.pc" <<PC
prefix=/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: SQLite
Description: SQL database engine
Version: $VERSION
Libs: -L\${libdir} -lsqlite3
Cflags: -I\${includedir}
PC
}
