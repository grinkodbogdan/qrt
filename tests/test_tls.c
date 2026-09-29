/* Host test for src/net/tls.c: a TLS 1.3 handshake and an HTTP request
 * against a real server (OpenSSL through Python, started by
 * tools/tls-test.sh).  Usage: test_tls IP PORT [SERVER-NAME]
 * With a server name it fetches "/" from that (real) server and prints the
 * status line instead of checking the local test pattern. */
#define TLS_HOST_TEST
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef size_t usize; typedef int64_t i64;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CRYPTO_HOST_TEST
static void *kalloc(usize n) { void *p = calloc(1, n); if (!p) abort(); return p; }
static void kfree(void *p) { free(p); }
static usize strlcpy(char *d, const char *s, usize cap) { usize n = strlen(s); if (cap) { usize c = MIN(n, cap - 1); memcpy(d, s, c); d[c] = 0; } return n; }
static int fmt(char *buf, usize cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
#include "../src/net/crypto.h"
#include "../src/net/crypto.c"
#include "../src/net/crypto_tls.c"
#include "../src/net/tls.h"
#include "../src/net/tls.c"

static int s_send(void *ctx, const u8 *d, usize n) {
    ssize_t r = send(*(int *)ctx, d, n, MSG_NOSIGNAL);
    if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    return (int)r;
}
static int s_recv(void *ctx, u8 *d, usize n) {
    ssize_t r = recv(*(int *)ctx, d, n, 0);
    if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    if (r == 0) return -1;
    return (int)r;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s HOST PORT\n", argv[0]); return 2; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((u16)atoi(argv[2])) };
    inet_pton(AF_INET, argv[1], &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 1; }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    tls_io_t io = { s_send, s_recv, &fd };
    const char *name = argc > 3 ? argv[3] : "localhost";
    tls_t *t = tls_new(&io, name);
    time_t t0 = time(NULL);
    int r;
    while ((r = tls_poll(t)) == 0) { if (time(NULL) - t0 > 5) { printf("FAIL handshake timeout\n"); return 1; } usleep(1000); }
    if (r < 0) { printf("FAIL handshake: %s\n", tls_error(t)); return 1; }
    char req[512];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: QRT-test\r\nConnection: close\r\n\r\n", argc > 3 ? "/" : "/big", name);
    tls_write(t, req, strlen(req));
    static char resp[4 << 20];
    usize got = 0;
    while (!tls_eof(t)) {
        if (tls_poll(t) < 0) { printf("FAIL during data: %s\n", tls_error(t)); return 1; }
        int n = tls_read(t, resp + got, sizeof resp - 1 - got);
        if (n > 0) got += (usize)n; else usleep(1000);
        if (time(NULL) - t0 > 10) { printf("FAIL data timeout after %zu bytes\n", got); return 1; }
    }
    resp[got] = 0;
    if (argc > 3) { char *e = strstr(resp, "\r\n"); if (e) *e = 0; printf("%s: %s (%zu bytes)\n", name, resp, got); return strncmp(resp, "HTTP/1.", 7) != 0; }
    char *body = strstr(resp, "\r\n\r\n");
    if (strncmp(resp, "HTTP/1.", 7) || !body) { printf("FAIL bad response: %.80s\n", resp); return 1; }
    body += 4;
    /* the server sends 300000 bytes of a known pattern */
    usize bl = got - (usize)(body - resp);
    for (usize i = 0; i < bl; i++) if (body[i] != "0123456789abcdef"[i % 16]) { printf("FAIL body differs at %zu\n", i); return 1; }
    if (bl != 300000) { printf("FAIL body is %zu bytes, want 300000\n", bl); return 1; }
    printf("tls: TLS 1.3 handshake with OpenSSL and a 300 KB response over AES-128-GCM pass\n");
    tls_free(t);
    return 0;
}
