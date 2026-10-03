/* sbc.h - the SBC encoder (sbc.c): A2DP's mandatory codec, 8 subbands, loudness
 * allocation, mono or stereo. */
#pragma once
#ifdef SBC_HOST_TEST
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint32_t u32; typedef int16_t i16; typedef int32_t i32; typedef size_t usize;
#else
#include "../../kernel/kernel.h"
#endif

typedef struct {
    int freq;                  /* 0: 16 kHz, 1: 32, 2: 44.1, 3: 48 */
    int channels, blocks, bitpool;
    float x[2][80];            /* the analysis filter's input history, per channel */
} sbc_t;

void sbc_init(sbc_t *s, int rate, int channels, int blocks, int bitpool);
int  sbc_frame_len(const sbc_t *s);                       /* bytes per frame */
int  sbc_samples(const sbc_t *s);                         /* PCM frames per SBC frame: blocks x 8 */
int  sbc_encode(sbc_t *s, const i16 *pcm, u8 *out);       /* pcm: blocks x 8 frames, interleaved; returns the frame's length */
