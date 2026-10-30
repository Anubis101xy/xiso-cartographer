#ifndef XCAT_CLI_SMB_CMDS_H
#define XCAT_CLI_SMB_CMDS_H

/* xcat-cli's SMB commands and the SMB parts of map and install (docs/formats/smb.md). Built
   when the host has libsmb2 (XCAT_SMB). */

#include <stdbool.h>

#include "xcat/credentials.h"
#include "xcat/smb.h"
#include "xcat/source.h"

/* smb-scan, smb-shares, smb-ls, smb-hash: *handled says whether argv[1] was one of them. */
int xcat_cli_smb(int argc, char **argv, bool *handled);

/* The usage lines of those commands. */
const char *xcat_cli_smb_usage(void);

/* The map's @smb lister (xc_map_smb_list_fn); ctx is an xc_credentials * or NULL. */
xc_err xcat_cli_smb_list(void *ctx, const xc_smb_url *folder, xc_smb_listing *out);

/* Opens the image at an smb:// URL with the login `creds` gives its host, or the game folder
   at one that ends in '/' (smb.md §3.1). *handle is the caller's to close with
   xcat_cli_smb_close. */
xc_err xcat_cli_smb_open(const char *url, const xc_credentials *creds, xc_source *src,
                         void **handle);
/* A game folder's validator (folder-image.md §5), the install's ETag; NULL for an image. */
const char *xcat_cli_smb_validator(void *handle);
void xcat_cli_smb_close(void *handle);

#endif /* XCAT_CLI_SMB_CMDS_H */
