/* Finding the NAS on the console's network (xcat/smb_scan.h, docs/formats/smb.md §5). */
#ifndef NXDK
#define _POSIX_C_SOURCE 200809L
#endif

#include "xcat/smb_scan.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/netbios.h"

#ifdef NXDK
#include <lwip/sockets.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

static uint32_t now_ms(void)
{
#ifdef NXDK
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U);
#endif
}

static uint32_t be32(const uint8_t a[4])
{
    return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
}

static void put_be32(uint32_t v, uint8_t a[4])
{
    a[0] = (uint8_t)(v >> 24);
    a[1] = (uint8_t)(v >> 16);
    a[2] = (uint8_t)(v >> 8);
    a[3] = (uint8_t)v;
}

bool xc_smb_scan_range(const uint8_t ip[4], const uint8_t mask[4], uint32_t *first, unsigned *count)
{
    uint32_t m = be32(mask);
    uint32_t host_bits = ~m;
    /* A mask is ones, then zeros; /31 and /32 have no other hosts to ask. */
    if (m == 0 || (host_bits & (host_bits + 1U)) != 0 || host_bits < 3U) {
        return false;
    }
    if (host_bits > 0xFFU) {
        m = 0xFFFFFF00U; /* wider than /24: only the /24 around the console */
        host_bits = 0xFFU;
    }
    uint32_t a = be32(ip);
    uint32_t net = a & m;
    if (ip[0] == 0 || ip[0] >= 224U || a == net || a == (net | host_bits)) {
        return false; /* not a host's address, or the network's own or broadcast one */
    }
    *first = net + 1U;
    *count = host_bits - 1U;
    return true;
}

static void close_socket(int s)
{
#ifdef NXDK
    (void)lwip_close(s);
#else
    (void)close(s);
#endif
}

static bool set_nonblocking(int s)
{
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        flags = 0;
    }
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void addr_of(uint32_t ip, uint16_t port, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    sa->sin_addr.s_addr = htonl(ip);
}

static void keep(uint32_t ip, uint16_t port, xc_smb_found *out, size_t max, size_t *n)
{
    for (size_t i = 0; i < *n; i++) {
        if (be32(out[i].ip) == ip) {
            return;
        }
    }
    if (*n < max) {
        memset(&out[*n], 0, sizeof out[*n]);
        put_be32(ip, out[*n].ip);
        out[*n].port = port;
        (*n)++;
    }
}

/* One batch: connects to addrs[0, n) at once, waits up to wait_ms for each to be accepted. */
static xc_err ask_batch(const uint32_t *addrs, unsigned n, uint16_t port, unsigned wait_ms,
                        xc_smb_found *out, size_t max, size_t *count)
{
    struct pollfd fds[64];
    uint32_t ips[64];
    unsigned open = 0;
    for (unsigned i = 0; i < n && i < 64; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            if (open == 0 && i == 0) {
                return XC_ERR_NET_IO;
            }
            break; /* out of sockets: this batch asks fewer */
        }
        if (!set_nonblocking(s)) {
            close_socket(s);
            continue;
        }
        struct sockaddr_in sa;
        addr_of(addrs[i], port, &sa);
        int rc = connect(s, (struct sockaddr *)&sa, sizeof sa);
        if (rc == 0) {
            keep(addrs[i], port, out, max, count);
            close_socket(s);
            continue;
        }
        if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN) {
            close_socket(s); /* refused at once: nothing listens there */
            continue;
        }
        fds[open].fd = s;
        fds[open].events = POLLOUT;
        fds[open].revents = 0;
        ips[open] = addrs[i];
        open++;
    }
    uint32_t start = now_ms();
    unsigned pending = open;
    while (pending > 0) {
        uint32_t spent = now_ms() - start;
        if (spent >= wait_ms) {
            break;
        }
        int rc = poll(fds, open, (int)(wait_ms - spent));
        if (rc <= 0) {
            if (rc < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        for (unsigned i = 0; i < open; i++) {
            if (fds[i].fd < 0 || fds[i].revents == 0) {
                continue;
            }
            int soerr = 0;
            socklen_t len = sizeof soerr;
            if ((fds[i].revents & POLLOUT) != 0 &&
                getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0) {
                keep(ips[i], port, out, max, count);
            }
            close_socket(fds[i].fd);
            fds[i].fd = -1; /* poll skips negative descriptors */
            pending--;
        }
    }
    for (unsigned i = 0; i < open; i++) {
        if (fds[i].fd >= 0) {
            close_socket(fds[i].fd);
        }
    }
    return XC_OK;
}

/* Asks each server found for its NetBIOS name (UDP 137), waiting up to wait_ms in all. */
static void ask_names(xc_smb_found *found, size_t n, unsigned wait_ms)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return;
    }
    if (!set_nonblocking(s)) {
        close_socket(s);
        return;
    }
    uint16_t base = (uint16_t)(0x5800U ^ (now_ms() & 0x07FFU));
    for (size_t i = 0; i < n; i++) {
        uint8_t q[XC_NBSTAT_QUERY_LEN];
        xc_nbstat_query((uint16_t)(base + i), q);
        struct sockaddr_in sa;
        addr_of(be32(found[i].ip), XC_NBNS_PORT, &sa);
        (void)sendto(s, (const char *)q, sizeof q, 0, (struct sockaddr *)&sa, sizeof sa);
    }
    size_t answered = 0;
    uint32_t start = now_ms();
    static uint8_t buf[XC_NBSTAT_MAX_ANSWER];
    while (answered < n) {
        uint32_t spent = now_ms() - start;
        if (spent >= wait_ms) {
            break;
        }
        struct pollfd p = {s, POLLIN, 0};
        if (poll(&p, 1, (int)(wait_ms - spent)) <= 0) {
            break;
        }
        struct sockaddr_in from;
        socklen_t from_len = sizeof from;
        int got = (int)recvfrom(s, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &from_len);
        if (got <= 0) {
            continue;
        }
        uint32_t ip = ntohl(from.sin_addr.s_addr);
        for (size_t i = 0; i < n; i++) {
            if (be32(found[i].ip) != ip || found[i].name[0] != '\0') {
                continue;
            }
            xc_nbstat st;
            if (xc_nbstat_parse(buf, (size_t)got, (uint16_t)(base + i), &st) == XC_OK) {
                memcpy(found[i].name, st.name, sizeof found[i].name);
                memcpy(found[i].group, st.group, sizeof found[i].group);
                answered++;
            }
            break;
        }
    }
    close_socket(s);
}

