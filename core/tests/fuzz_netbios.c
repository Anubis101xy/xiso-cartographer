/*
 * libFuzzer target for NetBIOS node status answers (xcat/netbios.h, RFC 1002 §4.2.18).
 *
 * The input is a datagram, read as the answer to transaction 0x4242. Parsing must give XC_OK
 * or XC_ERR_CORRUPT, and on success both names must be NUL-terminated printable ASCII. A
 * violation aborts. Seeds are in core/tests/fuzz-seeds/netbios/.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/netbios.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

static void check_name(const char *s)
{
    const char *nul = memchr(s, '\0', 16);
    check(nul != NULL);
    for (; s < nul; s++) {
        check(*s >= 0x20 && *s < 0x7F);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > XC_NBSTAT_MAX_ANSWER) {
        size = XC_NBSTAT_MAX_ANSWER;
    }
    xc_nbstat st;
    xc_err err = xc_nbstat_parse(data, size, 0x4242, &st);
    check(err == XC_OK || err == XC_ERR_CORRUPT);
    if (err == XC_OK) {
        check_name(st.name);
        check_name(st.group);
    }
    return 0;
}
