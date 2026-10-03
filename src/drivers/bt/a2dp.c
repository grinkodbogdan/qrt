/*
 * a2dp.c - Bluetooth headphones and speakers: A2DP, the Advanced Audio Distribution
 * Profile, as a source, over AVDTP (the Audio/Video Distribution Transport Protocol).
 *
 * When we connect (the Bluetooth app): an AVDTP signalling channel (L2CAP PSM 25);
 * DISCOVER lists the device's stream end-points; GET_CAPABILITIES finds an audio sink
 * that takes SBC; SET_CONFIGURATION picks 44.1 kHz (else 48), stereo, 16 blocks,
 * 8 subbands, loudness, bitpool up to 53 - the usual high quality; OPEN; a second
 * L2CAP channel to PSM 25 carries the media; START.  When headphones connect to us
 * (they often reconnect by themselves), we answer the same commands from our side: one
 * stream end-point, an SBC source.
 *
 * Streaming: the sound core sees an output at the chosen rate.  Its pump (the sound
 * thread, every few ms) mixes and encodes as many SBC frames as real time asks for,
 * 50 ms ahead, and packs them into RTP media packets (as many frames as the L2CAP MTU
 * takes); the bluetooth thread sends them as the controller has room.  If the link
 * falls behind, old audio is dropped rather than letting the delay grow.
 */
#include "bt.h"
#include "sbc.h"
#include "../../kernel/sound.h"

enum { A_IDLE, A_SIG, A_DISCOVER, A_GETCAP, A_SETCONF, A_OPEN, A_MEDIA, A_START, A_STREAMING, A_SUSPENDED };
enum { DISCOVER = 1, GET_CAPS, SET_CONF, GET_CONF, RECONF, OPEN, START, CLOSE, SUSPEND, ABORT, SECURITY, GET_ALL_CAPS, DELAY_REPORT };

#define LOCAL_SEID 1
#define PKT_Q 8
#define PKT_MAX 1024

static struct {
    int state;
    u16 handle;
    u8 addr[6];
    char name[48];
    int initiator;
    l2cap_ch_t *sig, *media;
    u8 seps[8]; int nseps, sep_i;          /* audio sinks the device listed */
    u8 remote_seid;
    u8 label, wait_sig;                    /* our pending command */
    u64 since;
    /* the configuration */
    int rate, channels, blocks, bitpool;
    sbc_t sbc;
    /* streaming */
    snd_output_t out;
    int out_added;
    char out_name[64];
    u64 t0, sent;                          /* when streaming started; PCM frames encoded since */
    u16 seq;
    u32 ts;
    u8 q[PKT_Q][PKT_MAX];
    u16 qlen[PKT_Q];
    volatile u32 qhead, qtail;
    u32 dropped;
} A;

static void set_status(const char *f) { bt_set_audio_status(f, A.name); }

static void media_open(l2cap_ch_t *ch);
static void media_closed(l2cap_ch_t *ch);
static const l2cap_ops_t media_ops = { media_open, NULL, media_closed };

/* ---- signalling ------------------------------------------------------------------------------ */
static void av_send(u8 label, int msg_type, u8 signal, const u8 *p, int len) {
    u8 b[64];
    if (!A.sig || len > 62) return;
    b[0] = (u8)(label << 4 | msg_type);                            /* single packet */
    b[1] = signal;
    if (len) memcpy(b + 2, p, (usize)len);
    l2cap_send(A.sig, b, 2 + len);
}

static void av_command(u8 signal, const u8 *p, int len) {
    A.label = (u8)((A.label + 1) & 15);
    A.wait_sig = signal;
    A.since = bt_now_ms();
    av_send(A.label, 0, signal, p, len);
}

static void av_accept(u8 label, u8 signal, const u8 *p, int len) { av_send(label, 2, signal, p, len); }
static void av_reject(u8 label, u8 signal, u8 err) { u8 e = err; av_send(label, 3, signal, &e, 1); }

