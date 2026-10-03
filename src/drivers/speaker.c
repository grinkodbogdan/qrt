/*
 * speaker.c - the Venue 8 Pro 5855's own speakers (0.9.5): Intel's audio DSP
 * running Sound Open Firmware, an SSP (I2S) port, and the Realtek RT5672 codec.
 *
 * The path, as Linux drives it (sound/soc/sof/intel/byt.c, atom.c, ipc3-*.c, and
 * sound/soc/intel/boards/cht_bsw_rt5672.c, sound/soc/codecs/rt5670.c):
 *
 *   QRT's mixer -> a ring in RAM -> the DSP's host DMA -> volume -> mixer -> volume
 *     -> SSP2, I2S 48 kHz 24-bit, 2 x 25-bit slots, the SoC clocking it
 *     -> RT5672 AIF1 -> DAC1 -> stereo DAC mixer -> PDM1 -> speaker amplifier
 *
 * The DSP ("LPE", PCI 8086:22a8, 2 MiB window) has 80 KiB of instruction RAM
 * (+0x0C0000), 160 KiB of data RAM (+0x100000), the SHIM registers (+0x140000)
 * and the mailbox (+0x144000).  We stop it, copy sof-cht.ri's blocks to the
 * offsets they name, release it, and wait for its FW_READY message.  Then IPC3
 * messages build the pipelines of sof-cht-rt5670.tplg (the topology Linux uses
 * with this codec, read out of the .tplg file: the same components, UUIDs, sizes
 * and SSP2 settings), and a PCM stream points the DSP at our ring through a page
 * table of 20-bit page numbers.  The DSP can only reach the low 2 GiB.
 *
 * IPC: the host writes a message to its mailbox and sets BUSY (bit 63) in IPCX;
 * the DSP answers in the same mailbox and sets DONE (bit 62).  The DSP's own
 * messages (positions, its boot) come through IPCD the same way, and we
 * acknowledge them.  We poll - no interrupt line is needed.
 *
 * The codec needs a 19.2 MHz master clock: the PMC's platform clock 3.  Its
 * PLL makes 24.576 MHz (512 x 48 kHz) from it; it is an I2S slave, its ASRC
 * follows the SSP's clock.
 *
 * None of this can run in QEMU.  Every step logs what it found, so a photo of
 * System Monitor's log tells what the tablet did.
 */
#include "speaker.h"
#include "audio.h"
#include "dwi2c.h"
#include "pci.h"
#include "touch.h"
#include "../kernel/sound.h"
#include "../kernel/vfs.h"
#if defined(__x86_64__) && !defined(SPEAKER_HOST_TEST)
#include "../arch/x64/sched.h"
#elif defined(SPEAKER_HOST_TEST)
void thread_yield(void);
void thread_sleep_ms(u64 ms);
void *thread_create(const char *name, void (*fn)(void *), void *arg, u64 cr3);
void thread_exit(void);
void *mm_map_mmio(u64 base, u64 size);
u64 pmm_alloc_contig(usize pages);
#endif

static char status[160] = "not started";
const char *speaker_status(void) { return status; }

#if defined(__x86_64__) || defined(SPEAKER_HOST_TEST)
#define FW_PATH   "/lib/firmware/sof-cht.ri"

/* the LPE window */
#define SHIM      0x140000
#define SHIM_CSR  (SHIM + 0x00)
#define SHIM_IMRX (SHIM + 0x28)
#define SHIM_IMRD (SHIM + 0x30)
#define SHIM_IPCX (SHIM + 0x38)
#define SHIM_IPCD (SHIM + 0x40)
#define MBOX      0x144000
#define CSR_RST       (1ull << 0)
#define CSR_VECTOR    (1ull << 1)
#define CSR_STALL     (1ull << 2)
#define CSR_PWAITMODE (1ull << 3)
#define IPC_BUSY  (1ull << 63)
#define IPC_DONE  (1ull << 62)

