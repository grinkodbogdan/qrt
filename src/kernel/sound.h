/* sound.h - QRT's sound core (sound.c): streams in, one mixed output out.
 *
 * Programs (through /dev/dsp) and the kernel itself (Settings' test sound) write
 * 16-bit PCM into streams, each at its own rate and channel count.  An output
 * driver - USB audio (src/drivers/usb/uaudio.c), Bluetooth A2DP (src/drivers/bt/a2dp.c) -
 * pulls the mix at its own rate with snd_mix(); the volume is applied there.  With no
 * output, the sound thread consumes the streams in real time, so players keep time. */
#pragma once
#ifdef SND_HOST_TEST
#include <stdint.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int16_t i16; typedef int32_t i32; typedef int64_t i64; typedef size_t usize;
#else
#include "kernel.h"
#endif

#define SND_RATE      48000          /* a stream's default rate */
#define SND_MAX       8              /* streams open at once */
#define SND_RING      32768          /* frames queued per stream (0.68 s at 48 kHz) */

typedef struct snd_stream snd_stream_t;

/* streams (any thread) */
snd_stream_t *snd_open(void);                       /* 48 kHz, stereo, 16-bit; NULL if all are in use */
void snd_close(snd_stream_t *s);                    /* what is queued still plays */
int  snd_config(snd_stream_t *s, int rate, int channels);   /* returns 0 (rate 4000..192000, 1 or 2 channels) */
int  snd_rate(snd_stream_t *s);
int  snd_channels(snd_stream_t *s);
int  snd_write(snd_stream_t *s, const void *pcm, int bytes);  /* bytes taken (whole frames; 0 = full) */
int  snd_space(snd_stream_t *s);                    /* bytes that fit now */
int  snd_queued(snd_stream_t *s);                   /* bytes waiting to be played */
u64  snd_played(snd_stream_t *s);                   /* bytes consumed by the mixer since open (or reset) */
void snd_reset(snd_stream_t *s);                    /* drop what is queued */

/* outputs */
typedef struct snd_output {
    const char *name;                               /* "USB audio: ...", "Bluetooth: ..." */
    int rate, channels;                             /* what the device plays */
    void (*pump)(struct snd_output *o);             /* called every few ms from the sound thread */
    struct snd_output *next;
} snd_output_t;
void snd_output_add(snd_output_t *o);               /* the newest output plays */
void snd_output_remove(snd_output_t *o);
void snd_mix(i16 *out, int frames, int rate, int channels);   /* the mix of every stream, volume applied */
const char *snd_output_name(void);                  /* "" if none */

/* volume, status */
int  snd_volume(void);                              /* 0..100 */
void snd_set_volume(int v);
const char *snd_status(void);                       /* one line: the output, streams */
void snd_beep(void);                                /* a short chime (Settings: test sound) */

/* the kernel side (sound.c, native kernel) */
void snd_init(void);                                /* starts the sound thread */
void snd_tick(u64 now_us);                          /* no output: consume in real time (sound thread) */
