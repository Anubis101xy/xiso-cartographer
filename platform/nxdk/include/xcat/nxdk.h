#ifndef XCAT_NXDK_H
#define XCAT_NXDK_H

/*
 * Xbox (nxdk) platform adapters: a file-backed xc_source, a FATX directory
 * xc_sink, drive helpers, and lwIP networking with a clock (docs/ARCHITECTURE.md §2, §4). Built
 * only into XBEs (app/Makefile, app/net-test/Makefile). Paths are DOS-style ("E:\Games\Title");
 * FATX limits paths to 250 bytes after the drive (docs/formats/fatx-names.md §3), so 256-byte
 * buffers always suffice once xc_fatx_check_listing() has passed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/install.h"
#include "xcat/net.h"
#include "xcat/sink.h"
#include "xcat/source.h"
#include "xcat/tls.h"

#define XC_NX_PATH_MAX 256 /* bytes, including the terminator */

typedef struct xc_nx_file {
    void *handle; /* HANDLE */
} xc_nx_file;

/* Opens `path` read-only as a source (64-bit offsets). `f` must outlive `src`. */
xc_err xc_nx_file_open(xc_source *src, xc_nx_file *f, const char *path);
void xc_nx_file_close(xc_nx_file *f);

typedef struct xc_nx_dir_sink {
    char root[XC_NX_PATH_MAX];
    char open_path[XC_NX_PATH_MAX];
    void *handle; /* HANDLE of the open file, or NULL */
    uint32_t expected;
    uint64_t written;
} xc_nx_dir_sink;

/* Writes into `root`, creating it and missing parents. Partial files are deleted. */
xc_err xc_nx_dir_sink_open(xc_sink *sink, xc_nx_dir_sink *ds, const char *root);
void xc_nx_dir_sink_close(xc_nx_dir_sink *ds);

/*
 * The last file operation of the directory sink that failed, for messages: which call
 * ("create", "write", "flush", "close") and its error (GetLastError() or NTSTATUS).
 * *op is NULL if nothing has failed.
 */
void xc_nx_last_io_error(const char **op, unsigned long *code);

/* Mounts E: (HDD partition 1) if needed. */
bool xc_nx_mount_e(void);

/* Mounts F: and G: (HDD partitions 6 and 7, on drives set up with them) if needed, and says
   whether each is there: its root can be read (docs/formats/settings.md §4.1). */
void xc_nx_mount_fg(bool *has_f, bool *has_g);

/* Free bytes on the drive containing `path` (e.g. "E:\"). */
xc_err xc_nx_free_space(const char *path, uint64_t *free_bytes);

/* Free bytes and the size of the drive containing `path`. */
xc_err xc_nx_disk_space(const char *path, uint64_t *free_bytes, uint64_t *total_bytes);

/* The cluster size of the drive `root` ("F:\"), or 0 if it can't be read. */
uint32_t xc_nx_cluster_size(const char *root);

/* True if `path` names an existing file or directory. */
bool xc_nx_exists(const char *path);

/* Creates `path` and any missing parents. */
bool xc_nx_mkdirs(const char *path);

/* Deletes the folder `path` and everything in it, read-only files too: a game's install
   folder the user asked to delete. Folders more than 40 levels deep, or a path over 255
   bytes, give XC_ERR_LIMIT; a file that can't be deleted, XC_ERR_IO (what could be deleted
   is gone). The caller checks that `path` is one of its install folders. */
xc_err xc_nx_remove_tree(const char *path);

/* Replaces the file `path` with data[0, len) all at once (docs/formats/login.md §5): writes
   `path`.tmp, flushes it, then renames it over `path`. A power cut leaves the old file or the
   new one, and at worst a stale .tmp. The .tmp is wiped and deleted if anything fails. */
xc_err xc_nx_replace_file(const char *path, const void *data, size_t len);

/*
 * Networking (xcat/net.h) over nxdk's lwIP (nx_net.c). xc_nx_net_startup()
 * brings the network up with the dashboard's settings (DHCP or static) and
 * writes the console's IPv4 address as text into ip[0, ip_size). It fails with
 * XC_ERR_NET_CONNECT if the settings can't be read or DHCP gets no address
 * (cable unplugged?). Call it once, before using `net`.
 */
xc_err xc_nx_net_startup(char *ip, size_t ip_size);
void xc_nx_net(xc_net *net);
/* The console's IPv4 address and network mask, after xc_nx_net_startup(); false if it has none
   (for the NAS scan, docs/formats/smb.md §5). */
bool xc_nx_net_ipv4(uint8_t ip[4], uint8_t mask[4]);
/* One UDP request and answer (xc_udp, for SNTP), after xc_nx_net_startup(). */
void xc_nx_udp(xc_udp *udp);

/* Monotonic milliseconds from the CPU's performance counter, and sleep. */
void xc_nx_clock(xc_clock *clock);

/* How the last connect went, for diagnosing slow first requests (M3 hardware notes). */
typedef struct xc_nx_net_stats {
    unsigned connects;   /* connect attempts so far */
    uint32_t dns_ms;     /* name lookup of the last one */
    uint32_t connect_ms; /* TCP handshake of the last one */
    xc_err result;       /* XC_OK, or why it failed */
} xc_nx_net_stats;
void xc_nx_net_last(xc_nx_net_stats *out);