xc_err xc_smb_probe(const char *ip, uint16_t port, unsigned wait_ms)
{
    unsigned v[4];
    int n = 0;
    uint8_t a[4];
    if (ip == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    for (int i = 0; i < 4; i++) {
        v[i] = 0;
        int digits = 0;
        while (ip[n] >= '0' && ip[n] <= '9' && digits < 4) {
            v[i] = v[i] * 10U + (unsigned)(ip[n] - '0');
            n++;
            digits++;
        }
        if (digits == 0 || v[i] > 255U || (i < 3 && ip[n++] != '.')) {
            return XC_ERR_INVALID_ARG;
        }
        a[i] = (uint8_t)v[i];
    }
    if (ip[n] != '\0') {
        return XC_ERR_INVALID_ARG;
    }
    xc_smb_found found[1];
    size_t count = 0;
    uint32_t addr = be32(a);
    xc_err err = ask_batch(&addr, 1, port, wait_ms, found, 1, &count);
    if (err != XC_OK) {
        return err;
    }
    return count == 1 ? XC_OK : XC_ERR_NET_CONNECT;
}

static int by_address(const void *a, const void *b)
{
    uint32_t x = be32(((const xc_smb_found *)a)->ip);
    uint32_t y = be32(((const xc_smb_found *)b)->ip);
    return x < y ? -1 : x > y;
}

xc_err xc_smb_scan(const xc_smb_scan_opts *opts, xc_smb_found *out, size_t max, size_t *count)
{
    if (opts == NULL || out == NULL || count == NULL) {
        return XC_ERR_INVALID_ARG;
    }
    *count = 0;
    uint32_t first = 0;
    unsigned total = 0;
    if (!xc_smb_scan_range(opts->ip, opts->mask, &first, &total)) {
        return XC_ERR_INVALID_ARG;
    }
    uint16_t port = opts->port != 0 ? opts->port : 445;
    unsigned batch = opts->batch != 0 ? opts->batch : 16;
    if (batch > 64) {
        batch = 64;
    }
    unsigned wait_ms = opts->wait_ms != 0 ? opts->wait_ms : 600;
    uint32_t self = be32(opts->ip);
    uint32_t addrs[64];
    xc_err err = XC_OK;
    for (unsigned done = 0; done < total && err == XC_OK;) {
        unsigned n = 0;
        while (n < batch && done < total) {
            uint32_t a = first + done++;
            if (a != self) {
                addrs[n++] = a;
            }
        }
        err = ask_batch(addrs, n, port, wait_ms, out, max, count);
        if (err == XC_OK && opts->progress != NULL && !opts->progress(opts->ctx, done, total)) {
            err = XC_ERR_CANCELLED;
        }
    }
    if (*count > 0) {
        ask_names(out, *count, 800);
        qsort(out, *count, sizeof *out, by_address);
    }
    return err;
}
