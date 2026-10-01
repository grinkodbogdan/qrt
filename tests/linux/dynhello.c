/* dynhello.c - a dynamically linked glibc program: ld-linux-x86-64.so.2
 * maps libc.so.6 with file-backed mmap and starts it.  Prints "dynamic: ok". */
#include <dlfcn.h>
#include <gnu/libc-version.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    printf("dynamic: glibc %s via %s\n", gnu_get_libc_version(), argv[0]);
    void *m = dlopen("libm.so.6", RTLD_NOW);                  /* a library loaded at run time */
    double (*cosp)(double) = m ? (double (*)(double))dlsym(m, "cos") : NULL;
    double c = cosp ? cosp(0.0) : -1;
    printf("dynamic: dlopen(libm.so.6) %s, cos(0) = %.1f\n", m ? "ok" : dlerror(), c);
    printf("dynamic: %s\n", m && c == 1.0 ? "ok" : "FAILED");
    return m && c == 1.0 ? 0 : 1;
}