/*
 * lwIP's counters since startup, for diagnosing slow downloads (M5): TCP segments in and out
 * (lwIP doesn't count plain ACKs as out), bad and dropped segments, buffer shortages; and the
 * configured receive window (platform/nxdk/lwip/lwipopts.h). Also the frames the driver
 * handed to lwIP, and how many lwIP refused (its input mailbox was full), which nxdk drops
 * without counting: xc_nx_net_startup() wraps the netif's input function to count them.
 * And the driver's receive buffers: how many there are, and the most lwIP held at once (each
 * frame keeps its buffer until the app has read it; with none free, the chip drops frames).
 */
typedef struct xc_nx_tcp_counters {
    uint32_t segments_in, segments_out, segments_bad, segments_dropped;
    uint32_t out_of_memory;
    uint32_t window_bytes;
    uint32_t frames_in, frames_refused;
    uint32_t rx_buffers, rx_buffers_max_used;
} xc_nx_tcp_counters;
void xc_nx_tcp_counters_get(xc_nx_tcp_counters *out);

/* The Ethernet link as the kernel last saw it: up, 10 or 100 Mbit/s (0 if unknown), duplex. */
typedef struct xc_nx_link {
    bool up;
    unsigned mbps;
    bool full_duplex;
} xc_nx_link;
xc_nx_link xc_nx_link_get(void);

/*
 * What lwIP agreed for each established TCP connection, as one line of text in buf[0, cap):
 * window scaling and both scale factors, SACK, our receive window and what we last
 * advertised, the peer's window, the congestion window, the MSS, and whether out-of-order
 * segments are queued. Returns the length. Shows whether the larger window is in effect.
 */
size_t xc_nx_tcp_describe(char *buf, size_t cap);

/*
 * Where a download's time goes (M5), summed over every recv on xc_nx_net connections: how long
 * they waited for data, and the state of our receive window as each one began: the bytes
 * received but not yet read (summed for an average, and the most), and how many found it
 * closed (zero advertised), and how many found out-of-order data queued (a segment before it
 * was lost or late). Waiting most of the time means the data arrives too slowly (sender or
 * path); a full window whenever the app reads means the console is too slow.
 */
typedef struct xc_nx_recv_counters {
    uint64_t wait_ms;
    uint32_t reads;
    uint32_t reads_window_closed;
    uint32_t reads_out_of_order;
    uint64_t unread_bytes_sum;
    uint32_t unread_bytes_max;
} xc_nx_recv_counters;
void xc_nx_recv_counters_get(xc_nx_recv_counters *out);

/*
 * For TLS (xcat/tls.h, ADR 0012, nx_tls.c). xc_nx_wallclock() is the console's clock, but
 * refused (XC_ERR_TLS_NO_TIME) when it's earlier than this XBE's build time: it was reset.
 * NTP comes in front of it (xc_nx_https_start, docs/formats/sntp.md). xc_nx_console_time()
 * is the clock's raw value and xc_nx_build_time() the floor, both in Unix seconds, for
 * messages; xc_nx_utc_date() writes one as "YYYY-MM-DD hh:mm:ss UTC".
 */
void xc_nx_wallclock(xc_wallclock *w);
int64_t xc_nx_console_time(void);
int64_t xc_nx_build_time(void);
void xc_nx_utc_date(char *buf, size_t size, int64_t unix_s);

/*
 * The time server to ask: the first line of E:\Apps\xcat\ntp.txt that isn't blank or a
 * '#' comment (a host name or IPv4 address, copied into buf), NULL for "off", or
 * pool.ntp.org when the file doesn't exist. Until the settings screen (M8).
 */
const char *xc_nx_ntp_setting(char *buf, size_t cap);

/*
 * Sets up HTTPS once for an XBE (ADR 0012): gathers randomness, gets the time (NTP from
 * `ntp_server`, NULL for off, else the console's clock if it isn't earlier than the build),
 * loads the trust anchors (E:\Apps\xcat\cacert.pem if present, else the built-in bundle), and
 * creates the TLS client over `tcp`. Each step is reported through `say`, one line at a time
 * (may be NULL). On success *tls_net is the HTTP client's `tls` net. Errors:
 * XC_ERR_TLS_NO_RANDOM, XC_ERR_TLS_NO_TIME, a CA file's parse error, XC_ERR_NOMEM.
 */
typedef struct xc_nx_https_opts {
    const xc_net *tcp;
    const xc_clock *clock;
    const char *ntp_server;
    void (*say)(const char *line);
} xc_nx_https_opts;
xc_err xc_nx_https_start(const xc_nx_https_opts *opts, xc_net *tls_net, xc_tls **tls);

/*
 * Fills `pool` for the TLS random generator (§6): the seed file E:\Apps\xcat\random-seed,
 * the clock, the performance counter and the MAC address (credited nothing), then 256
 * samples of timer-interrupt jitter, about a quarter of a second (one bit each, if it
 * changed). Writes a new seed file from the pool. Call after xc_nx_net_startup(), so the
 * MAC address is known. XC_ERR_TLS_NO_RANDOM if too little was credited.
 */
typedef struct xc_nx_entropy_stats {
    unsigned samples;
    unsigned bits;               /* credited in total */
    unsigned distinct_low_bytes; /* of the cycle counts' low bytes, 1-256: a health check */
    uint64_t min_cycles, max_cycles;
    bool seed_read, seed_written;
} xc_nx_entropy_stats;
xc_err xc_nx_entropy_gather(xc_entropy_pool *pool, xc_nx_entropy_stats *st);

/* File access for xc_install (xcat/install.h) on FATX, with flushed, durable writes, and the
   spool files of Deflate zip installs. */
void xc_nx_install_io(xc_install_io *io);
/* Closes the spool file the io keeps open (after an install that failed, say). */
void xc_nx_spool_close(void);

#endif /* XCAT_NXDK_H */
