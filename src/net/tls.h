/* tls.h - a TLS 1.3 client (RFC 8446): TLS_AES_128_GCM_SHA256 with X25519.
 *
 * Non-blocking: the caller supplies send/recv callbacks (a TCP socket in
 * QRT, a host socket in tests/test_tls.c) and calls tls_poll() until it
 * returns 1.
 *
 * The server's certificate is NOT verified: QRT has no certificate store
 * or signature checks yet.  The connection is encrypted against eavesdroppers
 * but not authenticated, so an attacker on the path could impersonate the
 * server.  The browser says so on every https page. */
#pragma once
#ifndef TLS_HOST_TEST
#include "../kernel/rt.h"
#endif

typedef struct {
    int (*send)(void *ctx, const u8 *data, usize len);   /* bytes accepted (may be fewer), -1 = error */
    int (*recv)(void *ctx, u8 *buf, usize len);          /* bytes read, 0 = none yet, -1 = closed or error */
    void *ctx;
} tls_io_t;

typedef struct tls tls_t;

tls_t *tls_new(const tls_io_t *io, const char *host);
int  tls_poll(tls_t *t);                         /* 1 = established, 0 = handshaking, -1 = failed */
int  tls_write(tls_t *t, const void *data, usize len);   /* after tls_poll() == 1; len or -1 */
int  tls_read(tls_t *t, void *buf, usize len);   /* plaintext bytes, 0 = none yet (call tls_poll) */
int  tls_eof(tls_t *t);                          /* the server closed and everything was read */
const char *tls_error(tls_t *t);
void tls_free(tls_t *t);