/* our end-point's capabilities: SBC, 44.1/48 kHz, mono or stereo, any block length, 8 subbands, loudness */
static const u8 our_caps[] = { 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x30 | 0x08 | 0x02, 0xf0 | 0x04 | 0x01, 2, 53 };

/* read an SBC media codec element (category 7) out of service capabilities */
static const u8 *find_sbc(const u8 *p, int len) {
    for (int o = 0; o + 2 <= len; o += 2 + p[o + 1]) {
        if (o + 2 + p[o + 1] > len) break;
        if (p[o] == 0x07 && p[o + 1] >= 6 && (p[o + 2] >> 4) == 0 && p[o + 3] == 0x00) return p + o + 4;   /* audio, SBC */
    }
    return NULL;
}

/* choose a configuration from the sink's SBC capabilities (4 bytes) */
static int choose(const u8 *c, u8 out[4]) {
    u8 f = c[0] & 0x30, m = c[0] & 0x0f, b = c[1] & 0xf0, sb = c[1] & 0x0c, al = c[1] & 0x03;
    if (!f || !(sb & 0x04) || !(al & 0x01) || !(m & 0x0a)) return 0;   /* 44.1/48, 8 subbands, loudness, mono/stereo */
    u8 freq = (f & 0x20) ? 0x20 : 0x10;
    u8 mode = (m & 0x02) ? 0x02 : 0x08;
    u8 blk = (b & 0x10) ? 0x10 : (b & 0x20) ? 0x20 : (b & 0x40) ? 0x40 : 0x80;
    int maxbp = c[3], minbp = c[2];
    int bp = MIN(maxbp, mode == 0x02 ? 53 : 31);
    if (bp < minbp || bp < 2) return 0;
    out[0] = freq | mode; out[1] = blk | 0x04 | 0x01; out[2] = (u8)minbp; out[3] = (u8)bp;
    return 1;
}

static void apply_config(const u8 *cfg) {
    A.rate = (cfg[0] & 0x20) ? 44100 : (cfg[0] & 0x10) ? 48000 : (cfg[0] & 0x40) ? 32000 : 16000;
    A.channels = (cfg[0] & 0x08) ? 1 : 2;
    A.blocks = (cfg[1] & 0x10) ? 16 : (cfg[1] & 0x20) ? 12 : (cfg[1] & 0x40) ? 8 : 4;
    A.bitpool = cfg[3];
    sbc_init(&A.sbc, A.rate, A.channels, A.blocks, A.bitpool);
    klog("bt: A2DP %s: SBC %d Hz, %s, %d blocks, bitpool %d (%d kbit/s)", A.name, A.rate, A.channels == 2 ? "stereo" : "mono",
         A.blocks, A.bitpool, sbc_frame_len(&A.sbc) * 8 * A.rate / sbc_samples(&A.sbc) / 1000);
}

/* ---- streaming ------------------------------------------------------------------------------- */
static void pump(snd_output_t *o) {
    (void)o;
    if (A.state != A_STREAMING || !A.media) return;
    u64 now = bt_now_ms();
    int fs = sbc_samples(&A.sbc), flen = sbc_frame_len(&A.sbc);
    int per_pkt = MIN(15, (l2cap_mtu(A.media) - 13) / flen);
    if (per_pkt < 1) return;
    i64 due = (i64)((now - A.t0) * (u64)A.rate / 1000) + A.rate / 20 - (i64)A.sent;   /* real time, 50 ms ahead */
    if (due > A.rate / 3) {                                        /* far behind (the link stalled): skip ahead */
        A.sent += (u64)(due - A.rate / 20);
        A.ts += (u32)(due - A.rate / 20);
        A.dropped++;
        due = A.rate / 20;
    }
    static i16 pcm[2 * 128];
    while (due >= per_pkt * fs && A.qhead - A.qtail < PKT_Q) {
        u8 *b = A.q[A.qhead % PKT_Q];
        b[0] = 0x80; b[1] = 0x60;                                  /* RTP 2, payload type 96 */
        b[2] = (u8)(A.seq >> 8); b[3] = (u8)A.seq;
        b[4] = (u8)(A.ts >> 24); b[5] = (u8)(A.ts >> 16); b[6] = (u8)(A.ts >> 8); b[7] = (u8)A.ts;
        b[8] = 0; b[9] = 0; b[10] = 0; b[11] = 1;                  /* SSRC 1 */
        b[12] = (u8)per_pkt;                                       /* SBC payload header: frame count */
        int n = 13;
        for (int f = 0; f < per_pkt; f++) {
            snd_mix(pcm, fs, A.rate, A.channels);
            n += sbc_encode(&A.sbc, pcm, b + n);
        }
        A.qlen[A.qhead % PKT_Q] = (u16)n;
        A.qhead++;
        A.seq++;
        A.ts += (u32)(per_pkt * fs);
        A.sent += (u64)(per_pkt * fs);
        due -= per_pkt * fs;
    }
}

