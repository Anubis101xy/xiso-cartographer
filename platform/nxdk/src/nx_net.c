/*
 * xc_net and xc_clock (xcat/net.h) for the Xbox: nxdk's lwIP sockets, non-blocking,
 * with lwip_select() for every timeout (nxdk builds lwIP without SO_RCVTIMEO). The
 * lwip_* names are used directly so lwIP's BSD-compatibility macros (close, send, ...)
 * never touch the xc_net fields of the same names.
 */
#include <errno.h>
#include <lwip/memp.h>
#include <lwip/netdb.h>
#include <lwip/netif.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/sockets.h>
#include <lwip/stats.h>
#include <lwip/tcpip.h>
#include <nxdk/net.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "xcat/nxdk.h"

extern struct netif *g_pnetif;              /* nxdk/net.c */
extern const struct memp_desc memp_RX_POOL; /* nvnetdrv_lwip.c: one per driver buffer in use */

struct xc_net_conn {
    int s;
    uint16_t local_port; /* finds its pcb for recv_counters, 0 if unknown */
};

static xc_nx_net_stats stats;
static xc_nx_recv_counters recv_counters;
static volatile uint32_t frames_in, frames_refused; /* counting_input, from the driver's DPC */

void xc_nx_recv_counters_get(xc_nx_recv_counters *out)
{
    *out = recv_counters;
}

/* Counts the state of our receive window as a recv on `conn` begins (xc_nx_recv_counters). */
static void count_window(const xc_net_conn *conn)
{
    if (conn->local_port == 0) {
        return;
    }
    LOCK_TCPIP_CORE();
    for (const struct tcp_pcb *p = tcp_active_pcbs; p != NULL; p = p->next) {
        if (p->local_port == conn->local_port) {
            uint32_t unread = TCP_WND > p->rcv_wnd ? (uint32_t)(TCP_WND - p->rcv_wnd) : 0U;
            recv_counters.unread_bytes_sum += unread;
            if (unread > recv_counters.unread_bytes_max) {
                recv_counters.unread_bytes_max = unread;
            }
            if (p->rcv_ann_wnd == 0) {
                recv_counters.reads_window_closed++;
            }
            if (p->ooseq != NULL) {
                recv_counters.reads_out_of_order++;
            }
            break;
        }
    }
    UNLOCK_TCPIP_CORE();
}

void xc_nx_net_last(xc_nx_net_stats *out)
{
    *out = stats;
}

void xc_nx_tcp_counters_get(xc_nx_tcp_counters *out)
{
    /* Read without locking: lwIP's thread may be counting meanwhile, which is fine for a
     * report. Counters are 32-bit (LWIP_STATS_LARGE, platform/nxdk/lwip/lwipopts.h). */
    memset(out, 0, sizeof *out);
    out->segments_in = lwip_stats.mib2.tcpinsegs;
    out->segments_out = lwip_stats.mib2.tcpoutsegs;
    out->segments_bad = lwip_stats.mib2.tcpinerrs;
    out->segments_dropped = lwip_stats.tcp.drop;
    out->out_of_memory = lwip_stats.tcp.memerr + lwip_stats.link.memerr;
    out->window_bytes = TCP_WND;
    out->frames_in = frames_in;
    out->frames_refused = frames_refused;
    out->rx_buffers = RX_BUFF_CNT;
    out->rx_buffers_max_used = (uint32_t)memp_RX_POOL.stats->max;
}

/* --- frames from the driver ----------------------------------------------------------- */

/*
 * nxdk's driver hands each received frame to g_pnetif->input (tcpip_input) from its DPC.
 * lwIP queues it in its thread's mailbox (SYS_MBOX_SIZE slots); when that is full, the frame
 * is dropped and nothing counts it. This wrapper counts frames and refusals.
 */
static netif_input_fn lwip_input;

static err_t counting_input(struct pbuf *p, struct netif *inp)
{
    frames_in++;
    err_t err = lwip_input(p, inp);
    if (err != ERR_OK) {
        frames_refused++;
    }
    return err;
}

