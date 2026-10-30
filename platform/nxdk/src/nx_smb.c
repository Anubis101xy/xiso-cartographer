/*
 * SMB on the console (docs/formats/smb.md, ADR 0018): strong random bytes for libsmb2 (its
 * NTLMv2 client challenge and SMB3 nonces), from an entropy pool gathered like TLS's (ADR
 * 0012 §6), since the console has no generator of its own.
 */
#include "xcat/nxdk.h"

#include "xcat/tls.h"

/* libsmb2 is built with SMB2_RANDOM_BYTES_HOOK=xc_nx_smb2_random (app/smb.mk). */
int xc_nx_smb2_random(void *buf, size_t len);

int xc_nx_smb2_random(void *buf, size_t len)
{
    static xc_entropy_pool *pool;
    static xc_entropy entropy;
    static bool ready;
    if (!ready) {
        xc_nx_entropy_stats st;
        if ((pool == NULL && xc_entropy_pool_create(&pool) != XC_OK) ||
            xc_nx_entropy_gather(pool, &st) != XC_OK) {
            return -1; /* libsmb2 falls back to its own sequence */
        }
        xc_entropy_pool_source(pool, &entropy);
        ready = true;
    }
    return entropy.get(entropy.ctx, buf, len) == XC_OK ? 0 : -1;
}
