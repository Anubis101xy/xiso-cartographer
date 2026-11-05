#ifndef XCAT_SMB_SCAN_H
#define XCAT_SMB_SCAN_H

/*
 * Finding the NAS (docs/formats/smb.md §5): every address of the console's network is asked
 * for an SMB connection (TCP port 445), a few at a time, and each that accepts is asked its
 * NetBIOS name (UDP port 137). Nothing is sent to an address outside the console's own /24
 * (or smaller) network, and nothing but those two probes. BSD sockets: lwIP's on the console,
 * the OS's on a PC.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_SMB_SCAN_MAX 64 /* servers kept */

typedef struct xc_smb_found {
    uint8_t ip[4];
    uint16_t port;
    char name[16];  /* its NetBIOS name, or "" if it didn't say */
    char group[16]; /* its workgroup, or "" */
} xc_smb_found;

/* Called after each batch: `done` of `total` addresses asked. Return false to stop. */
typedef bool (*xc_smb_scan_progress_fn)(void *ctx, unsigned done, unsigned total);

typedef struct xc_smb_scan_opts {
    uint8_t ip[4];    /* the console's address */
    uint8_t mask[4];  /* its network mask; anything wider than /24 scans the /24 around ip */
    uint16_t port;    /* 0: 445 */
    unsigned batch;   /* addresses asked at once; 0: 16 */
    unsigned wait_ms; /* how long a batch waits for answers; 0: 600 */
    xc_smb_scan_progress_fn progress; /* may be NULL */
    void *ctx;
} xc_smb_scan_opts;

/*
 * Scans and fills out[0, max) with the servers found, in address order; *count is how many.
 * XC_ERR_INVALID_ARG for a mask that isn't one or an address that isn't usable;
 * XC_ERR_NET_IO if no socket can be opened; XC_ERR_CANCELLED if progress said stop (what was
 * found so far is kept).
 */
xc_err xc_smb_scan(const xc_smb_scan_opts *opts, xc_smb_found *out, size_t max, size_t *count);

/* Whether an IPv4 address `ip` ("192.168.1.50") accepts a TCP connection on `port` within
   wait_ms: XC_OK, else XC_ERR_NET_CONNECT (refused, or no answer at all: the NAS is off) or
   XC_ERR_NET_IO. Lets a connection to a NAS that's off fail in seconds, not at the request
   timeout. XC_ERR_INVALID_ARG if `ip` isn't an IPv4 address. */
xc_err xc_smb_probe(const char *ip, uint16_t port, unsigned wait_ms);

/* The addresses a scan with these options asks, how many and the first (smb.md §5). False if
   the mask or address isn't usable. */
bool xc_smb_scan_range(const uint8_t ip[4], const uint8_t mask[4], uint32_t *first,
                       unsigned *count);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_SMB_SCAN_H */