static void stream_start(void) {
    A.state = A_STREAMING;
    A.t0 = bt_now_ms();
    A.sent = 0; A.seq = 1; A.ts = 0;
    A.qhead = A.qtail = 0;
    sbc_init(&A.sbc, A.rate, A.channels, A.blocks, A.bitpool);
    if (!A.out_added) {
        fmt(A.out_name, sizeof A.out_name, "Bluetooth: %s", A.name);
        A.out = (snd_output_t){ A.out_name, A.rate, A.channels, pump, NULL };
        snd_output_add(&A.out);
        A.out_added = 1;
    }
    set_status("Playing on %s");
}

static void stream_stop(void) {
    if (A.out_added) { snd_output_remove(&A.out); A.out_added = 0; }
    if (A.state == A_STREAMING) A.state = A_SUSPENDED;
}

/* ---- the initiator's steps ------------------------------------------------------------------------ */
static void next_getcap(void) {
    while (A.sep_i < A.nseps) {
        u8 s = (u8)(A.seps[A.sep_i] << 2);
        A.state = A_GETCAP;
        av_command(GET_CAPS, &s, 1);
        return;
    }
    set_status("%s takes no audio format QRT can send (SBC)");
    hci_disconnect(A.handle);
}

static void responded(u8 signal, int ok, const u8 *p, int len) {
    if (signal != A.wait_sig) return;
    A.wait_sig = 0;
    if (!ok) {
        if (signal == GET_CAPS) { A.sep_i++; next_getcap(); return; }
        klog("bt: AVDTP command %u rejected (error %02x)", signal, len ? p[0] : 0);
        set_status("%s refused the audio stream");
        hci_disconnect(A.handle);
        return;
    }
    switch (signal) {
    case DISCOVER:
        A.nseps = A.sep_i = 0;
        for (int i = 0; i + 1 < len && A.nseps < 8; i += 2) {
            int seid = p[i] >> 2, inuse = (p[i] >> 1) & 1, media = p[i + 1] >> 4, tsep = (p[i + 1] >> 3) & 1;
            if (!inuse && media == 0 && tsep == 1) A.seps[A.nseps++] = (u8)seid;
        }
        if (!A.nseps) { set_status("%s has no free audio sink"); hci_disconnect(A.handle); return; }
        next_getcap();
        break;
    case GET_CAPS: case GET_ALL_CAPS: {
        const u8 *c = find_sbc(p, len);
        u8 cfg[4];
        if (!c || !choose(c, cfg)) { A.sep_i++; next_getcap(); return; }
        A.remote_seid = A.seps[A.sep_i];
        u8 sc[14] = { (u8)(A.remote_seid << 2), LOCAL_SEID << 2, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, cfg[0], cfg[1], cfg[2], cfg[3] };
        apply_config(cfg);
        A.state = A_SETCONF;
        av_command(SET_CONF, sc, 12);
        break;
    }
    case SET_CONF: {
        u8 s = (u8)(A.remote_seid << 2);
        A.state = A_OPEN;
        av_command(OPEN, &s, 1);
        break;
    }
    case OPEN:
        A.state = A_MEDIA;
        A.since = bt_now_ms();
        A.media = l2cap_connect(A.handle, 25, &media_ops, NULL);
        break;
    case START:
        stream_start();
        break;
    case SUSPEND:
        stream_stop();
        break;
    }
}

