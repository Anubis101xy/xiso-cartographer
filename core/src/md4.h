#ifndef XCAT_MD4_H
#define XCAT_MD4_H

/* MD4 (RFC 1320), only for the NT hash of an SMB password (xcat/smb.h, smb.md §4). Internal. */

#include <stddef.h>
#include <stdint.h>

#define XC_MD4_BYTES 16

/* The MD4 digest of data[0, len). */
void xc_md4(const void *data, size_t len, uint8_t out[XC_MD4_BYTES]);

#endif /* XCAT_MD4_H */
