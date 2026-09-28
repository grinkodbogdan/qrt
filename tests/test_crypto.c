/* Host unit test for src/net/crypto.c against published test vectors.
 * Build/run: make check */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef size_t usize;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CRYPTO_HOST_TEST
#include "../src/net/crypto.h"
#include "../src/net/crypto.c"

static int fails;
static void unhex(const char *h, u8 *out) { for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); *out++ = (u8)v; } }
static void check(const char *what, const u8 *got, const char *want_hex) {
    u8 want[256];
    usize n = strlen(want_hex) / 2;
    unhex(want_hex, want);
    if (memcmp(got, want, n)) {
        printf("FAIL %s\n  got  ", what);
        for (usize i = 0; i < n; i++) printf("%02x", got[i]);
        printf("\n  want %s\n", want_hex);
        fails++;
    }
}

int main(void) {
    u8 out[64];
    /* FIPS 180 */
    sha1("abc", 3, out);
    check("sha1(abc)", out, "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    check("sha1(448 bits)", out, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    /* RFC 2202 test case 1 and 2 */
    u8 key[80];
    memset(key, 0x0b, 20);
    hmac_sha1(key, 20, "Hi There", 8, out);
    check("hmac-sha1 #1", out, "b617318655057264e28bc0b6fb378c8ef146be00");
    hmac_sha1((const u8 *)"Jefe", 4, "what do ya want for nothing?", 28, out);
    check("hmac-sha1 #2", out, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    /* IEEE 802.11i H.4.3: passphrase -> PSK */
    pbkdf2_sha1("password", (const u8 *)"IEEE", 4, 4096, out, 32);
    check("pbkdf2 IEEE", out, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
    pbkdf2_sha1("ThisIsAPassword", (const u8 *)"ThisIsASSID", 11, 4096, out, 32);
    check("pbkdf2 ThisIsASSID", out, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af");
    /* IEEE 802.11i H.3 PRF test vector 1: key 0x0b*20, "prefix", "Hi There" */
    prf_sha1(key, 20, "prefix", (const u8 *)"Hi There", 8, out, 64);
    check("prf-512 #1", out, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a");
    /* FIPS 197 C.1 */
    aes128_t a;
    u8 k[16], pt[16], ct[16], back[16];
    unhex("000102030405060708090a0b0c0d0e0f", k);
    unhex("00112233445566778899aabbccddeeff", pt);
    aes128_init(&a, k);
    aes128_encrypt(&a, pt, ct);
    check("aes128 encrypt", ct, "69c4e0d86a7b0430d8cdb78070b4c55a");
    aes128_decrypt(&a, ct, back);
    check("aes128 decrypt", back, "00112233445566778899aabbccddeeff");
    /* RFC 3394 4.1 */
    u8 kek[16], kd[16], wrapped[24], unwrapped[16];
    unhex("000102030405060708090a0b0c0d0e0f", kek);
    unhex("00112233445566778899aabbccddeeff", kd);
    aes_wrap(kek, kd, 16, wrapped);
    check("aes wrap", wrapped, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5");
    if (aes_unwrap(kek, wrapped, 24, unwrapped)) { printf("FAIL aes unwrap integrity\n"); fails++; }
    check("aes unwrap", unwrapped, "00112233445566778899aabbccddeeff");
    wrapped[3] ^= 1;
    if (!aes_unwrap(kek, wrapped, 24, unwrapped)) { printf("FAIL aes unwrap accepted a corrupted key\n"); fails++; }
    /* RFC 3610 packet vector #1 (M = 8, L = 2) */
    u8 ck[16], nonce[13], aad[8], msg[23], enc[23], mic[8], dec[23];
    unhex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", ck);
    unhex("00000003020100a0a1a2a3a4a5", nonce);
    unhex("0001020304050607", aad);
    unhex("08090a0b0c0d0e0f101112131415161718191a1b1c1d1e", msg);
    aes128_init(&a, ck);
    ccm_encrypt(&a, nonce, aad, 8, msg, 23, enc, mic);
    check("ccm ciphertext", enc, "588c979a61c663d2f066d0c2c0f989806d5f6b61dac384");
    check("ccm mic", mic, "17e8d12cfdf926e0");
    if (ccm_decrypt(&a, nonce, aad, 8, enc, 23, dec, mic)) { printf("FAIL ccm decrypt rejected a good packet\n"); fails++; }
    check("ccm decrypt", dec, "08090a0b0c0d0e0f101112131415161718191a1b1c1d1e");
    enc[5] ^= 0x10;
    if (!ccm_decrypt(&a, nonce, aad, 8, enc, 23, dec, mic)) { printf("FAIL ccm accepted a corrupted packet\n"); fails++; }
    if (fails) { printf("crypto: %d failures\n", fails); return 1; }
    printf("crypto: all tests passed\n");
    return 0;
}