/* ---- commands from the device (it can drive the stream too) --------------------------------- */
static void incoming(u8 label, u8 signal, const u8 *p, int len) {
    switch (signal) {
    case DISCOVER: {
        u8 r[2] = { (u8)(LOCAL_SEID << 2 | (A.state >= A_OPEN ? 0x02 : 0)), 0x00 };   /* audio source; in use once configured */
        av_accept(label, signal, r, 2);
        break;
    }
    case GET_CAPS: case GET_ALL_CAPS:
        if (len < 1 || (p[0] >> 2) != LOCAL_SEID) { av_reject(label, signal, 0x12); break; }   /* BAD_ACP_SEID */
        av_accept(label, signal, our_caps, sizeof our_caps);
        break;
    case SET_CONF: {
        if (len < 2 || (p[0] >> 2) != LOCAL_SEID) { u8 e[2] = { 0, 0x12 }; av_send(label, 3, signal, e, 2); break; }
        const u8 *c = find_sbc(p + 2, len - 2);
        if (!c || !(c[0] & 0x30) || !(c[0] & 0x0a) || !(c[1] & 0x04) || !(c[1] & 0x01)) { u8 e[2] = { 0x07, 0x29 }; av_send(label, 3, signal, e, 2); break; }
        A.remote_seid = (u8)(p[1] >> 2);
        apply_config(c);
        A.state = A_OPEN;
        av_accept(label, signal, NULL, 0);
        break;
    }
    case GET_CONF: {
        u8 r[10] = { 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0, 0, 2, (u8)A.bitpool };
        r[6] = (u8)((A.rate == 44100 ? 0x20 : 0x10) | (A.channels == 2 ? 0x02 : 0x08));
        r[7] = (u8)((A.blocks == 16 ? 0x10 : A.blocks == 12 ? 0x20 : A.blocks == 8 ? 0x40 : 0x80) | 0x05);
        av_accept(label, signal, r, 10);
        break;
    }
    case OPEN: A.state = A_MEDIA; A.since = bt_now_ms(); av_accept(label, signal, NULL, 0); break;   /* the media channel comes next */
    case START:
        av_accept(label, signal, NULL, 0);
        if (A.media) stream_start();
        break;
    case SUSPEND: av_accept(label, signal, NULL, 0); stream_stop(); set_status("%s paused the stream"); break;
    case CLOSE: case ABORT:
        av_accept(label, signal, NULL, 0);
        stream_stop();
        A.state = A_SIG;
        break;
    case DELAY_REPORT: av_accept(label, signal, NULL, 0); break;
    case SECURITY: case RECONF: av_reject(label, signal, 0x1e); break;          /* not supported */
    default: av_send(label, 1, signal, NULL, 0); break;                     /* general reject */
    }
}

static void sig_data(l2cap_ch_t *ch, const u8 *p, int len) {
    (void)ch;
    if (len < 2) return;
    u8 label = p[0] >> 4, type = p[0] & 3, pkt = (p[0] >> 2) & 3, signal = p[1] & 0x3f;
    if (pkt != 0) return;                                          /* fragmented signals: not used by anyone in practice */
    if (type == 0) incoming(label, signal, p + 2, len - 2);
    else if (label == A.label) responded(signal, type == 2, p + 2, len - 2);
}

static void sig_open(l2cap_ch_t *ch) {
    A.sig = ch;
    if (A.initiator && A.state == A_SIG) { A.state = A_DISCOVER; av_command(DISCOVER, NULL, 0); }
}

static void sig_closed(l2cap_ch_t *ch) {
    (void)ch;
    A.sig = NULL;
    stream_stop();
    if (A.state != A_IDLE) A.state = A_SIG;
}