/* IPC3 */
#define GLB(x)          ((u32)(x) << 28)
#define CMD(x)          ((u32)(x) << 16)
#define GLB_REPLY       GLB(1)
#define GLB_TPLG        GLB(3)
#define GLB_STREAM      GLB(6)
#define GLB_FW_READY    GLB(7)
#define GLB_DAI         GLB(8)
#define GLB_TRACE       GLB(9)
#define TPLG_COMP_NEW   CMD(1)
#define TPLG_CONNECT    CMD(3)
#define TPLG_PIPE_NEW   CMD(0x10)
#define TPLG_PIPE_DONE  CMD(0x13)
#define TPLG_BUFFER_NEW CMD(0x20)
#define DAI_CONFIG      CMD(1)
#define STREAM_PARAMS   CMD(1)
#define STREAM_START    CMD(4)
#define STREAM_STOP     CMD(5)
#define STREAM_POSITION CMD(0xa)
#define STREAM_XRUN     CMD(9)
#define PANIC_MAGIC     0x0dead000u
#define PANIC_MASK      0x0ffff000u

enum { COMP_HOST = 1, COMP_DAI = 2, COMP_VOLUME = 5, COMP_MIXER = 6, COMP_BUFFER = 12 };
enum { FMT_S16 = 0, FMT_S24 = 1, FMT_S32 = 2 };

/* the topology's components (ids are ours) */
enum { P1 = 1, MIXER1 = 2, PGA1 = 3, DAI_SSP2 = 4, BUF10 = 5, BUF11 = 6, P3 = 10, HOST0 = 11, PGA3 = 12, BUF30 = 13, BUF31 = 14 };

/* UUIDs, as stored in sof-cht-rt5670.tplg */
static const u8 uuid_mixer[16] = { 0x37,0xc0,0x06,0xbc,0xaa,0x12,0x7c,0x41,0x9a,0x97,0x89,0x28,0x2e,0x32,0x1a,0x76 };
static const u8 uuid_volume[16] = { 0x7e,0x67,0x7e,0xb7,0xf4,0x5f,0x88,0x41,0xaf,0x14,0xfb,0xa8,0xbd,0xbf,0x86,0x82 };
static const u8 uuid_dai[16] = { 0x27,0x0d,0xb0,0xc2,0xbc,0xff,0x50,0x41,0xa5,0x1a,0x24,0x5c,0x79,0xc5,0xe5,0x4b };
static const u8 uuid_host[16] = { 0x0c,0x10,0x9d,0x8b,0x78,0x6d,0x8f,0x41,0x90,0xa3,0xe0,0xe8,0x05,0xd0,0x85,0x2b };

#define RING_PAGES 4
#define RING_BYTES (RING_PAGES * 4096)

static struct {
    volatile u8 *lpe;
    u32 hostbox, dspbox, streambox, posn_offset;
    u32 abi;
    u8 *ring;                    /* RING_BYTES, below 2 GiB */
    u8 *ptable;
    u64 wpos;                    /* bytes written into the ring, ever */
    volatile u64 dsp_pos;        /* the DSP's host position (bytes in the ring) */
    u64 dsp_wraps, last_dsp;
    u64 started_us;
    int positions, xruns;
    snd_output_t out;
    i16 mix[1024 * 2];
} S;

/* ---- the window ---------------------------------------------------------------------- */
#ifdef SPEAKER_HOST_TEST
static void fake_dsp_write(u32 off, u64 v);             /* tests/test_speaker.c: the simulated DSP */
static u64 rd64(u32 off) { return *(volatile u64 *)(S.lpe + off); }
static void wr64(u32 off, u64 v) { *(volatile u64 *)(S.lpe + off) = v; fake_dsp_write(off, v); }
#else
static u64 rd64(u32 off) { return *(volatile u64 *)(S.lpe + off); }
static void wr64(u32 off, u64 v) { *(volatile u64 *)(S.lpe + off) = v; }
#endif
static void copy_to(u32 off, const void *src, usize n) {        /* 32-bit accesses, as the DSP's RAM wants */
    const u8 *s = src;
    for (usize i = 0; i < n; i += 4) {
        u32 w = 0;
        memcpy(&w, s + i, MIN(4, n - i));
        *(volatile u32 *)(S.lpe + off + i) = w;
    }
}
static void copy_from(void *dst, u32 off, usize n) {
    u8 *d = dst;
    for (usize i = 0; i < n; i += 4) {
        u32 w = *(volatile u32 *)(S.lpe + off + i);
        memcpy(d + i, &w, MIN(4, n - i));
    }
}

