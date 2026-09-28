/*
 * tcp.c - a small TCP client (RFC 793 state machine, RFC 6298-style timer).
 *
 * Enough for HTTP-style request/response over a LAN or the Internet: 64 KB
 * send and receive buffers, MSS option, in-order delivery only (a segment
 * that arrives early is dropped and the peer retransmits it), go-back-N
 * retransmission with exponential backoff.  No listening sockets.
 */
#include "net.h"
#include "wifilog.h"
#include "crypto.h"

#define NSOCK   16
#define BUFSZ   65536
#define MSS_US  1460

typedef struct {
    int state, used, user_closed, close_req, fin_sent, fin_rcvd;
    u32 rip; u16 rport, lport; u32 lip;
    u32 iss, snd_una, snd_nxt, snd_wnd, fin_seq;
    u32 irs, rcv_nxt;
    u16 mss;
    u8 *tx; usize tx_len;              /* bytes from snd_una (the SYN/FIN do not occupy the buffer) */
    u8 *rx; usize rx_head, rx_len;     /* ring */
    int rto_ms, retries;
    u64 rto_at, free_at;
    u32 last_wnd_adv;
} tcb_t;

static tcb_t tcb[NSOCK];
static u16 next_port = 50000;

static int seq_lt(u32 a, u32 b) { return (i32)(a - b) < 0; }
static int seq_le(u32 a, u32 b) { return (i32)(a - b) <= 0; }

static u16 rx_window(tcb_t *t) { usize f = BUFSZ - t->rx_len; return (u16)MIN(f, (usize)65535); }

static void seg_send(tcb_t *t, u32 seq, u8 flags, const u8 *data, usize len, int syn_opts) {
    netif_t *n = net_primary();
    if (!n) return;
    u8 s[1500];
    usize hl = syn_opts ? 24 : 20;
    if (hl + len > 1480) return;
    put16(s, t->lport); put16(s + 2, t->rport);
    put32(s + 4, seq);
    put32(s + 8, (flags & 0x10) ? t->rcv_nxt : 0);
    s[12] = (u8)((hl / 4) << 4);
    s[13] = flags;
    u16 w = rx_window(t);
    put16(s + 14, w);
    t->last_wnd_adv = w;
    put16(s + 16, 0); put16(s + 18, 0);
    if (syn_opts) { s[20] = 2; s[21] = 4; put16(s + 22, MSS_US); }
    if (len) memcpy(s + hl, data, len);
    put16(s + 16, net_checksum(s, hl + len, net_pseudo_sum(n->ip, t->rip, 6, hl + len)));
    ip_output(t->rip, 6, s, hl + len);
}

enum { F_FIN = 1, F_SYN = 2, F_RST = 4, F_PSH = 8, F_ACK = 16 };

static void arm(tcb_t *t) { t->rto_at = k_now_ms() + (u64)t->rto_ms; }

/* send whatever the window allows, then the FIN if the user closed */
static void output(tcb_t *t) {
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT && t->state != TCP_FIN_WAIT_1 && t->state != TCP_LAST_ACK) return;
    u32 wnd = t->snd_wnd ? t->snd_wnd : 1;
    for (;;) {
        u32 inflight = t->snd_nxt - t->snd_una;
        if (t->fin_sent) break;
        usize sent_bytes = inflight;
        if (sent_bytes >= t->tx_len) break;
        if (inflight >= wnd) break;
        usize n = MIN(t->tx_len - sent_bytes, (usize)(wnd - inflight));
        n = MIN(n, (usize)t->mss);
        seg_send(t, t->snd_nxt, F_ACK | F_PSH, t->tx + sent_bytes, n, 0);
        t->snd_nxt += (u32)n;
        if (!t->rto_at) arm(t);
    }
    if (t->close_req && !t->fin_sent && t->snd_nxt - t->snd_una == t->tx_len) {
        t->fin_seq = t->snd_nxt;
        seg_send(t, t->snd_nxt, F_FIN | F_ACK, NULL, 0, 0);
        t->snd_nxt++;
        t->fin_sent = 1;
        t->state = t->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
        if (!t->rto_at) arm(t);
    }
}

static void release(tcb_t *t) {
    if (t->tx) kfree(t->tx);
    if (t->rx) kfree(t->rx);
    memset(t, 0, sizeof *t);
}

int tcp_connect(u32 ip, u16 port) {
    netif_t *n = net_primary();
    if (!n) return -1;
    for (int i = 0; i < NSOCK; i++) {
        tcb_t *t = &tcb[i];
        if (t->used) continue;
        memset(t, 0, sizeof *t);
        t->used = 1;
        t->rip = ip; t->rport = port; t->lip = n->ip;
        t->lport = next_port++;
        if (next_port < 50000) next_port = 50000;
        random_bytes((u8 *)&t->iss, 4);
        t->snd_una = t->iss;
        t->snd_nxt = t->iss + 1;
        t->mss = 536;
        t->snd_wnd = 1;
        t->tx = kalloc(BUFSZ);
        t->rx = kalloc(BUFSZ);
        t->rto_ms = 1000;
        t->state = TCP_SYN_SENT;
        seg_send(t, t->iss, F_SYN, NULL, 0, 1);
        arm(t);
        return i;
    }
    return -1;
}

