/*
 * sbc.c - an SBC encoder (A2DP specification, appendix B: the low-complexity subband codec),
 * the codec every A2DP sink must take.
 *
 * One frame is 'blocks' x 8 subband samples per channel, from 'blocks' x 8 PCM samples:
 * for each block the analysis filter windows the last 80 input samples with the
 * prototype filter C[], folds them to 16 values and turns those into 8 subband samples
 * with a cosine matrix.  Each subband then gets a scale factor (its peak as a power of
 * two) and, from the bitpool, a number of bits (the loudness allocation: the spec's
 * bitneed/bitslice procedure, shared between the channels in stereo); the samples are
 * quantised to that many bits.  The frame: a header with a CRC-8 over header and
 * scale factors, the scale factors, the samples.  Stereo, not joint stereo: every sink
 * takes it, and the encoder stays simple.
 *
 * Floating point is fine here: it runs in a kernel thread (the sound thread), whose
 * FPU state is saved like a program's.  tests/test_sbc.c checks it against ffmpeg's
 * decoder on the host.
 */
#include "sbc.h"

#ifdef SBC_HOST_TEST
#include <string.h>
#endif

/* the prototype filter for 8 subbands (A2DP specification, table 12.?, 80 coefficients) */
static const float C8[80] = {
     0.00000000e+00f,  1.56575398e-04f,  3.43256425e-04f,  5.54620202e-04f,  8.23919506e-04f,  1.13992507e-03f,  1.47640169e-03f,  1.78371725e-03f,
     2.01182542e-03f,  2.10371989e-03f,  1.99454554e-03f,  1.61656283e-03f,  9.02154502e-04f, -1.78805361e-04f, -1.64973098e-03f, -3.49717454e-03f,
     5.65949473e-03f,  8.02941163e-03f,  1.04584443e-02f,  1.27472335e-02f,  1.46525263e-02f,  1.59045603e-02f,  1.62208471e-02f,  1.53184106e-02f,
     1.29371806e-02f,  8.85757540e-03f,  2.92408442e-03f, -4.91578024e-03f, -1.46404076e-02f, -2.61098752e-02f, -3.90751381e-02f, -5.31873032e-02f,
     6.79989431e-02f,  8.29847578e-02f,  9.75753918e-02f,  1.11196689e-01f,  1.23264548e-01f,  1.33264415e-01f,  1.40753505e-01f,  1.45389847e-01f,
     1.46955068e-01f,  1.45389847e-01f,  1.40753505e-01f,  1.33264415e-01f,  1.23264548e-01f,  1.11196689e-01f,  9.75753918e-02f,  8.29847578e-02f,
    -6.79989431e-02f, -5.31873032e-02f, -3.90751381e-02f, -2.61098752e-02f, -1.46404076e-02f, -4.91578024e-03f,  2.92408442e-03f,  8.85757540e-03f,
     1.29371806e-02f,  1.53184106e-02f,  1.62208471e-02f,  1.59045603e-02f,  1.46525263e-02f,  1.27472335e-02f,  1.04584443e-02f,  8.02941163e-03f,
    -5.65949473e-03f, -3.49717454e-03f, -1.64973098e-03f, -1.78805361e-04f,  9.02154502e-04f,  1.61656283e-03f,  1.99454554e-03f,  2.10371989e-03f,
     2.01182542e-03f,  1.78371725e-03f,  1.47640169e-03f,  1.13992507e-03f,  8.23919506e-04f,  5.54620202e-04f,  3.43256425e-04f,  1.56575398e-04f,
};

/* loudness offsets for 8 subbands, by sampling frequency (16, 32, 44.1, 48 kHz) */
static const int OFFSET8[4][8] = {
    { -2, 0, 0, 0, 0, 0, 0, 1 }, { -3, 0, 0, 0, 0, 0, 1, 2 }, { -4, 0, 0, 0, 0, 0, 1, 2 }, { -4, 0, 0, 0, 0, 0, 1, 2 },
};

/* cos((i + 0.5) * (k - 4) * pi / 8), computed once (no cos() in the kernel: a short series) */
static float M[8][16];
static int m_ready;