/* ---- messages from the DSP ----------------------------------------------------------------- */
static int fw_ready_seen;
static void rx_one(void) {
    u64 d = rd64(SHIM_IPCD);
    if (!(d & IPC_BUSY)) return;
    if (((u32)d & PANIC_MASK) == PANIC_MAGIC) {
        fmt(status, sizeof status, "the DSP firmware stopped (panic %08x)", (u32)d);
        klog("speaker: %s", status);
    } else {
        u32 hdr[2];
        copy_from(hdr, S.dspbox, 8);
        u32 cmd = hdr[1];
        if ((cmd & 0xf0000000u) == GLB_FW_READY) fw_ready_seen = 1;
        else if ((cmd & 0xf0000000u) == GLB_STREAM && (cmd & 0x0fff0000u) == STREAM_POSITION) {
            u64 pos = 0;
            u32 at = (S.streambox ? S.streambox + S.posn_offset : S.dspbox) + 28;   /* sof_ipc_stream_posn.host_posn */
            copy_from(&pos, at, 8);
            S.dsp_pos = pos % RING_BYTES;
            S.positions++;
        } else if ((cmd & 0xf0000000u) == GLB_STREAM && (cmd & 0x0fff0000u) == STREAM_XRUN) S.xruns++;
    }
    wr64(SHIM_IPCD, (d & ~IPC_BUSY) | IPC_DONE);           /* accept the next one */
}

/* ---- messages to the DSP ----------------------------------------------------------------- */
static int tx(const void *msg, u32 len, void *reply, u32 rlen) {
    copy_to(S.hostbox, msg, len);
    wr64(SHIM_IPCX, IPC_BUSY);
    u64 end = k_now_ms() + 500;
    while (!(rd64(SHIM_IPCX) & IPC_DONE)) {
        rx_one();
        if (k_now_ms() > end) {
            klog("speaker: the DSP did not answer %08x", ((const u32 *)msg)[1]);
            return -1;
        }
        thread_yield();
    }
    u32 r[3] = { 0 };
    copy_from(r, S.hostbox, 12);
    if (reply && rlen) copy_from(reply, S.hostbox, rlen);
    wr64(SHIM_IPCX, rd64(SHIM_IPCX) & ~IPC_DONE);
    if ((i32)r[2] < 0) { klog("speaker: the DSP refused %08x: error %d", ((const u32 *)msg)[1], (i32)r[2]); return (i32)r[2]; }
    return 0;
}

/* ---- the topology ----------------------------------------------------------------- */
/* struct sof_ipc_comp + struct sof_ipc_comp_config, then the type's fields, then the UUID */
static int comp_new(u32 id, u32 type, u32 pipe, u32 sink, u32 source, u32 frame_fmt,
                    const u32 *extra, int nextra, const u8 uuid[16]) {
    u32 m[40];
    memset(m, 0, sizeof m);
    int n = 0;
    m[n++] = 0;                                     /* hdr.size, below */
    m[n++] = GLB_TPLG | TPLG_COMP_NEW;
    m[n++] = id; m[n++] = type; m[n++] = pipe; m[n++] = 0 /* core */; m[n++] = 16 /* ext_data_length */;
    m[n++] = 36; m[n++] = 0;                        /* comp_config hdr */
    m[n++] = sink; m[n++] = source; m[n++] = 0; m[n++] = frame_fmt; m[n++] = 0; m[n++] = 0; m[n++] = 0;
    for (int i = 0; i < nextra; i++) m[n++] = extra[i];
    memcpy(&m[n], uuid, 16);
    n += 4;
    m[0] = (u32)n * 4;
    return tx(m, m[0], NULL, 0);
}
static int buffer_new(u32 id, u32 pipe, u32 size, u32 caps) {
    u32 m[11] = { 44, GLB_TPLG | TPLG_BUFFER_NEW, id, COMP_BUFFER, pipe, 0, 0, size, caps, 0, 0 };
    return tx(m, sizeof m, NULL, 0);
}
static int pipe_new(u32 comp, u32 pipe, u32 sched, u32 priority, u32 mips) {
    u32 m[12] = { 48, GLB_TPLG | TPLG_PIPE_NEW, comp, pipe, sched, 0, 1000 /* us */, priority, mips, 0, 0, 0 };
    return tx(m, sizeof m, NULL, 0);
}
static int connect(u32 src, u32 sink) { u32 m[4] = { 16, GLB_TPLG | TPLG_CONNECT, src, sink }; return tx(m, sizeof m, NULL, 0); }
static int pipe_done(u32 comp) { u32 m[3] = { 12, GLB_TPLG | TPLG_PIPE_DONE, comp }; return tx(m, sizeof m, NULL, 0); }