xc_nx_link xc_nx_link_get(void)
{
    DWORD st = PhyGetLinkState(FALSE);
    xc_nx_link link;
    link.up = (st & XNET_ETHERNET_LINK_ACTIVE) != 0;
    link.mbps = (st & XNET_ETHERNET_LINK_100MBPS) != 0  ? 100U
                : (st & XNET_ETHERNET_LINK_10MBPS) != 0 ? 10U
                                                        : 0U;
    link.full_duplex = (st & XNET_ETHERNET_LINK_FULL_DUPLEX) != 0;
    return link;
}

size_t xc_nx_tcp_describe(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    size_t len = 0;
    LOCK_TCPIP_CORE(); /* the pcb list belongs to lwIP's thread */
    for (const struct tcp_pcb *p = tcp_active_pcbs; p != NULL && len + 1 < cap; p = p->next) {
        if (p->state != ESTABLISHED) {
            continue;
        }
        bool scaled = (p->flags & TF_WND_SCALE) != 0;
        int n = snprintf(buf + len, cap - len,
                         "%sport %u: scaling %s (ours %u, theirs %u), SACK %s, our window %lu "
                         "(advertised %lu), theirs %lu, cwnd %lu, mss %u, out-of-order %s",
                         len > 0 ? "; " : "", (unsigned)p->remote_port, scaled ? "on" : "OFF",
                         scaled ? (unsigned)p->rcv_scale : 0U, scaled ? (unsigned)p->snd_scale : 0U,
                         (p->flags & TF_SACK) != 0 ? "on" : "OFF", (unsigned long)p->rcv_wnd,
                         (unsigned long)p->rcv_ann_wnd, (unsigned long)p->snd_wnd,
                         (unsigned long)p->cwnd, (unsigned)p->mss,
                         p->ooseq != NULL ? "queued" : "none");
        if (n < 0) {
            break;
        }
        len += (size_t)n < cap - len ? (size_t)n : cap - len - 1;
    }
    UNLOCK_TCPIP_CORE();
    return len;
}

/* --- clock ---------------------------------------------------------------------------- */

static uint64_t clock_now_ms(void *ctx)
{
    (void)ctx;
    static ULONGLONG freq;
    if (freq == 0) {
        freq = KeQueryPerformanceFrequency();
    }
    /* Split so counter * 1000 can't overflow (it would after ~7 h at 733 MHz). */
    ULONGLONG c = KeQueryPerformanceCounter();
    return (uint64_t)(c / freq * 1000U + c % freq * 1000U / freq);
}

static void clock_sleep_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    Sleep(ms);
}

void xc_nx_clock(xc_clock *clock)
{
    clock->now_ms = clock_now_ms;
    clock->sleep_ms = clock_sleep_ms;
    clock->ctx = NULL;
}

/* --- startup -------------------------------------------------------------------------- */

xc_err xc_nx_net_startup(char *ip, size_t ip_size)
{
    if (nxNetInit(NULL) != 0) {
        return XC_ERR_NET_CONNECT;
    }
    if (lwip_input == NULL) {
        lwip_input = g_pnetif->input;
        g_pnetif->input = counting_input; /* one pointer store: the DPC sees old or new */
    }
    if (ip != NULL && ip_size > 0) {
        (void)snprintf(ip, ip_size, "%s", ip4addr_ntoa(netif_ip4_addr(g_pnetif)));
    }
    return XC_OK;
}

bool xc_nx_net_ipv4(uint8_t ip[4], uint8_t mask[4])
{
    if (g_pnetif == NULL) {
        return false;
    }
    uint32_t a = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(g_pnetif)));
    uint32_t m = lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(g_pnetif)));
    for (int i = 0; i < 4; i++) {
        ip[i] = (uint8_t)(a >> (24 - 8 * i));
        mask[i] = (uint8_t)(m >> (24 - 8 * i));
    }
    return a != 0;
}

/* --- sockets -------------------------------------------------------------------------- */