static tcb_t *get(int s) { return s >= 0 && s < NSOCK && tcb[s].used ? &tcb[s] : NULL; }

int tcp_state(int s) { tcb_t *t = get(s); return t ? t->state : TCP_CLOSED; }

int tcp_send(int s, const void *data, usize len) {
    tcb_t *t = get(s);
    if (!t || (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) || t->close_req) return -1;
    usize room = BUFSZ - t->tx_len, n = MIN(len, room);
    memcpy(t->tx + t->tx_len, data, n);
    t->tx_len += n;
    output(t);
    return (int)n;
}

int tcp_recv(int s, void *buf, usize len) {
    tcb_t *t = get(s);
    if (!t) return -1;
    usize n = MIN(len, t->rx_len);
    u8 *o = buf;
    for (usize i = 0; i < n; i++) o[i] = t->rx[(t->rx_head + i) % BUFSZ];
    t->rx_head = (t->rx_head + n) % BUFSZ;
    t->rx_len -= n;
    /* reopen a closed window */
    if (n && t->last_wnd_adv < 2 * t->mss && rx_window(t) >= 2 * t->mss && t->state >= TCP_ESTABLISHED && t->state != TCP_FAILED)
        seg_send(t, t->snd_nxt, F_ACK, NULL, 0, 0);
    return (int)n;
}

int tcp_readable(int s) { tcb_t *t = get(s); return t ? (int)t->rx_len : 0; }
int tcp_writable(int s) { tcb_t *t = get(s); return t && !t->close_req ? (int)(BUFSZ - t->tx_len) : 0; }
int tcp_peer_closed(int s) { tcb_t *t = get(s); return !t || (t->fin_rcvd && !t->rx_len) || t->state == TCP_FAILED || t->state == TCP_CLOSED; }

void tcp_close(int s) {
    tcb_t *t = get(s);
    if (!t) return;
    t->user_closed = 1;
    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) { t->close_req = 1; output(t); }
    else if (t->state == TCP_SYN_SENT || t->state == TCP_FAILED || t->state == TCP_CLOSED) release(t);
    if (t->used && !t->free_at) t->free_at = k_now_ms() + 30000;    /* give up on a peer that never closes */
}

void tcp_abort(int s) {
    tcb_t *t = get(s);
    if (!t) return;
    if (t->state != TCP_SYN_SENT && t->state != TCP_FAILED && t->state != TCP_CLOSED)
        seg_send(t, t->snd_nxt, F_RST | F_ACK, NULL, 0, 0);
    release(t);
}

static void send_rst(u32 src, u32 dst, const u8 *p, usize len) {
    netif_t *n = net_primary();
    if (!n) return;
    u8 flags = p[13];
    if (flags & F_RST) return;
    u8 s[20];
    memset(s, 0, 20);
    put16(s, be16(p + 2)); put16(s + 2, be16(p));
    usize hl = (usize)(p[12] >> 4) * 4;
    usize dlen = len - hl + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0);
    if (flags & F_ACK) { put32(s + 4, be32(p + 8)); s[13] = F_RST; }
    else { put32(s + 8, be32(p + 4) + (u32)dlen); s[13] = F_RST | F_ACK; }
    s[12] = 5 << 4;
    put16(s + 16, net_checksum(s, 20, net_pseudo_sum(dst, src, 6, 20)));
    ip_output(src, 6, s, 20);
}

