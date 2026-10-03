/* Host test for src/drivers/speaker.c (make check).
 *
 * A simulated Cherry Trail audio DSP: a 2 MiB window like the LPE's.  When the
 * driver releases the stall it "boots" - FW_READY with mailbox windows, as SOF
 * 2.1 does - and it answers every IPC.  Each message is decoded with Linux's own
 * SOF IPC3 structures (tests/sof, from Linux 6.12 include/sound/sof): sizes,
 * fields, UUIDs, the SSP2 settings of sof-cht-rt5670.tplg, the stream's page
 * table.  The firmware blocks must land where sof-cht.ri says, the pipelines
 * must connect host -> ... -> SSP2, and position messages must move the ring.
 * A simulated RT5672 on I2C2 records the codec set-up.
 * Build/run: make check */
#define SPEAKER_HOST_TEST
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "../src/drivers/speaker.c"

#include <sound/sof/header.h>
#include <sound/sof/topology.h>
#include <sound/sof/dai.h>
#include <sound/sof/stream.h>
#include <sound/sof/info.h>
#include <uapi/sound/sof/abi.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---- the kernel around the driver ----------------------------------------------------- */
kernel_t k;
void strlcpy(char *d, const char *s, usize cap) { if (!cap) return; usize n = strlen(s); if (n >= cap) n = cap - 1; memcpy(d, s, n); d[n] = 0; }
int fmt(char *buf, usize cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
void klog(const char *f, ...) { va_list ap; va_start(ap, f); if (getenv("VERBOSE")) { vprintf(f, ap); printf("\n"); } va_end(ap); }
static u64 now_us;
u64 k_now_us(void) { return now_us += 50; }
u64 k_now_ms(void) { return k_now_us() / 1000; }
void hal_delay_us(u32 us) { now_us += us; }
u32 hal_setting_get(const c16 *name, u32 def) { (void)name; return def; }
void thread_yield(void) {}
void thread_sleep_ms(u64 ms) { now_us += ms * 1000; }
void *thread_create(const char *n, void (*fn)(void *), void *a, u64 cr3) { (void)n; (void)fn; (void)a; (void)cr3; return NULL; }
void thread_exit(void) { exit(1); }
void *kalloc(usize n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
u32 venue_gnvs(void) { return 0; }

/* PCI: the DSP at 00:15.0, the PMC at 00:1f.0 */
static u8 *lpe_win;
static u32 pmc_regs[0x80];
u32 pci_read32(u8 b, u8 d, u8 f, u16 off) {
    (void)b; (void)f;
    if (d == 0x15 && off == 0) return 0x22a88086;
    if (d == 0x1f && off == 0x44) return 0xfed03000;
    return 0;
}
void pci_write32(u8 b, u8 d, u8 f, u16 off, u32 v) { (void)b; (void)d; (void)f; (void)off; (void)v; }
u64 pci_bar(u8 b, u8 d, u8 f, int bar) { (void)b; (void)d; (void)f; (void)bar; return 0x91200000; }
void *mm_map_mmio(u64 base, u64 size) {
    (void)size;
    if (base == 0x91200000) return lpe_win;
    if (base >= 0xfed03000 && base < 0xfed03200) return (u8 *)pmc_regs + (base - 0xfed03000);
    return NULL;
}
u64 pmm_alloc_contig(usize pages) {                        /* the DSP reaches the low 2 GiB */
    void *p = mmap(NULL, pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    return p == MAP_FAILED ? 0 : (u64)(usize)p;
}

/* the firmware file */
static u8 *fw_data; static u64 fw_len;
static vnode_t fw_node;
vnode_t *vfs_lookup(const char *path) { return !strcmp(path, FW_PATH) && fw_data ? &fw_node : NULL; }
u64 vfs_size(vnode_t *n) { (void)n; return fw_len; }
i64 vfs_read(vnode_t *n, u64 off, void *buf, u64 len) { (void)n; memcpy(buf, fw_data + off, len); return (i64)len; }

/* the sound core: a ramp, so the ring's contents can be checked */
static snd_output_t *out_reg;
static i16 ramp;
void snd_output_add(snd_output_t *o) { out_reg = o; }
void snd_mix(i16 *o, int frames, int rate, int channels) { (void)rate; for (int i = 0; i < frames * channels; i++) o[i] = ++ramp; }

/* ---- the RT5672 on I2C2 ---------------------------------------------------------------- */
static u16 reg[256], priv[256];
static dwi2c_t i2c2;
dwi2c_t *audio_bus(void) { return &i2c2; }
int dwi2c_xfer(dwi2c_t *c, u8 addr, const u8 *w, int wlen, u8 *r, int rlen) {
    (void)c;
    if (addr != 0x1c || wlen < 1) return DW_EABORT;
    if (wlen == 3) {
        u16 v = (u16)(w[1] << 8 | w[2]);
        if (w[0] == 0x00) { memset(reg, 0, sizeof reg); reg[0x29] = 0x8080; reg[0x2a] = 0x5656; reg[0x31] = 0x5f00; reg[0x63] = 0x00c3; reg[0x70] = 0x8000; reg[0x73] = 0x7770; reg[0x19] = 0xafaf; reg[0x62] = 1; }
        else if (w[0] == 0x6c) priv[reg[0x6a] & 0xff] = v;
        else reg[w[0]] = v;
        return 0;
    }
    if (wlen == 1 && rlen == 2) {
        u16 v = w[0] == 0xff ? 0x6271 : w[0] == 0xfd ? 4 : reg[w[0]];
        r[0] = (u8)(v >> 8); r[1] = (u8)v;
        return 0;
    }
    return DW_EABORT;
}

/* ---- the DSP -------------------------------------------------------------------------- */
#define HOSTBOX 0x144000
#define DSPBOX  0x144400
#define STREAMBOX 0x144800
static int booted, n_msgs, n_comp, n_buf, n_pipe, n_conn, n_done, n_dai, started;
static u32 comp_type[64];                         /* by id */
static u32 conn_from[16], conn_to[16];
static u32 host_id, posn_off = 0x40;

static void reply(u32 cmd, u32 extra_a, u32 extra_b, int n_extra) {
    u32 r[5] = { 12 + 4 * (u32)n_extra, SOF_IPC_GLB_REPLY | (cmd & 0x0fff0000), 0, extra_a, extra_b };
    memcpy(lpe_win + HOSTBOX, r, r[0]);
    *(u64 *)(lpe_win + SHIM_IPCX) = IPC_DONE;
}

static void boot(void) {
    struct sof_ipc_fw_ready fr;
    memset(&fr, 0, sizeof fr);
    fr.hdr.size = sizeof fr; fr.hdr.cmd = SOF_IPC_FW_READY;
    fr.dspbox_offset = DSPBOX; fr.hostbox_offset = HOSTBOX; fr.dspbox_size = fr.hostbox_size = 0x400;
    fr.version.major = 2; fr.version.minor = 1; fr.version.micro = 1; fr.version.abi_version = SOF_ABI_VER(3, 20, 0);
    memcpy(lpe_win + MBOX, &fr, sizeof fr);
    struct { struct sof_ipc_ext_data_hdr h; u32 n; struct sof_ipc_window_elem e[3]; } __attribute__((packed)) w;
    memset(&w, 0, sizeof w);
    w.h.hdr.size = sizeof w; w.h.hdr.cmd = SOF_IPC_FW_READY; w.h.type = SOF_IPC_EXT_WINDOW; w.n = 3;
    w.e[0] = (struct sof_ipc_window_elem){ { sizeof w.e[0] }, SOF_IPC_REGION_DOWNBOX, 0, 0, 0x400, 0x000 };
    w.e[1] = (struct sof_ipc_window_elem){ { sizeof w.e[0] }, SOF_IPC_REGION_UPBOX, 0, 0, 0x400, 0x400 };
    w.e[2] = (struct sof_ipc_window_elem){ { sizeof w.e[0] }, SOF_IPC_REGION_STREAM, 0, 0, 0x400, 0x800 };
    memcpy(lpe_win + MBOX + sizeof fr, &w, sizeof w);
    *(u64 *)(lpe_win + SHIM_IPCD) = IPC_BUSY | SOF_IPC_FW_READY;
    booted = 1;
}

static void check_comp(const u8 *m, u32 size) {
    const struct sof_ipc_comp *c = (const void *)m;
    usize want = c->type == SOF_COMP_HOST ? sizeof(struct sof_ipc_comp_host) : c->type == SOF_COMP_VOLUME ? sizeof(struct sof_ipc_comp_volume) :
                 c->type == SOF_COMP_MIXER ? sizeof(struct sof_ipc_comp_mixer) : c->type == SOF_COMP_DAI ? sizeof(struct sof_ipc_comp_dai) : 0;
    CHECK(want && size == want + 16 && c->ext_data_length == 16, "comp %u type %u: size %u, want %zu + 16 bytes of UUID", c->id, c->type, size, want);
    const u8 *uuid = m + want;
    const u8 *expect = c->type == SOF_COMP_HOST ? uuid_host : c->type == SOF_COMP_VOLUME ? uuid_volume : c->type == SOF_COMP_MIXER ? uuid_mixer : uuid_dai;
    CHECK(!memcmp(uuid, expect, 16), "comp %u: UUID", c->id);
    const struct sof_ipc_comp_config *cfg = (const void *)(m + sizeof(struct sof_ipc_comp));
    CHECK(cfg->hdr.size == sizeof *cfg, "comp %u: config size %u", c->id, cfg->hdr.size);
    if (c->type == SOF_COMP_DAI) {
        const struct sof_ipc_comp_dai *d = (const void *)m;
        CHECK(d->type == SOF_DAI_INTEL_SSP && d->dai_index == 2 && d->direction == SOF_IPC_STREAM_PLAYBACK && cfg->frame_fmt == SOF_IPC_FRAME_S24_4LE,
              "DAI component: type %u index %u dir %u fmt %u", d->type, d->dai_index, d->direction, cfg->frame_fmt);
    }
    if (c->type == SOF_COMP_VOLUME) {
        const struct sof_ipc_comp_volume *v = (const void *)m;
        CHECK(v->channels == 2 && v->max_value == 0x10000 && v->min_value < v->max_value, "volume %u: channels %u range %x..%x", c->id, v->channels, v->min_value, v->max_value);
    }
    if (c->type == SOF_COMP_HOST) {
        const struct sof_ipc_comp_host *h = (const void *)m;
        CHECK(h->direction == SOF_IPC_STREAM_PLAYBACK && cfg->periods_sink == 2, "host: direction %u periods %u", h->direction, cfg->periods_sink);
        host_id = c->id;
    }
    if (c->id < 64) comp_type[c->id] = c->type;
    n_comp++;
}

static void check_pcm(const u8 *m, u32 size) {
    const struct sof_ipc_pcm_params *p = (const void *)m;
    CHECK(size == sizeof *p && p->params.hdr.size == sizeof p->params, "PCM params: size %u (want %zu), params %u (want %zu)", size, sizeof *p, p->params.hdr.size, sizeof p->params);
    CHECK(p->comp_id == host_id, "PCM params for component %u, the host is %u", p->comp_id, host_id);
    const struct sof_ipc_stream_params *s = &p->params;
    CHECK(s->rate == 48000 && s->channels == 2 && s->frame_fmt == SOF_IPC_FRAME_S16_LE && s->sample_container_bytes == 2 && s->sample_valid_bytes == 2,
          "PCM params: %u Hz, %u channels, format %u, %u/%u bytes", s->rate, s->channels, s->frame_fmt, s->sample_valid_bytes, s->sample_container_bytes);
    CHECK(s->buffer.pages == RING_PAGES && s->buffer.size == RING_BYTES && s->buffer.phy_addr < 0x80000000u && s->host_period_bytes, "PCM buffer: %u pages, %u bytes at %x, period %u",
          s->buffer.pages, s->buffer.size, s->buffer.phy_addr, s->host_period_bytes);
    /* the page table, read back the way the DSP reads it */
    const u8 *pt = (const u8 *)(usize)s->buffer.phy_addr;
    for (u32 i = 0; i < s->buffer.pages; i++) {
        const u8 *e = pt + (5 * i) / 2;
        u32 v = (u32)e[0] | (u32)e[1] << 8 | (u32)e[2] << 16 | (u32)e[3] << 24;
        u32 pfn = (i & 1) ? (v >> 4) & 0xfffff : v & 0xfffff;
        CHECK(pfn == (u32)(((usize)S.ring + i * 4096) >> 12), "page table entry %u: %x, want %x", i, pfn, (u32)(((usize)S.ring + i * 4096) >> 12));
    }
}

static void check_dai_config(const u8 *m, u32 size) {
    const struct sof_ipc_dai_config *c = (const void *)m;
    CHECK(size >= offsetof(struct sof_ipc_dai_config, ssp) + sizeof c->ssp, "DAI config: %u bytes", size);
    CHECK(c->type == SOF_DAI_INTEL_SSP && c->dai_index == 2, "DAI config: type %u index %u", c->type, c->dai_index);
    CHECK(c->format == (SOF_DAI_FMT_I2S | SOF_DAI_FMT_CBC_CFC | SOF_DAI_FMT_NB_NF), "DAI config: format %x", c->format);
    const struct sof_ipc_dai_ssp_params *s = &c->ssp;
    CHECK(s->mclk_rate == 19200000 && s->bclk_rate == 2400000 && s->fsync_rate == 48000 && s->tdm_slots == 2 && s->tdm_slot_width == 25 &&
          s->sample_valid_bits == 24 && s->tx_slots == 3 && s->rx_slots == 3 && s->mclk_direction == 1 && s->mclk_id == 0,
          "SSP2: mclk %u bclk %u fsync %u slots %u x %u, %u bits, tx %x rx %x, mclk dir %u id %u", s->mclk_rate, s->bclk_rate, s->fsync_rate,
          s->tdm_slots, s->tdm_slot_width, s->sample_valid_bits, s->tx_slots, s->rx_slots, s->mclk_direction, s->mclk_id);
    n_dai++;
}

static void ipc(void) {
    u32 hdr[2];
    memcpy(hdr, lpe_win + HOSTBOX, 8);
    u32 size = hdr[0], cmd = hdr[1];
    const u8 *m = lpe_win + HOSTBOX;
    n_msgs++;
    switch (cmd) {
    case SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_COMP_NEW: check_comp(m, size); break;
    case SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_BUFFER_NEW: {
        const struct sof_ipc_buffer *b = (const void *)m;
        CHECK(size == sizeof *b && b->comp.type == SOF_COMP_BUFFER && b->size == 768 && (b->caps & SOF_MEM_CAPS_RAM), "buffer %u: size %u, %u bytes, caps %x", b->comp.id, size, b->size, b->caps);
        if (b->comp.id < 64) comp_type[b->comp.id] = SOF_COMP_BUFFER;
        n_buf++;
        break;
    }
    case SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_PIPE_NEW: {
        const struct sof_ipc_pipe_new *p = (const void *)m;
        CHECK(size == sizeof *p && p->period == 1000 && p->time_domain == SOF_TIME_DOMAIN_DMA, "pipeline %u: size %u period %u domain %u", p->pipeline_id, size, p->period, p->time_domain);
        n_pipe++;
        break;
    }
    case SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_COMP_CONNECT: {
        const struct sof_ipc_pipe_comp_connect *c = (const void *)m;
        CHECK(size == sizeof *c && c->source_id < 64 && c->sink_id < 64 && comp_type[c->source_id] && comp_type[c->sink_id], "connect %u -> %u: unknown components", c->source_id, c->sink_id);
        if (n_conn < 16) { conn_from[n_conn] = c->source_id; conn_to[n_conn] = c->sink_id; }
        n_conn++;
        break;
    }
    case SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_PIPE_COMPLETE: CHECK(size == sizeof(struct sof_ipc_pipe_ready), "pipe complete: size %u", size); n_done++; break;
    case SOF_IPC_GLB_DAI_MSG | SOF_IPC_DAI_CONFIG: check_dai_config(m, size); break;
    case SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_PCM_PARAMS: check_pcm(m, size); reply(cmd, host_id, posn_off, 2); return;
    case SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_TRIG_START: {
        const struct sof_ipc_stream *s = (const void *)m;
        CHECK(size == sizeof *s && s->comp_id == host_id, "start: size %u, component %u", size, s->comp_id);
        started = 1;
        break;
    }
    default: CHECK(0, "unexpected IPC %08x (%u bytes)", cmd, size);
    }
    reply(cmd, 0, 0, 0);
}

static void fake_dsp_write(u32 off, u64 v) {
    if (off == SHIM_CSR && !(v & (CSR_RST | CSR_STALL)) && !booted) boot();
    if (off == SHIM_IPCX && (v & IPC_BUSY)) ipc();
}

int main(void) {
    k.is_venue = k.native = 1;
    FILE *f = fopen("firmware/sof-cht.ri", "rb");
    if (!f) { printf("test_speaker: firmware/sof-cht.ri missing\n"); return 1; }
    fseek(f, 0, SEEK_END); fw_len = (u64)ftell(f); fseek(f, 0, SEEK_SET);
    fw_data = malloc(fw_len);
    if (fread(fw_data, 1, fw_len, f) != fw_len) return 1;
    fclose(f);
    lpe_win = calloc(1, 0x200000);
    *(u64 *)(lpe_win + SHIM_CSR) = 0;

    int r = bringup();
    CHECK(r == 0, "bring-up failed: %s", status);

    /* the firmware's blocks are where the image says */
    u8 *reef = fw_data;
    while (memcmp(reef, "Reef", 4)) reef++;
    u8 *mod = reef + 16, *blk = mod + 12;
    int nblk = 0, same = 0;
    for (u32 i = 0; i < *(u32 *)(mod + 8); i++) {
        u32 bsize = *(u32 *)(blk + 4), off = *(u32 *)(blk + 8);
        nblk++;
        same += !memcmp(lpe_win + off, blk + 12, bsize);
        blk += 12 + bsize;
    }
    CHECK(same == nblk && nblk == 20, "firmware: %d of %d blocks in place", same, nblk);
    CHECK(S.hostbox == HOSTBOX && S.dspbox == DSPBOX && S.streambox == STREAMBOX, "mailboxes %x %x %x", S.hostbox, S.dspbox, S.streambox);
    CHECK(S.abi == SOF_ABI_VER(3, 20, 0), "ABI %x", S.abi);

    /* the topology: 2 pipelines, 5 components (host, 2 volumes, mixer, DAI), 4 buffers, 8 connections, both complete, SSP2 configured */
    CHECK(n_pipe == 2 && n_comp == 5 && n_buf == 4 && n_conn == 8 && n_done == 2 && n_dai == 2 && started,
          "topology: %d pipelines, %d components, %d buffers, %d connections, %d complete, %d DAI configs, started %d",
          n_pipe, n_comp, n_buf, n_conn, n_done, n_dai, started);
    /* follow the connections from the host: it must reach the SSP2 DAI */
    u32 at = host_id;
    int steps = 0;
    while (comp_type[at] != SOF_COMP_DAI && steps < 16) {
        int i = 0;
        while (i < n_conn && conn_from[i] != at) i++;
        if (i == n_conn) break;
        at = conn_to[i]; steps++;
    }
    CHECK(comp_type[at] == SOF_COMP_DAI && steps == 8, "the host's data reaches %s after %d steps", comp_type[at] == SOF_COMP_DAI ? "the DAI" : "a dead end", steps);

    /* the codec: clocks, I2S slave 24-bit, the speaker path powered and unmuted */
    CHECK(reg[0x80] == 0x4000 && reg[0x81] == (30 << 7 | 3) && reg[0x82] == 0x3000, "codec clocks: GLB %04x PLL %04x %04x", reg[0x80], reg[0x81], reg[0x82]);
    CHECK((reg[0x70] & 0x800c) == 0x8008, "codec I2S1: %04x", reg[0x70]);
    CHECK(reg[0x2a] == 0x1616 && (reg[0x31] & 0xf000) == 0xa000 && !(reg[0x29] & 0x4040), "codec mixers: %04x %04x %04x", reg[0x29], reg[0x2a], reg[0x31]);
    CHECK((reg[0x61] & 0x9800) == 0x9800 && (reg[0x62] & 0x0880) == 0x0880 && (reg[0x63] & 0xe818) == 0xe818 && (reg[0x64] & 0x200), "codec power: %04x %04x %04x %04x", reg[0x61], reg[0x62], reg[0x63], reg[0x64]);
    CHECK(priv[0x14] == 0x9a8a && priv[0x38] == 0x1fe1 && priv[0x3d] == 0x3640, "codec private registers");
    CHECK((pmc_regs[(0x60 + 12) / 4] & 7) == 5, "platform clock 3: %x", pmc_regs[(0x60 + 12) / 4]);

    /* playing: the output is registered; positions from the DSP move the ring */
    CHECK(out_reg && out_reg->rate == 48000, "no 48 kHz output registered");
    out_reg->pump(out_reg);
    u64 w0 = S.wpos;
    CHECK(w0 >= RING_BYTES / 2, "first fill: %llu bytes", (unsigned long long)w0);
    for (int round = 1; round <= 6; round++) {
        struct sof_ipc_stream_posn pos;
        memset(&pos, 0, sizeof pos);
        pos.rhdr.hdr.size = sizeof pos; pos.rhdr.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_POSITION | host_id;
        pos.host_posn = (u64)(round * RING_BYTES / 4) % RING_BYTES;
        memcpy(lpe_win + STREAMBOX + posn_off, &pos, sizeof pos);
        memcpy(lpe_win + DSPBOX, &pos, 8);
        *(u64 *)(lpe_win + SHIM_IPCD) = IPC_BUSY | pos.rhdr.hdr.cmd;
        out_reg->pump(out_reg);
        CHECK(!(*(u64 *)(lpe_win + SHIM_IPCD) & IPC_BUSY) && (*(u64 *)(lpe_win + SHIM_IPCD) & IPC_DONE), "position %d not acknowledged", round);
    }
    /* the DSP has read 6 quarters (one wrap); the driver stays half a ring ahead of it */
    CHECK(S.positions == 6 && S.dsp_wraps == 1 && S.wpos == 6 * RING_BYTES / 4 + RING_BYTES / 2, "ring: %d positions, %llu wraps, written %llu (want %u)", S.positions,
          (unsigned long long)S.dsp_wraps, (unsigned long long)S.wpos, 6 * RING_BYTES / 4 + RING_BYTES / 2);
    i16 *ring = (i16 *)S.ring;
    int ok = 1;
    for (int i = 1; i < RING_BYTES / 2; i++) if ((i16)(ring[i] - ring[i - 1]) != 1 && i != (int)((S.wpos % RING_BYTES) / 2)) { ok = 0; break; }
    CHECK(ok, "the ring does not hold the mix in order");

    if (fails) { printf("test_speaker: %d failures\n", fails); return 1; }
    printf("test_speaker: SOF firmware load, FW_READY windows, %d IPC messages (Linux's SOF structures), SSP2, RT5672 path, ring positions pass\n", n_msgs);
    return 0;
}
