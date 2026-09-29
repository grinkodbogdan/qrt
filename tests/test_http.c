/* Host test for the parsing half of src/net/http.c: URLs, relative links,
 * response heads and chunked bodies.  Build/run: make check */
#define HTTP_HOST_TEST
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef size_t usize;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
/* strlcpy and strlcat come from glibc (2.38+) */
static int fmt(char *buf, usize cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
#include "../src/net/http.h"
#include "../src/net/http.c"

static int fails;
static void want(const char *what, const char *got, const char *exp) {
    if (strcmp(got, exp)) { printf("FAIL %s\n  got  %s\n  want %s\n", what, got, exp); fails++; }
}

int main(void) {
    url_t u;
    char out[2300];
    if (url_parse("Example.COM", &u) || strcmp(u.host, "example.com") || u.https || u.port != 80 || strcmp(u.path, "/")) { printf("FAIL bare host\n"); fails++; }
    if (url_parse("https://a.b:8443/x?y=1#frag", &u) || !u.https || u.port != 8443 || strcmp(u.path, "/x?y=1")) { printf("FAIL full url\n"); fails++; }
    if (!url_parse("ftp://x", &u)) { printf("FAIL accepted ftp\n"); fails++; }
    const char *base = "https://en.wikipedia.org/wiki/Cherry_Trail?x=1";
    url_resolve(base, "/wiki/Atom", out, sizeof out);                 want("absolute path", out, "https://en.wikipedia.org/wiki/Atom");
    url_resolve(base, "Intel", out, sizeof out);                      want("relative", out, "https://en.wikipedia.org/wiki/Intel");
    url_resolve(base, "../w/index.php?t=1", out, sizeof out);         want("dot-dot", out, "https://en.wikipedia.org/w/index.php?t=1");
    url_resolve(base, "//upload.wikimedia.org/a.png", out, sizeof out); want("scheme-relative", out, "https://upload.wikimedia.org/a.png");
    url_resolve(base, "?q=2", out, sizeof out);                       want("query only", out, "https://en.wikipedia.org/wiki/Cherry_Trail?q=2");
    url_resolve(base, "#top", out, sizeof out);                       want("fragment", out, "https://en.wikipedia.org/wiki/Cherry_Trail?x=1");
    url_resolve("http://h/a/b/c", "./d", out, sizeof out);            want("dot", out, "http://h/a/b/d");
    url_resolve("http://h/a/b/", "../../..", out, sizeof out);        want("above root", out, "http://h/");
    url_resolve("http://h:8080/", "x y", out, sizeof out);            want("port and space", out, "http://h:8080/x%20y");
    url_resolve(base, "http://other/", out, sizeof out);              want("other site", out, "http://other/");
    url_encode_component("a b&c=d/é", out, sizeof out);               want("form encoding", out, "a+b%26c%3Dd%2F%C3%A9");

    const char *resp = "HTTP/1.1 301 Moved Permanently\r\nLocation:  https://x/y \r\nContent-Type: text/html; charset=utf-8\r\n"
                       "Transfer-Encoding: chunked\r\n\r\n";
    http_head_t h;
    if (http_parse_head((const u8 *)resp, strlen(resp) - 2, &h) != 0) { printf("FAIL head incomplete\n"); fails++; }
    if (http_parse_head((const u8 *)resp, strlen(resp), &h) != 1 || h.code != 301 || !h.chunked || h.content_length != -1) { printf("FAIL head\n"); fails++; }
    want("location", h.location, "https://x/y");
    want("content type", h.content_type, "text/html; charset=utf-8");
    char body[] = "5\r\nhello\r\n7;ext=1\r\n, world\r\n0\r\n\r\n";
    int complete;
    long n = http_dechunk((u8 *)body, strlen(body), &complete);
    if (n != 12 || !complete || memcmp(body, "hello, world", 12)) { printf("FAIL dechunk (%ld, %d)\n", n, complete); fails++; }
    char cut[] = "5\r\nhello\r\n7\r\n, wo";
    n = http_dechunk((u8 *)cut, strlen(cut), &complete);
    if (n != 5 || complete) { printf("FAIL dechunk cut short (%ld)\n", n); fails++; }
    if (fails) { printf("http: %d failures\n", fails); return 1; }
    printf("http: URLs, relative links, response heads and chunked bodies pass\n");
    return 0;
}