void tcp_input(u32 src, u32 dst, const u8 *p, usize len) {
    if (len < 20 || net_checksum(p, len, net_pseudo_sum(src, dst, 6, len)) != 0) return;
    u16 sport = be16(p), dport = be16(p + 2);
    u32 seq = be32(p + 4), ack = be32(p + 8);
    usize hl = (usize)(p[12] >> 4) * 4;
    if (hl < 20 || hl > len) return;
    u8 flags = p[13];
    u32 wnd = be16(p + 14);
    const u8 *data = p + hl;
    usize dlen = len - hl;
    tcb_t *t = NULL;
    for (int i = 0; i < NSOCK; i++)
        if (tcb[i].used && tcb[i].rip == src && tcb[i].rport == sport && tcb[i].lport == dport) t = &tcb[i];
    if (!t) { send_rst(src, dst, p, len); return; }

    if (t->state == TCP_SYN_SENT) {
        if ((flags & F_ACK) && ack != t->iss + 1) { send_rst(src, dst, p, len); return; }
        if (flags & F_RST) { if (flags & F_ACK) t->state = TCP_FAILED; return; }
        if (!(flags & F_SYN) || !(flags & F_ACK)) return;
        t->irs = seq;
        t->rcv_nxt = seq + 1;
        t->snd_una = ack;
        t->snd_wnd = wnd;
        for (usize i = 20; i + 1 < hl;) {                        /* options: MSS */
            u8 k = p[i];
            if (k == 0) break;
            if (k == 1) { i++; continue; }
            u8 l = p[i + 1];
            if (l < 2) break;
            if (k == 2 && l == 4 && i + 4 <= hl) t->mss = (u16)MIN(be16(p + i + 2), MSS_US);
            i += l;
        }
        t->state = TCP_ESTABLISHED;
        t->rto_at = 0;
        t->retries = 0;
        seg_send(t, t->snd_nxt, F_ACK, NULL, 0, 0);
        output(t);
        return;
    }
    if (t->state == TCP_FAILED || t->state == TCP_CLOSED) return;
    if (flags & F_RST) { t->state = TCP_FAILED; return; }

    /* acknowledgements */
    if (flags & F_ACK) {
        if (seq_lt(t->snd_una, ack) && seq_le(ack, t->snd_nxt)) {
            u32 acked = ack - t->snd_una;
            u32 data_acked = acked;
            if (t->fin_sent && seq_le(t->fin_seq + 1, ack)) data_acked--;        /* the FIN's sequence number */
            if (data_acked > t->tx_len) data_acked = (u32)t->tx_len;
            memmove(t->tx, t->tx + data_acked, t->tx_len - data_acked);
            t->tx_len -= data_acked;
            t->snd_una = ack;
            t->retries = 0;
            t->rto_ms = 1000;
            t->rto_at = t->snd_una == t->snd_nxt ? 0 : k_now_ms() + (u64)t->rto_ms;
            if (t->fin_sent && ack == t->fin_seq + 1) {
                if (t->state == TCP_FIN_WAIT_1) t->state = TCP_FIN_WAIT_2;
                else if (t->state == TCP_CLOSING) { t->state = TCP_TIME_WAIT; t->free_at = k_now_ms() + 2000; }
                else if (t->state == TCP_LAST_ACK) { t->state = TCP_CLOSED; if (t->user_closed) release(t); return; }
            }
        }
        t->snd_wnd = wnd;
    }

    /* data, in order only */
    int need_ack = 0;
    if (dlen) {
        if (seq == t->rcv_nxt && !t->fin_rcvd) {
            usize take = MIN(dlen, (usize)(BUFSZ - t->rx_len));
            for (usize i = 0; i < take; i++) t->rx[(t->rx_head + t->rx_len + i) % BUFSZ] = data[i];
            t->rx_len += take;
            t->rcv_nxt += (u32)take;
            if (take < dlen) flags &= (u8)~F_FIN;       /* the FIN comes after bytes we could not keep */
        }
        need_ack = 1;
    }
    if ((flags & F_FIN) && seq + (u32)dlen == t->rcv_nxt && !t->fin_rcvd) {
        t->rcv_nxt++;
        t->fin_rcvd = 1;
        need_ack = 1;
        if (t->state == TCP_ESTABLISHED) t->state = TCP_CLOSE_WAIT;
        else if (t->state == TCP_FIN_WAIT_1) t->state = TCP_CLOSING;
        else if (t->state == TCP_FIN_WAIT_2) { t->state = TCP_TIME_WAIT; t->free_at = k_now_ms() + 2000; }
    }
    if (need_ack) seg_send(t, t->snd_nxt, F_ACK, NULL, 0, 0);
    output(t);
}

void tcp_poll(void) {
    u64 now = k_now_ms();
    for (int i = 0; i < NSOCK; i++) {
        tcb_t *t = &tcb[i];
        if (!t->used) continue;
        if (t->free_at && now >= t->free_at && (t->user_closed || t->state == TCP_TIME_WAIT)) {
            if (t->user_closed) { release(t); continue; }
            t->state = TCP_CLOSED;
        }
        if (!t->rto_at || now < t->rto_at) continue;
        if (++t->retries > 8) { wifilog("tcp: connection timed out"); t->state = TCP_FAILED; t->rto_at = 0; continue; }
        t->rto_ms = MIN(t->rto_ms * 2, 16000);
        if (t->state == TCP_SYN_SENT) { seg_send(t, t->iss, F_SYN, NULL, 0, 1); arm(t); continue; }
        /* go back to the first unacknowledged byte */
        t->snd_nxt = t->snd_una;
        int fin_pending = t->fin_sent;
        t->fin_sent = 0;
        t->rto_at = 0;
        if (fin_pending) {
            if (t->state == TCP_FIN_WAIT_1) t->state = TCP_ESTABLISHED;
            else if (t->state == TCP_LAST_ACK) t->state = TCP_CLOSE_WAIT;
            else if (t->state == TCP_CLOSING) { t->state = TCP_CLOSE_WAIT; }
        }
        output(t);
        if (!t->rto_at) arm(t);
    }
}