typedef enum wait_for { WAIT_READ, WAIT_WRITE } wait_for;

/* Waits until `s` is readable or writable. XC_OK, XC_ERR_NET_TIMEOUT or XC_ERR_NET_IO. */
static xc_err wait_sock(int s, wait_for what, uint32_t timeout_ms)
{
    if (s < 0 || s >= FD_SETSIZE) {
        return XC_ERR_NET_IO;
    }
    uint64_t deadline = clock_now_ms(NULL) + timeout_ms;
    for (;;) {
        uint64_t now = clock_now_ms(NULL);
        uint64_t left = deadline > now ? deadline - now : 0;
        fd_set fds;
        fd_set errs;
        FD_ZERO(&fds);
        FD_ZERO(&errs);
        FD_SET(s, &fds);
        FD_SET(s, &errs); /* lwIP reports a failed connect here */
        struct timeval tv;
        tv.tv_sec = (long)(left / 1000U);
        tv.tv_usec = (long)(left % 1000U * 1000U);
        int n = lwip_select(s + 1, what == WAIT_READ ? &fds : NULL,
                            what == WAIT_WRITE ? &fds : NULL, &errs, &tv);
        if (n > 0) {
            return FD_ISSET(s, &fds) ? XC_OK : XC_ERR_NET_IO;
        }
        if (n == 0) {
            return XC_ERR_NET_TIMEOUT;
        }
        if (errno != EINTR) {
            return XC_ERR_NET_IO;
        }
    }
}

static bool would_block(void)
{
    return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN;
}

static xc_err net_connect(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms,
                          xc_net_conn **out)
{
    (void)ctx;
    char port_str[8];
    (void)snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; /* URLs are IPv4 or names (http.md §2.1) */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *ai = NULL;
    uint64_t t0 = clock_now_ms(NULL);
    stats.connects++;
    stats.connect_ms = 0;
    if (lwip_getaddrinfo(host, port_str, &hints, &ai) != 0 || ai == NULL) {
        stats.dns_ms = (uint32_t)(clock_now_ms(NULL) - t0);
        stats.result = XC_ERR_NET_DNS;
        return XC_ERR_NET_DNS;
    }
    uint64_t t1 = clock_now_ms(NULL);
    stats.dns_ms = (uint32_t)(t1 - t0);
    xc_net_conn *conn = malloc(sizeof *conn);
    int s = lwip_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    xc_err err = conn == NULL ? XC_ERR_NOMEM : XC_OK;
    if (err == XC_OK && (s < 0 || lwip_fcntl(s, F_SETFL, O_NONBLOCK) != 0)) {
        err = XC_ERR_NET_CONNECT;
    }
    if (err == XC_OK && lwip_connect(s, ai->ai_addr, ai->ai_addrlen) != 0) {
        if (!would_block()) {
            err = XC_ERR_NET_CONNECT;
        } else {
            err = wait_sock(s, WAIT_WRITE, timeout_ms);
            if (err == XC_ERR_NET_IO) {
                err = XC_ERR_NET_CONNECT;
            }
            int so_err = 0;
            socklen_t len = (socklen_t)sizeof so_err;
            if (err == XC_OK &&
                (lwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &len) != 0 || so_err != 0)) {
                err = XC_ERR_NET_CONNECT;
            }
        }
    }
    lwip_freeaddrinfo(ai);
    stats.connect_ms = (uint32_t)(clock_now_ms(NULL) - t1);
    stats.result = err;
    if (err != XC_OK) {
        if (s >= 0) {
            (void)lwip_close(s);
        }
        free(conn);
        return err;
    }
    conn->s = s;
    conn->local_port = 0;
    struct sockaddr_in local;
    socklen_t local_len = (socklen_t)sizeof local;
    if (lwip_getsockname(s, (struct sockaddr *)&local, &local_len) == 0) {
        conn->local_port = lwip_ntohs(local.sin_port);
    }
    *out = conn;
    return XC_OK;
}

