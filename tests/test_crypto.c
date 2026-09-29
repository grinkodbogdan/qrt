/* Host unit test for src/net/crypto.c against published test vectors.
 * Build/run: make check */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef size_t usize; typedef int64_t i64;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CRYPTO_HOST_TEST
#include "../src/net/crypto.h"
#include "../src/net/crypto.c"
#include "../src/net/crypto_tls.c"

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
    /* ---- TLS 1.3 primitives ---- */
    u8 o32[64];
    sha256("abc", 3, o32);
    check("sha256(abc)", o32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, o32);
    check("sha256(448 bits)", o32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {   /* a million 'a' in odd-sized pieces */
        static u8 am[1000];
        memset(am, 'a', sizeof am);
        sha256_t st;
        sha256_init(&st);
        for (int i = 0; i < 1000; i++) sha256_update(&st, am, 1000);
        sha256_final(&st, o32);
        check("sha256(million a)", o32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
    /* RFC 4231 test case 2 */
    hmac_sha256((const u8 *)"Jefe", 4, "what do ya want for nothing?", 28, o32);
    check("hmac-sha256", o32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    /* RFC 5869 test case 1 */
    u8 ikm[22], salt[13], info[10], prk[32], okm[42];
    memset(ikm, 0x0b, 22);
    unhex("000102030405060708090a0b0c", salt);
    unhex("f0f1f2f3f4f5f6f7f8f9", info);
    hkdf_extract(salt, 13, ikm, 22, prk);
    check("hkdf extract", prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    hkdf_expand(prk, info, 10, okm, 42);
    check("hkdf expand", okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    /* RFC 8448 simple 1-RTT: early secret, then the "derived" secret */
    u8 zero32[32] = { 0 }, early[32], derived[32], empty_hash[32];
    hkdf_extract(NULL, 0, zero32, 32, early);
    check("tls13 early secret", early, "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a");
    sha256("", 0, empty_hash);
    hkdf_expand_label(early, "derived", empty_hash, 32, derived, 32);
    check("tls13 derived", derived, "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba");
    /* RFC 7748 5.2 */
    u8 sc[32], xpt[32], xo[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", sc);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", xpt);
    x25519(xo, sc, xpt);
    check("x25519 vector 1", xo, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    /* RFC 7748 6.1: Alice and Bob */
    u8 ask[32], bsk[32], apk[32], bpk[32], s1[32], s2[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", ask);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bsk);
    x25519_base(apk, ask);
    check("x25519 alice public", apk, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    x25519_base(bpk, bsk);
    check("x25519 bob public", bpk, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    x25519(s1, ask, bpk);
    x25519(s2, bsk, apk);
    check("x25519 shared (alice)", s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    check("x25519 shared (bob)", s2, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    /* GCM spec (McGrew/Viega) test case 4: 60-byte plaintext, 20-byte AAD */
    u8 gk[16], giv[12], gpt[60], gaad[20], gct[60], gtag[16], gback[60];
    unhex("feffe9928665731c6d6a8f9467308308", gk);
    unhex("cafebabefacedbaddecaf888", giv);
    unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", gpt);
    unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", gaad);
    aes128_init(&a, gk);
    gcm_encrypt(&a, giv, gaad, 20, gpt, 60, gct, gtag);
    check("gcm ciphertext", gct, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091");
    check("gcm tag", gtag, "5bc94fbc3221a5db94fae95ae7121a47");
    if (gcm_decrypt(&a, giv, gaad, 20, gct, 60, gback, gtag)) { printf("FAIL gcm rejected a good message\n"); fails++; }
    check("gcm decrypt", gback, "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
    gct[7] ^= 4;
    if (!gcm_decrypt(&a, giv, gaad, 20, gct, 60, gback, gtag)) { printf("FAIL gcm accepted a corrupted message\n"); fails++; }
    if (fails) { printf("crypto: %d failures\n", fails); return 1; }
    printf("crypto: all tests passed\n");
    return 0;
}