static float cos_rad(float x) {
    /* reduce to [-pi, pi], then a Taylor series to x^14 (error < 1e-7) */
    const float pi = 3.14159265358979f;
    while (x > pi) x -= 2 * pi;
    while (x < -pi) x += 2 * pi;
    float x2 = x * x, term = 1, sum = 1;
    for (int n = 1; n <= 7; n++) { term *= -x2 / (float)((2 * n - 1) * (2 * n)); sum += term; }
    return sum;
}

void sbc_init(sbc_t *s, int rate, int channels, int blocks, int bitpool) {
    memset(s, 0, sizeof *s);
    s->freq = rate == 16000 ? 0 : rate == 32000 ? 1 : rate == 44100 ? 2 : 3;
    s->channels = channels == 1 ? 1 : 2;
    s->blocks = blocks == 4 || blocks == 8 || blocks == 12 ? blocks : 16;
    s->bitpool = bitpool;
    if (!m_ready) {
        for (int i = 0; i < 8; i++)
            for (int k = 0; k < 16; k++) M[i][k] = cos_rad((i + 0.5f) * (k - 4) * 3.14159265358979f / 8);
        m_ready = 1;
    }
}

/* stereo shares the bitpool between the channels, so both kinds are blocks x bitpool bits */
int sbc_frame_len(const sbc_t *s) { return 4 + 4 * s->channels + (s->blocks * s->bitpool + 7) / 8; }
int sbc_samples(const sbc_t *s) { return s->blocks * 8; }

/* bits, MSB first */
typedef struct { u8 *p; int bit; } bw_t;
static void put(bw_t *w, u32 v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (!(w->bit & 7)) w->p[w->bit >> 3] = 0;
        if ((v >> i) & 1) w->p[w->bit >> 3] |= (u8)(0x80 >> (w->bit & 7));
        w->bit++;
    }
}

static u8 crc8(const u8 *data, int bits) {
    u8 crc = 0x0f;
    for (int i = 0; i < bits; i++) {
        int in = (data[i >> 3] >> (7 - (i & 7))) & 1;
        int top = (crc >> 7) & 1;
        crc = (u8)(crc << 1);
        if (in ^ top) crc ^= 0x1d;                                     /* x^8 + x^4 + x^3 + x^2 + 1 */
    }
    return crc;
}

/* the loudness bit allocation, both channels sharing the bitpool (stereo) or one channel (mono) */
static void allocate(const sbc_t *s, const int sf[2][8], int bits[2][8]) {
    int nch = s->channels, need[2][8], maxneed = 0;
    for (int ch = 0; ch < nch; ch++)
        for (int sb = 0; sb < 8; sb++) {
            int n;
            if (sf[ch][sb] == 0) n = -5;
            else {
                int loud = sf[ch][sb] - OFFSET8[s->freq][sb];
                n = loud > 0 ? loud / 2 : loud;
            }
            need[ch][sb] = n;
            if (n > maxneed) maxneed = n;
        }
    int bitcount = 0, slicecount = 0, bitslice = maxneed + 1;
    do {
        bitslice--;
        bitcount += slicecount;
        slicecount = 0;
        for (int ch = 0; ch < nch; ch++)
            for (int sb = 0; sb < 8; sb++) {
                if (need[ch][sb] > bitslice + 1 && need[ch][sb] < bitslice + 16) slicecount++;
                else if (need[ch][sb] == bitslice + 1) slicecount += 2;
            }
    } while (bitcount + slicecount < s->bitpool);
    if (bitcount + slicecount == s->bitpool) { bitcount += slicecount; bitslice--; }
    for (int ch = 0; ch < nch; ch++)
        for (int sb = 0; sb < 8; sb++) {
            if (need[ch][sb] < bitslice + 2) bits[ch][sb] = 0;
            else { int b = need[ch][sb] - bitslice; bits[ch][sb] = b > 16 ? 16 : b; }
        }
    /* what is left: first to subbands that already have bits (or need exactly one more slice), then to any */
    int ch = 0, sb = 0;
    while (bitcount < s->bitpool && sb < 8) {
        if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) { bits[ch][sb]++; bitcount++; }
        else if (need[ch][sb] == bitslice + 1 && s->bitpool > bitcount + 1) { bits[ch][sb] = 2; bitcount += 2; }
        if (nch == 2 && ch == 0) ch = 1; else { ch = 0; sb++; }
    }
    ch = 0; sb = 0;
    while (bitcount < s->bitpool && sb < 8) {
        if (bits[ch][sb] < 16) { bits[ch][sb]++; bitcount++; }
        if (nch == 2 && ch == 0) ch = 1; else { ch = 0; sb++; }
    }
}

