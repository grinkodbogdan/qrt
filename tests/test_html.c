/* Host test for src/apps/html.c: structure, whitespace, entities, lists,
 * links, forms and skipped elements.  Build/run: make check */
#define HTML_HOST_TEST
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef size_t usize; typedef int32_t i32;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
static void *kalloc(usize n) { void *p = calloc(1, n); if (!p) abort(); return p; }
static void kfree(void *p) { free(p); }
static int fmt(char *buf, usize cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
#include "../src/apps/html.h"
#include "../src/apps/html.c"

static int fails;
/* the document as text: words joined by their spaces, "|" for breaks, "||" for gaps, [field] */
static void render(hdoc_t *d, char *out, usize cap) {
    usize o = 0;
    out[0] = 0;
    for (int i = 0; i < d->n_nodes && o + 300 < cap; i++) {
        hnode_t *n = &d->nodes[i];
        if (n->kind == N_TEXT) o += (usize)snprintf(out + o, cap - o, "%s%s%s%s", n->space ? " " : "", n->link >= 0 ? "<" : "", d->text + n->off, n->link >= 0 ? ">" : "");
        else if (n->kind == N_BREAK) o += (usize)snprintf(out + o, cap - o, "|");
        else if (n->kind == N_GAP) o += (usize)snprintf(out + o, cap - o, "||");
        else if (n->kind == N_HR) o += (usize)snprintf(out + o, cap - o, "--");
        else if (n->kind == N_FIELD) o += (usize)snprintf(out + o, cap - o, "%s[%s=%s]", n->space ? " " : "", d->fields[n->field].name, d->fields[n->field].value);
    }
}
static void check(const char *what, const char *html, const char *want) {
    hdoc_t d;
    html_parse(&d, "http://x/", (const u8 *)html, strlen(html), 0, 0);
    char got[4096];
    render(&d, got, sizeof got);
    if (strcmp(got, want)) { printf("FAIL %s\n  got  %s\n  want %s\n", what, got, want); fails++; }
    html_free(&d);
}

int main(void) {
    check("words", "<p>Hello,   <b>bold</b>world  and <i>more</i>.</p>", "Hello, boldworld and more.");
    check("paragraphs", "<p>one</p><p>two</p>\n<h1>Head</h1>three", "one||two||Head||three");
    check("entities", "a &amp; b &lt;c&gt; caf&eacute; &#8212; &#x41;&nbsp;B &unknown; &#150;", "a & b <c> caf\xc3\xa9 \xe2\x80\x94 A B &unknown; \xe2\x80\x93");
    check("lists", "<ul><li>a</li><li>b<ol><li>c</li><li>d</li></ol></li></ul>after",
          "\xe2\x80\xa2" "a|\xe2\x80\xa2" "b|1.c|2.d||after");
    check("links", "see <a href=\"/p\">the page</a>, <a href='javascript:x()'>js</a>", "see <the> <page>, js");
    check("skipped", "a<script>var x = '<p>no</p>';</script>b<style>p{}</style>c<!-- comment --> d<svg><text>no</text></svg>", "abc d");
    check("br and pre", "x<br><br>y<pre>  one\n\ttwo</pre>z", "x||y||  one|    two||z");
    check("form", "<form action=/s><input name=q value=hi><input type=hidden name=t value=1><button>Go  now</button></form>",
          "[q=hi] [=Go now]");
    check("table", "<table><tr><td>a</td><td>b</td></tr><tr><th>c</th></tr></table>", "a b|c");
    check("block in li", "<ul><li><div>x</div></li><li><p>y</p></li></ul>", "\xe2\x80\xa2" "x|\xe2\x80\xa2" "y");
    check("lone lt", "1 < 2 and <3", "1 < 2 and <3");
    hdoc_t d;
    const char *t = "<html><head><title> My  &amp; Page </title></head><body>x</body></html>";
    html_parse(&d, "http://x/", (const u8 *)t, strlen(t), 0, 0);
    if (strcmp(d.title, "My & Page ") && strcmp(d.title, "My & Page")) { printf("FAIL title '%s'\n", d.title); fails++; }
    html_free(&d);
    const u8 latin[] = "caf\xe9 au lait";
    html_parse(&d, "http://x/", latin, 12, 0, !html_is_utf8(latin, 12));
    if (strcmp(d.text + d.nodes[0].off, "caf\xc3\xa9")) { printf("FAIL latin-1\n"); fails++; }
    html_free(&d);
    if (fails) { printf("html: %d failures\n", fails); return 1; }
    printf("html: text, entities, lists, links, forms and skipped elements pass\n");
    return 0;
}