static void media_open(l2cap_ch_t *ch) {
    A.media = ch;
    klog("bt: A2DP media channel open (MTU %d)", l2cap_mtu(ch));
    if (A.initiator && A.state == A_MEDIA) { u8 s = (u8)(A.remote_seid << 2); A.state = A_START; av_command(START, &s, 1); }
}

static void media_closed(l2cap_ch_t *ch) { (void)ch; A.media = NULL; stream_stop(); }

static const l2cap_ops_t sig_ops = { sig_open, sig_data, sig_closed };

/* PSM 25 from the device: the first channel signals, the second carries media */
static void in_open(l2cap_ch_t *ch) { if (!A.sig || A.sig == ch) sig_open(ch); else media_open(ch); }
static void in_data(l2cap_ch_t *ch, const u8 *p, int len) { if (ch == A.sig) sig_data(ch, p, len); }
static void in_closed(l2cap_ch_t *ch) { if (ch == A.sig) sig_closed(ch); else if (ch == A.media) media_closed(ch); }
static const l2cap_ops_t in_ops = { in_open, in_data, in_closed };

/* ---- the link --------------------------------------------------------------------------------- */
extern void sdp_init(void);

void a2dp_init(void) {
    memset(&A, 0, sizeof A);
    l2cap_listen(25, &in_ops);
    sdp_init();
}

void a2dp_link_ready(u16 handle, const u8 addr[6], const char *name, int initiator) {
    stream_stop();
    A.state = A_SIG;
    A.handle = handle;
    memcpy(A.addr, addr, 6);
    strlcpy(A.name, name, sizeof A.name);
    A.initiator = initiator;
    A.sig = A.media = NULL;
    A.wait_sig = 0;
    A.since = bt_now_ms();
    if (initiator) {
        set_status("Connecting to %s (audio)");
        A.sig = l2cap_connect(handle, 25, &sig_ops, NULL);
        if (!A.sig) hci_disconnect(handle);
    } else set_status("%s connected");
}

void a2dp_link_down(u16 handle) {
    if (handle != A.handle) return;
    stream_stop();
    A.state = A_IDLE;
    A.sig = A.media = NULL;
}

int bt_audio_connected(const u8 addr[6]) { return A.state == A_STREAMING && !memcmp(addr, A.addr, 6); }

/* the bluetooth thread: send queued media; give up on a device that stops answering */
void a2dp_poll(void) {
    while (A.qtail != A.qhead && A.media && hci_acl_room() > 0) {
        u32 i = A.qtail % PKT_Q;
        l2cap_send(A.media, A.q[i], A.qlen[i]);
        A.qtail++;
    }
    if (A.state == A_IDLE || A.state == A_STREAMING || A.state == A_SUSPENDED) return;
    /* the headset connected to us but leaves the audio to us (most do): start it ourselves, as BlueZ does */
    if (A.state == A_SIG && !A.initiator && bt_now_ms() - A.since > 3000) {
        klog("bt: A2DP: %s did not start the audio signalling; QRT does", A.name);
        A.initiator = 1;
        A.since = bt_now_ms();
        set_status("Connecting to %s (audio)");
        if (A.sig) { A.state = A_DISCOVER; av_command(DISCOVER, NULL, 0); }
        else if (!(A.sig = l2cap_connect(A.handle, 25, &sig_ops, NULL))) hci_disconnect(A.handle);
        return;
    }
    /* the headset opened the media channel and waits: start the stream */
    if (A.state == A_MEDIA && A.media && A.initiator && !A.wait_sig && bt_now_ms() - A.since > 1000) {
        u8 sd = (u8)(A.remote_seid << 2);
        A.state = A_START;
        av_command(START, &sd, 1);
        return;
    }
    if ((A.wait_sig || A.state == A_MEDIA || A.state == A_SIG) && bt_now_ms() - A.since > 15000) {
        klog("bt: A2DP: no answer from %s (state %d, waiting for %u)", A.name, A.state, A.wait_sig);
        set_status("%s did not start the audio stream");
        A.wait_sig = 0;
        A.since = bt_now_ms();
        hci_disconnect(A.handle);
    }
}
