/* net.h - QRT's IPv4 stack: ARP, IPv4, ICMP, UDP, DHCP client, DNS, SNTP, TCP.
 *
 * Link drivers (Wi-Fi through wlan.c, the e1000 in QEMU) register a netif
 * and hand received Ethernet frames to net_input(); everything runs from
 * net_poll(), called by the shell loop, so there is no locking. */
#pragma once
#include "../kernel/kernel.h"

typedef struct netif {
    const char *name;                 /* "Wi-Fi", "Ethernet" */
    u8 mac[6];
    int link;                         /* 1 = carrier / associated */
    int (*send)(struct netif *n, const u8 *eth, usize len);
    u32 ip, mask, gw, dns;            /* host byte order; 0 = not configured */
    u64 lease_until_ms;
    char detail[64];                  /* e.g. the SSID */
} netif_t;

void net_register(netif_t *n);
void net_link_changed(netif_t *n);   /* call after changing n->link: starts or stops DHCP */
void net_input(netif_t *n, const u8 *eth, usize len);
void net_poll(void);
void net_time_http_poll(void);       /* the clock's HTTP fallback; call without the network lock */
netif_t *net_primary(void);          /* the configured interface, if any */
const char *net_status(void);        /* one line for the UI */

/* UDP */
typedef void (*udp_fn)(void *ctx, u32 src_ip, u16 src_port, const u8 *data, usize len);
int  udp_bind(u16 port, udp_fn fn, void *ctx);      /* 0 = ok */
void udp_unbind(u16 port);
u16  udp_ephemeral(void);
int  udp_send(u32 dst, u16 sport, u16 dport, const void *data, usize len);

/* ICMP echo */
int  net_ping(u32 dst, u16 id, u16 seq, const void *data, usize len);
typedef void (*ping_fn)(u32 src, u16 id, u16 seq, const u8 *data, usize len);
void net_on_echo_reply(ping_fn fn);

/* DNS: start a lookup, then poll; 1 = done (*ip set, or 0 if not found), 0 = pending, -1 = error */
int  dns_start(const char *name);
int  dns_result(int q, u32 *ip);

/* TCP (client side) */
int  tcp_connect(u32 ip, u16 port);                 /* socket id >= 0 */
int  tcp_state(int s);                              /* see TCP_* below */
int  tcp_send(int s, const void *data, usize len);  /* bytes queued (may be less) */
int  tcp_recv(int s, void *buf, usize len);         /* bytes read, 0 = nothing yet */
int  tcp_readable(int s);                           /* bytes waiting */
int  tcp_writable(int s);                           /* room in the send buffer */
void tcp_close(int s);                              /* orderly close; frees when done */
void tcp_abort(int s);
enum { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT,
       TCP_LAST_ACK, TCP_TIME_WAIT, TCP_CLOSING, TCP_FAILED };
int  tcp_peer_closed(int s);                        /* FIN received and all data read */

/* helpers (shared with tcp.c) */
u16  be16(const u8 *p);
u32  be32(const u8 *p);
void put16(u8 *p, u16 v);
void put32(u8 *p, u32 v);
int  ip_output(u32 dst, u8 proto, const u8 *payload, usize len);
u32  net_pseudo_sum(u32 src, u32 dst, u8 proto, usize len);
void ip_to_str(u32 ip, char *buf, usize cap);
int  str_to_ip(const char *s, u32 *ip);
u16  net_checksum(const void *data, usize len, u32 sum);