/* SSP2: I2S, the SoC provides BCLK and FSYNC, MCLK 19.2 MHz, BCLK 2.4 MHz, 48 kHz, 2 slots of 25 bits, 24 valid */
static int dai_config(u32 flags) {
    u8 m[124];
    memset(m, 0, sizeof m);
    u32 *w = (u32 *)m;
    w[0] = sizeof m; w[1] = GLB_DAI | DAI_CONFIG;
    w[2] = 1 /* SOF_DAI_INTEL_SSP */; w[3] = 2 /* SSP2 */;
    *(u16 *)(m + 16) = 0x0001 | 0x4000;     /* SOF_DAI_FMT_I2S | CBC_CFC (codec consumes both clocks) */
    m[18] = 0; m[19] = (u8)flags;
    u8 *ssp = m + 52;                       /* after reserved[8] */
    *(u32 *)(ssp + 0) = 60;                 /* hdr.size */
    *(u16 *)(ssp + 6) = 0;                  /* mclk_id */
    *(u32 *)(ssp + 8) = 19200000;           /* mclk_rate */
    *(u32 *)(ssp + 12) = 48000;             /* fsync_rate */
    *(u32 *)(ssp + 16) = 2400000;           /* bclk_rate */
    *(u32 *)(ssp + 20) = 2;                 /* tdm_slots */
    *(u32 *)(ssp + 24) = 3;                 /* rx_slots */
    *(u32 *)(ssp + 28) = 3;                 /* tx_slots */
    *(u32 *)(ssp + 32) = 24;                /* sample_valid_bits */
    *(u16 *)(ssp + 36) = 25;                /* tdm_slot_width */
    *(u32 *)(ssp + 40) = 1;                 /* mclk_direction: into the codec */
    return tx(m, sizeof m, NULL, 0);
}

static int build_topology(void) {
    int e = 0;
    /* pipeline 1: mixer -> buffer -> volume -> buffer -> SSP2 */
    e |= pipe_new(P1, 1, DAI_SSP2, 1, 5000);
    e |= comp_new(MIXER1, COMP_MIXER, 1, 2, 2, FMT_S24, NULL, 0, uuid_mixer);
    e |= buffer_new(BUF10, 1, 768, 65);
    u32 vol1[5] = { 2, 0x41, 0x10000, 0 /* linear ramp */, 20 };
    e |= comp_new(PGA1, COMP_VOLUME, 1, 2, 2, FMT_S24, vol1, 5, uuid_volume);
    e |= buffer_new(BUF11, 1, 768, 65);
    u32 dai[4] = { 0 /* playback */, 2 /* SSP2 */, 1 /* SSP */, 0 };
    e |= comp_new(DAI_SSP2, COMP_DAI, 1, 0, 2, FMT_S24, dai, 4, uuid_dai);
    if (e) return e;
    if ((e = dai_config(0))) return e;
    /* pipeline 3: PCM 0 (our ring) -> buffer -> volume -> buffer -> the mixer above */
    e |= pipe_new(P3, 3, DAI_SSP2, 0, 100000);
    u32 host[3] = { 0 /* playback */, 0 /* interrupts */, 0 };
    e |= comp_new(HOST0, COMP_HOST, 3, 2, 0, FMT_S16, host, 3, uuid_host);
    e |= buffer_new(BUF30, 3, 768, 97);
    u32 vol3[5] = { 2, 0x41, 0x10000, 0, 20 };
    e |= comp_new(PGA3, COMP_VOLUME, 3, 2, 2, FMT_S32, vol3, 5, uuid_volume);
    e |= buffer_new(BUF31, 3, 768, 97);
    if (e) return e;
    e |= connect(HOST0, BUF30); e |= connect(BUF30, PGA3); e |= connect(PGA3, BUF31); e |= connect(BUF31, MIXER1);
    e |= connect(MIXER1, BUF10); e |= connect(BUF10, PGA1); e |= connect(PGA1, BUF11); e |= connect(BUF11, DAI_SSP2);
    if (e) return e;
    e |= pipe_done(P1);
    e |= pipe_done(P3);
    return e;
}