static xc_err net_send(void *ctx, xc_net_conn *conn, const void *buf, size_t len,
                       uint32_t timeout_ms)
{
    (void)ctx;
    const char *p = buf;
    while (len > 0) {
        ssize_t n = lwip_send(conn->s, p, len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n == 0 || !would_block()) {
            return XC_ERR_NET_IO;
        }
        xc_err err = wait_sock(conn->s, WAIT_WRITE, timeout_ms);
        if (err != XC_OK) {
            return err;
        }
    }
    return XC_OK;
}

static xc_err net_recv(void *ctx, xc_net_conn *conn, void *buf, size_t cap, uint32_t timeout_ms,
                       size_t *got)
{
    (void)ctx;
    *got = 0;
    recv_counters.reads++;
    count_window(conn);
    for (;;) {
        uint64_t t0 = clock_now_ms(NULL);
        xc_err err = wait_sock(conn->s, WAIT_READ, timeout_ms);
        recv_counters.wait_ms += clock_now_ms(NULL) - t0;
        if (err != XC_OK) {
            return err;
        }
        ssize_t n = lwip_recv(conn->s, buf, cap, 0);
        if (n >= 0) {
            *got = (size_t)n; /* 0: the peer closed the connection */
            return XC_OK;
        }
        if (errno != EINTR && !would_block()) {
            return XC_ERR_NET_IO;
        }
    }
}

static void net_close(void *ctx, xc_net_conn *conn)
{
    (void)ctx;
    if (conn != NULL) {
        (void)lwip_close(conn->s);
        free(conn);
    }
}

/* --- UDP -------------------------------------------------------------------------------- */

/* One datagram out and one back (xc_udp): the socket is connected to the server, so lwIP only
 * delivers that address's datagrams. */
static xc_err udp_exchange(void *ctx, const char *host, uint16_t port, const void *req,
                           size_t req_len, void *resp, size_t cap, uint32_t timeout_ms, size_t *got,
                           uint32_t *rtt_ms)
{
    (void)ctx;
    *got = 0;
    *rtt_ms = 0;
    char port_str[8];
    (void)snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    struct addrinfo *ai = NULL;
    if (lwip_getaddrinfo(host, port_str, &hints, &ai) != 0 || ai == NULL) {
        return XC_ERR_NET_DNS;
    }
    int s = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bool ok = s >= 0 && lwip_fcntl(s, F_SETFL, O_NONBLOCK) == 0 &&
              lwip_connect(s, ai->ai_addr, ai->ai_addrlen) == 0 &&
              lwip_send(s, req, req_len, 0) == (ssize_t)req_len;
    lwip_freeaddrinfo(ai);
    if (!ok) {
        if (s >= 0) {
            (void)lwip_close(s);
        }
        return XC_ERR_NET_IO;
    }
    uint64_t sent = clock_now_ms(NULL);
    xc_err err = wait_sock(s, WAIT_READ, timeout_ms);
    while (err == XC_OK) {
        ssize_t n = lwip_recv(s, resp, cap, 0);
        if (n >= 0) {
            *got = (size_t)n; /* lwIP drops what doesn't fit */
            *rtt_ms = (uint32_t)(clock_now_ms(NULL) - sent);
            break;
        }
        if (errno != EINTR && !would_block()) {
            err = XC_ERR_NET_IO;
            break;
        }
        uint64_t now = clock_now_ms(NULL);
        err = now - sent >= timeout_ms
                  ? XC_ERR_NET_TIMEOUT
                  : wait_sock(s, WAIT_READ, (uint32_t)(timeout_ms - (now - sent)));
    }
    (void)lwip_close(s);
    return err;
}

void xc_nx_udp(xc_udp *udp)
{
    udp->exchange = udp_exchange;
    udp->ctx = NULL;
}

void xc_nx_net(xc_net *net)
{
    net->connect = net_connect;
    net->send = net_send;
    net->recv = net_recv;
    net->close = net_close;
    net->ctx = NULL;
}
