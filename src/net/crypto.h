/* crypto.h - what WPA2-Personal needs: SHA-1, HMAC-SHA1, PBKDF2, the 802.11
 * PRF, AES-128, AES key unwrap (RFC 3394) and CCMP (AES-CCM, M=8, L=2). */
#pragma once
#ifndef CRYPTO_HOST_TEST
#include "../kernel/rt.h"
#endif

typedef struct { u32 h[5]; u64 len; u8 buf[64]; u32 n; } sha1_t;
void sha1_init(sha1_t *s);
void sha1_update(sha1_t *s, const void *data, usize len);
void sha1_final(sha1_t *s, u8 out[20]);
void sha1(const void *data, usize len, u8 out[20]);

void hmac_sha1(const u8 *key, usize klen, const void *data, usize len, u8 out[20]);
void hmac_sha1_vec(const u8 *key, usize klen, int n, const void *const *data, const usize *len, u8 out[20]);
void pbkdf2_sha1(const char *pass, const u8 *salt, usize slen, int iter, u8 *out, usize olen);
/* IEEE 802.11 PRF (HMAC-SHA1 based): out = PRF-(olen*8)(key, label, data) */
void prf_sha1(const u8 *key, usize klen, const char *label, const u8 *data, usize dlen, u8 *out, usize olen);

typedef struct { u32 rk[44]; } aes128_t;
void aes128_init(aes128_t *a, const u8 key[16]);
void aes128_encrypt(const aes128_t *a, const u8 in[16], u8 out[16]);
void aes128_decrypt(const aes128_t *a, const u8 in[16], u8 out[16]);
int  aes_unwrap(const u8 kek[16], const u8 *in, usize inlen, u8 *out);   /* inlen multiple of 8, out = inlen - 8; 0 = ok */
void aes_wrap(const u8 kek[16], const u8 *in, usize inlen, u8 *out);     /* out = inlen + 8 */

/* CCM with an 8-byte MIC and 2-byte length (as CCMP uses): 0 = ok */
void ccm_encrypt(const aes128_t *a, const u8 nonce[13], const u8 *aad, usize alen, const u8 *pt, usize len, u8 *ct, u8 mic[8]);
int  ccm_decrypt(const aes128_t *a, const u8 nonce[13], const u8 *aad, usize alen, const u8 *ct, usize len, u8 *pt, const u8 mic[8]);

void random_bytes(u8 *out, usize n);