static int start_stream(void) {
    /* the page table: 20-bit page numbers, two in five bytes */
    memset(S.ptable, 0, 4096);
    for (int i = 0; i < RING_PAGES; i++) {
        u32 pfn = (u32)((u64)(usize)(S.ring + i * 4096) >> 12);
        u8 *p = S.ptable + (5 * i) / 2;
        u32 v;
        memcpy(&v, p, 4);
        if (i & 1) v = (v & ~0xfffffff0u) | (p[0] & 0xf) | pfn << 4;
        else v = (v & 0xfff00000u) | pfn;
        memcpy(p, &v, 4);
    }
    if (dai_config(1 /* HW_PARAMS */)) return -1;
    u8 m[108];
    memset(m, 0, sizeof m);
    u32 *w = (u32 *)m;
    w[0] = sizeof m; w[1] = GLB_STREAM | STREAM_PARAMS; w[2] = HOST0;
    u8 *p = m + 24;                                     /* sof_ipc_stream_params */
    *(u32 *)(p + 0) = 84;
    *(u32 *)(p + 4) = 28;                               /* host buffer hdr.size */
    *(u32 *)(p + 8) = (u32)(usize)S.ptable;             /* phy_addr: the page table */
    *(u32 *)(p + 12) = RING_PAGES;
    *(u32 *)(p + 16) = RING_BYTES;
    *(u32 *)(p + 32) = 0;                               /* direction: playback */
    *(u32 *)(p + 36) = FMT_S16;
    *(u32 *)(p + 40) = 0;                               /* interleaved */
    *(u32 *)(p + 44) = 48000;
    *(u16 *)(p + 48) = 0;                               /* stream_tag */
    *(u16 *)(p + 50) = 2;                               /* channels */
    *(u16 *)(p + 52) = 2;                               /* valid bytes */
    *(u16 *)(p + 54) = 2;                               /* container bytes */
    *(u32 *)(p + 56) = RING_BYTES / 4;                  /* a position message each quarter */
    u32 reply[5] = { 0 };
    if (tx(m, sizeof m, reply, sizeof reply)) return -1;
    S.posn_offset = reply[4];
    u32 t[3] = { 12, GLB_STREAM | STREAM_START, HOST0 };
    return tx(t, sizeof t, NULL, 0);
}

/* ---- feeding the ring (the sound thread) ----------------------------------------------- */
static void pump(snd_output_t *o) {
    (void)o;
    rx_one();
    /* where the DSP reads: its last position, or the clock when it sends none */
    u64 read;
    if (S.positions) {
        if (S.dsp_pos < S.last_dsp) S.dsp_wraps++;
        S.last_dsp = S.dsp_pos;
        read = S.dsp_wraps * RING_BYTES + S.dsp_pos;
    } else read = (k_now_us() - S.started_us) * 48000 / 1000000 * 4;
    u64 target = read + RING_BYTES / 2;                 /* stay about 40 ms ahead */
    if (S.wpos < read) S.wpos = read;                   /* fell behind: skip ahead */
    while (S.wpos < target) {
        u32 at = (u32)(S.wpos % RING_BYTES);
        u32 n = (u32)MIN(target - S.wpos, (u64)(RING_BYTES - at));
        n = MIN(n, (u32)sizeof S.mix) & ~3u;
        if (!n) break;
        snd_mix(S.mix, (int)(n / 4), 48000, 2);
        memcpy(S.ring + at, S.mix, n);
        S.wpos += n;
    }
}

