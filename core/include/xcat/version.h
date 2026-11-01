#ifndef XCAT_VERSION_H
#define XCAT_VERSION_H

#include <stdint.h>

/* Single source of truth for the project version; CMakeLists.txt parses these lines. */
#define XC_VERSION_MAJOR 0
#define XC_VERSION_MINOR 10
#define XC_VERSION_PATCH 1

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the version as "MAJOR.MINOR.PATCH". The string is static and never NULL. */
const char *xc_version_string(void);

/* Returns the version packed as (MAJOR << 16) | (MINOR << 8) | PATCH, for ordered comparison. */
uint32_t xc_version_number(void);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_VERSION_H */
