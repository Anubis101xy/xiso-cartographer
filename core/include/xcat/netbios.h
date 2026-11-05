#ifndef XCAT_NETBIOS_H
#define XCAT_NETBIOS_H

/*
 * NetBIOS node status (RFC 1002 §4.2.17-4.2.18), as the NAS scan uses it to name what it
 * finds (docs/formats/smb.md §5): the request for "*", and the server and workgroup names in
 * the answer. Parsing only; the platform sends and receives the datagrams (UDP port 137).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xcat/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XC_NBNS_PORT 137
#define XC_NBSTAT_QUERY_LEN 50
#define XC_NBSTAT_MAX_ANSWER 1024 /* bytes read from an answer; longer ones are cut */

/* Writes the node status request for "*" with transaction ID `id` (RFC 1002 §4.2.17). */
void xc_nbstat_query(uint16_t id, uint8_t out[XC_NBSTAT_QUERY_LEN]);

typedef struct xc_nbstat {
    /* The machine's file server name (unique, suffix 0x20), else its unique suffix-0x00 name:
       printable ASCII, trailing spaces trimmed, other bytes as '?'. "" if it has none. */
    char name[16];
    char group[16]; /* its workgroup or domain (group, suffix 0x00); "" if none */
    bool has_mac;
    uint8_t mac[6]; /* the statistics' unit ID, the network card's address */
} xc_nbstat;

/* Reads an answer to request `id`. XC_ERR_CORRUPT if data[0, len) isn't a node status answer
   to it (wrong ID, an error code, or anything out of bounds). */
xc_err xc_nbstat_parse(const uint8_t *data, size_t len, uint16_t id, xc_nbstat *out);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_NETBIOS_H */