/* ---- the codec -------------------------------------------------------------------------- */
static dwi2c_t *cbus;
static int cw(u8 reg, u16 v) { u8 b[3] = { reg, (u8)(v >> 8), (u8)v }; return dwi2c_xfer(cbus, 0x1c, b, 3, NULL, 0); }
static int cr(u8 reg, u16 *v) { u8 b[2]; int e = dwi2c_xfer(cbus, 0x1c, &reg, 1, b, 2); if (!e) *v = (u16)(b[0] << 8 | b[1]); return e; }
static int cpriv(u16 reg, u16 v) { return cw(0x6a, reg) | cw(0x6c, v); }   /* private registers: index, then data */
static void cupd(u8 reg, u16 mask, u16 v) { u16 o = 0; cr(reg, &o); cw(reg, (u16)((o & ~mask) | (v & mask))); }

static int codec_init(void) {
    cbus = audio_bus();
    if (!cbus) return -1;
    u16 id = 0;
    if (cr(0xff, &id) || id != 0x6271) { klog("speaker: no RT5670/5672 on I2C2 (id %04x)", id); return -1; }
    cw(0x00, 0);                                        /* reset */
    cupd(0x63, 0x00c0 | 0x0010, 0x0010);                /* PWR_ANLG1: VREF2 on, headphone off */
    thread_sleep_ms(100);
    cw(0x00, 0);
    u16 ver = 0;
    cr(0xfd, &ver);
    cw(0xc2, ver >= 4 ? 0x0980 : 0x0d00);               /* GPIO_CTRL3, by chip version */
    cpriv(0x14, 0x9a8a); cpriv(0x38, 0x1fe1); cpriv(0x3d, 0x3640); cw(0x8a, 0x0123);   /* Realtek's init list */
    /* bias: VREF1, MB, BG, VREF2; then fast VREF; LDO 5 */
    cupd(0x63, 0xa810, 0xa810);
    thread_sleep_ms(10);
    cupd(0x63, 0x4008, 0x4008);
    cupd(0x91, 0x0c00, 0x0000);                         /* CHARGE_PUMP: OSW off */
    cupd(0xfa, 0x0001, 0x0001);                         /* DIG_MISC */
    cupd(0x63, 0x0007, 0x0005);
    /* clocks: PLL1 from MCLK 19.2 MHz -> 24.576 MHz (Realtek's preset: N 30, K 3, M 3), system clock from PLL1 */
    cw(0x81, (u16)(30 << 7 | 3));
    cw(0x82, (u16)(3 << 12));
    cupd(0x80, 0xf800, 0x4000);                         /* GLB_CLK: SCLK = PLL1, PLL1 source = MCLK */
    cupd(0x64, 0x0200, 0x0200);                         /* PWR_ANLG2: PLL */
    /* I2S1: slave, I2S, 24-bit; 64 fs bit clock, pre-divider 2 */
    cupd(0x70, 0x8000 | 0x0080 | 0x0003 | 0x000c, 0x8000 | 0x0008);
    cupd(0x73, 0xf000, 0x9000);
    /* ASRC: the DAC filter follows I2S1 (the SoC's clock, 50 fs) */
    cupd(0x84, 0xf000, 0x1000);
    cupd(0x83, 0x0c00, 0x0c00);
    /* the path: IF1 -> DAC1 (unmuted) -> stereo DAC mixer (L1, R1) -> PDM1 (stereo DAC, unmuted) */
    cw(0x29, 0x8080);                                   /* AD_DA_MIXER: ADC mix muted, DAC1 from IF1, unmuted */
    cw(0x2a, 0x1616);                                   /* STO_DAC_MIXER: DAC L1 and R1 in */
    cupd(0x31, 0xf000, 0xa000);                         /* PDM_OUT_CTRL: PDM1 L/R from the stereo DAC, unmuted */
    cw(0x19, 0xafaf);                                   /* DAC1 digital volume: 0 dB */
    cupd(0x61, 0x9800, 0x9800);                         /* PWR_DIG1: I2S1, DAC L1, DAC R1 */
    cupd(0x62, 0x0880, 0x0880);                         /* PWR_DIG2: DAC stereo filter, PDM1 */
    u16 p1 = 0, p2 = 0, a1 = 0;
    cr(0x61, &p1); cr(0x62, &p2); cr(0x63, &a1);
    klog("speaker: RT5672 (version %d) set up: power %04x %04x %04x", ver, p1, p2, a1);
    return 0;
}

