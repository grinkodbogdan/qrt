/* http.h - HTTP/1.1 GET over QRT's TCP (http://) or TLS 1.3 (https://).
 * Non-blocking: start with http_get(), then call http_poll() every frame. */
#pragma once
#ifndef HTTP_HOST_TEST
#include "../kernel/rt.h"
#endif

typedef struct { int https; char host[256]; u16 port; char path[2048]; } url_t;
int  url_parse(const char *s, url_t *u);                     /* 0 = ok; "example.com" means http:// */
void url_format(const url_t *u, char *out, usize cap);
void url_resolve(const char *base, const char *ref, char *out, usize cap);   /* a link relative to a page */
void url_encode_component(const char *in, char *out, usize cap);             /* for form fields */

/* the parts of a response QRT uses; body is decoded (chunked) in place */
typedef struct {
    int code;
    char content_type[128], location[2048], encoding[32], date[40];
    long content_length;              /* -1 = not given */
    int chunked;
    usize header_len;                 /* bytes up to and including the blank line */
} http_head_t;
int  http_parse_head(const u8 *buf, usize len, http_head_t *h);   /* 1 = complete, 0 = need more, -1 = bad */
long http_dechunk(u8 *body, usize len, int *complete);            /* new length, or -1 = bad */

#ifndef HTTP_HOST_TEST
typedef struct http http_t;
http_t *http_get(const char *url);
http_t *http_post(const char *url, const char *form_body);   /* application/x-www-form-urlencoded */
int  http_poll(http_t *h);                  /* 0 = working, 1 = done, -1 = failed */
const char *http_progress(http_t *h);       /* "Looking up example.com..." */
const char *http_error(http_t *h);
int  http_code(http_t *h);
const char *http_url(http_t *h);            /* after redirects */
const char *http_content_type(http_t *h);
const u8 *http_body(http_t *h, usize *len);
int  http_secure(http_t *h);                /* fetched over TLS */
const char *http_date(http_t *h);           /* the server's Date: header, "" if none */
long long http_date_parse(const char *s);  /* "Sun, 06 Nov 1994 08:49:37 GMT" -> Unix time, -1 = bad */
void http_free(http_t *h);
#endif
