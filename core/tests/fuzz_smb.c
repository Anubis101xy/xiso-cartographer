/*
 * libFuzzer target for smb:// URLs and image names (docs/formats/smb.md §1, §3).
 *
 * The input is a URL. Parsing must give XC_OK or XC_ERR_SMB_URL. A parsed URL must hold valid
 * names, format to its canonical form, and that form must parse back to the same URL. The
 * image name of the path, if it has one, must be a prefix of its last name. A violation
 * aborts. Seeds are in core/tests/fuzz-seeds/smb/.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xcat/smb.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(int ok)
{
    if (!ok) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static xc_smb_url u, back;
    xc_err err = xc_smb_url_parse((const char *)data, size, &u);
    check(err == XC_OK || err == XC_ERR_SMB_URL);
    if (err != XC_OK) {
        return 0;
    }
    check(u.port != 0 && u.host[0] != '\0');
    check(xc_smb_valid_name(u.share, strlen(u.share)));
    size_t len = strlen(u.path);
    check(len <= XC_SMB_MAX_PATH);
    for (const char *p = u.path; *p != '\0';) {
        size_t n = strcspn(p, "/");
        check(xc_smb_valid_name(p, n));
        p += n;
        if (*p == '/') {
            p++;
            check(*p != '\0');
        }
    }
    static char canon[XC_SMB_MAX_URL * 3];
    check(xc_smb_url_format(&u, canon, sizeof canon) == XC_OK);
    check(xc_smb_url_parse(canon, strlen(canon), &back) == XC_OK);
    check(strcmp(u.host, back.host) == 0 && u.port == back.port && u.is_ipv4 == back.is_ipv4 &&
          strcmp(u.share, back.share) == 0 && strcmp(u.path, back.path) == 0);
    char name[XC_SMB_MAX_NAME + 1];
    bool zip = false;
    if (xc_smb_image_name(u.path, name, sizeof name, &zip)) {
        const char *last = strrchr(u.path, '/');
        last = last != NULL ? last + 1 : u.path;
        check(name[0] != '\0' && strncmp(last, name, strlen(name)) == 0);
    }
    return 0;
}