/* the codec's 19.2 MHz master clock: PMC platform clock 3 */
static int mclk_on(void) {
    u32 pbase = pci_read32(0, 0x1f, 0, 0x44) & 0xfffffe00u;
    if (!pbase) { klog("speaker: no PMC base"); return -1; }
    volatile u32 *clk = mm_map_mmio(pbase + 0x60 + 3 * 4, 4);
    if (!clk) return -1;
    u32 v = *clk;
    *clk = (v & ~7u) | 1u /* forced on */ | 4u /* 19.2 MHz */;
    klog("speaker: platform clock 3 %08x -> %08x", v, *clk);
    return 0;
}

/* ---- start ------------------------------------------------------------------------------- */
static u64 lpe_base(void) {
    if (pci_read32(0, 0x15, 0, 0) == 0x22a88086) {     /* PCI mode */
        pci_write32(0, 0x15, 0, 0x84, pci_read32(0, 0x15, 0, 0x84) & ~3u);   /* D0 */
        pci_write32(0, 0x15, 0, 0x04, pci_read32(0, 0x15, 0, 0x04) | 0x6);  /* memory, bus master */
        u64 b = pci_bar(0, 0x15, 0, 0);
        if (b) return b;
    }
    u32 g = venue_gnvs();                               /* ACPI mode: LPE0 in the firmware's NVS */
    return g ? *(volatile u32 *)(usize)(g + 335) : 0;
}