int sbc_encode(sbc_t *s, const i16 *pcm, u8 *out) {
    int nch = s->channels, nb = s->blocks;
    static float S[16][2][8];                                          /* subband samples [block][ch][sb] */
    /* analysis: 8 new samples per block and channel */
    for (int blk = 0; blk < nb; blk++)
        for (int ch = 0; ch < nch; ch++) {
            float *X = s->x[ch];
            for (int i = 79; i >= 8; i--) X[i] = X[i - 8];
            for (int i = 7; i >= 0; i--) X[i] = (float)pcm[(blk * 8 + (7 - i)) * nch + ch];
            float Y[16];
            for (int i = 0; i < 16; i++) {
                float y = 0;
                for (int k = 0; k < 5; k++) y += C8[i + 16 * k] * X[i + 16 * k];
                Y[i] = y;
            }
            for (int sb = 0; sb < 8; sb++) {
                float v = 0;
                for (int k = 0; k < 16; k++) v += M[sb][k] * Y[k];
                S[blk][ch][sb] = v;
            }
        }
    /* scale factors: 2^(sf + 1) above the subband's peak */
    int sf[2][8] = { { 0 } }, bits[2][8];
    for (int ch = 0; ch < nch; ch++)
        for (int sb = 0; sb < 8; sb++) {
            float mx = 0;
            for (int blk = 0; blk < nb; blk++) { float a = S[blk][ch][sb] < 0 ? -S[blk][ch][sb] : S[blk][ch][sb]; if (a > mx) mx = a; }
            int f = 0;
            while (f < 15 && (float)(2 << f) <= mx) f++;
            sf[ch][sb] = f;
        }
    allocate(s, sf, bits);
    /* the frame */
    bw_t w = { out, 0 };
    put(&w, 0x9c, 8);
    put(&w, (u32)s->freq, 2);
    put(&w, (u32)(nb / 4 - 1), 2);
    put(&w, nch == 2 ? 2 : 0, 2);                                     /* stereo / mono */
    put(&w, 0, 1);                                                     /* loudness */
    put(&w, 1, 1);                                                     /* 8 subbands */
    put(&w, (u32)s->bitpool, 8);
    put(&w, 0, 8);                                                     /* CRC, below */
    for (int ch = 0; ch < nch; ch++) for (int sb = 0; sb < 8; sb++) put(&w, (u32)sf[ch][sb], 4);
    /* CRC over header bytes 1-2 and the scale factors (the CRC byte itself left out) */
    u8 crcbuf[2 + 8];
    crcbuf[0] = out[1]; crcbuf[1] = out[2];
    memcpy(crcbuf + 2, out + 4, (usize)(4 * nch));
    out[3] = crc8(crcbuf, 16 + 32 * nch);
    for (int blk = 0; blk < nb; blk++)
        for (int ch = 0; ch < nch; ch++)
            for (int sb = 0; sb < 8; sb++) {
                int b = bits[ch][sb];
                if (!b) continue;
                u32 levels = (1u << b) - 1;
                float scale = (float)(2 << sf[ch][sb]);
                float q = (S[blk][ch][sb] / scale + 1.0f) * (float)levels / 2.0f;
                i32 v = (i32)q;
                if (v < 0) v = 0;
                if ((u32)v > levels) v = (i32)levels;
                put(&w, (u32)v, b);
            }
    return (w.bit + 7) / 8;
}