/* everything up to a playing stream; 0 or -1 (status says why) */
static int bringup(void) {
    u64 base = lpe_base();
    if (!base) { strlcpy(status, "no audio DSP found (PCI 00:15.0 or LPE0)", sizeof status); goto out; }
    S.lpe = mm_map_mmio(base, 0x200000);
    u64 csr = rd64(SHIM_CSR);
    klog("speaker: audio DSP at %llx, CSR %llx", base, csr);
    if (csr == ~0ull) { strlcpy(status, "the audio DSP does not answer (powered off?)", sizeof status); goto out; }

    vnode_t *n = vfs_lookup(FW_PATH);
    if (!n || n->dir) { strlcpy(status, FW_PATH " is missing", sizeof status); goto out; }
    u64 size = vfs_size(n);
    u8 *fw = kalloc(size);
    vfs_read(n, 0, fw, size);
    u8 *reef = NULL;
    for (u64 i = 0; i + 16 <= size && i < 4096; i += 4) if (!memcmp(fw + i, "Reef", 4)) { reef = fw + i; break; }
    if (!reef) { strlcpy(status, "sof-cht.ri: not a SOF image", sizeof status); kfree(fw); goto out; }

    /* the ring and its page table, in memory the DSP can reach */
    u64 ring = pmm_alloc_contig(RING_PAGES + 1);
    if (!ring || ring + (RING_PAGES + 1) * 4096 > 0x80000000ull) { strlcpy(status, "no DMA memory below 2 GiB", sizeof status); kfree(fw); goto out; }
    S.ring = (u8 *)(usize)ring;
    S.ptable = S.ring + RING_BYTES;
    memset(S.ring, 0, RING_BYTES);

    if (mclk_on() || codec_init()) { strlcpy(status, "the codec did not come up (see the log)", sizeof status); kfree(fw); goto out; }

    /* reset and stall the DSP, copy the firmware in, let it run */
    wr64(SHIM_IMRX, 3); wr64(SHIM_IMRD, 3);
    wr64(SHIM_CSR, rd64(SHIM_CSR) | CSR_RST | CSR_VECTOR | CSR_STALL);
    hal_delay_us(20);
    wr64(SHIM_CSR, rd64(SHIM_CSR) & ~CSR_RST);
    u32 nmod = *(u32 *)(reef + 8);
    u8 *p = reef + 16;
    int blocks = 0;
    for (u32 m = 0; m < nmod && p + 12 <= fw + size; m++) {
        u32 msize = *(u32 *)(p + 4), nblk = *(u32 *)(p + 8);
        u8 *b = p + 12;
        for (u32 i = 0; i < nblk && b + 12 <= fw + size; i++) {
            i32 type = *(i32 *)b;
            u32 bsize = *(u32 *)(b + 4), off = *(u32 *)(b + 8);
            if ((type == 1 || type == 2 || type == 3) && bsize && off + bsize <= 0x200000 && b + 12 + bsize <= fw + size) {
                copy_to(off, b + 12, bsize);
                blocks++;
            }
            b += 12 + bsize;
        }
        p += 12 + msize;
    }
    kfree(fw);
    S.dspbox = S.hostbox = MBOX;
    wr64(SHIM_CSR, rd64(SHIM_CSR) & ~CSR_STALL);
    u64 end = k_now_ms() + 1000;
    while ((rd64(SHIM_CSR) & CSR_PWAITMODE) && k_now_ms() < end) thread_sleep_ms(10);
    end = k_now_ms() + 3000;
    while (!fw_ready_seen && k_now_ms() < end) { rx_one(); thread_sleep_ms(2); }
    if (!fw_ready_seen) {
        fmt(status, sizeof status, "the DSP firmware did not start (%d blocks loaded, CSR %llx, IPCD %llx)", blocks, rd64(SHIM_CSR), rd64(SHIM_IPCD));
        goto out;
    }
    /* FW_READY: its version, then the mailbox windows */
    u32 ready[32];
    copy_from(ready, MBOX, sizeof ready);
    u16 *ver = (u16 *)&ready[7];
    S.abi = ready[16];                                  /* fw_version at [6]: hdr, 4 x u16, date[12], time[10], tag[6], abi */
    u32 ext = MBOX + ready[0];
    for (int guard = 0; guard < 8; guard++) {
        u32 eh[3];
        copy_from(eh, ext, 12);
        if (eh[1] != GLB_FW_READY || eh[0] < 12 || eh[0] > 1024) break;
        if (eh[2] == 1) {                               /* SOF_IPC_EXT_WINDOW */
            u32 win[64];
            copy_from(win, ext, MIN(eh[0], sizeof win));
            u32 nw = win[3];
            for (u32 i = 0; i < nw && i < 8; i++) {
                u32 *e = &win[4 + i * 6];               /* hdr, type, id, flags, size, offset */
                u32 at = MBOX + e[5];
                if (e[1] == 0) S.hostbox = at;
                else if (e[1] == 1) S.dspbox = at;
                else if (e[1] == 4) S.streambox = at;
            }
        }
        ext += eh[0];
    }
    klog("speaker: SOF %d.%d.%d (ABI %x) running, %d blocks; mailboxes host %x dsp %x stream %x",
         ver[0], ver[1], ver[2], S.abi, blocks, S.hostbox, S.dspbox, S.streambox);
    if (build_topology()) { strlcpy(status, "the DSP refused the pipeline (see the log)", sizeof status); goto out; }
    S.started_us = k_now_us();
    if (start_stream()) { strlcpy(status, "the DSP refused the stream (see the log)", sizeof status); goto out; }
    S.out = (snd_output_t){ "Speaker", 48000, 2, pump, NULL };
    snd_output_add(&S.out);
    strlcpy(status, "playing: SOF on the audio DSP, SSP2, RT5672 speaker path", sizeof status);
    klog("speaker: %s", status);
    return 0;
out:
    klog("speaker: %s", status);
    return -1;
}

static void speaker_thread(void *arg) {
    (void)arg;
    if (!bringup())
        for (;;) {                                      /* the sound thread pumps; report once a while */
            thread_sleep_ms(10000);
            if (S.positions || S.xruns) fmt(status, sizeof status, "playing: %d positions, %d underruns from the DSP", S.positions, S.xruns);
        }
    thread_exit();
}

void speaker_start(void) {
    if (!k.native || !k.is_venue) { strlcpy(status, k.native ? "not a Venue 8 Pro" : "firmware mode", sizeof status); return; }
    if (!hal_setting_get(u"QrtSpeaker", 1)) { strlcpy(status, "turned off (QrtSpeaker = 0)", sizeof status); return; }
    strlcpy(status, "starting", sizeof status);
    thread_create("speaker", speaker_thread, NULL, 0);
}
#else
void speaker_start(void) {}
#endif
