/* libsmb2's config.h for nxdk (original Xbox), written by hand for xiso-cartographer:
 * nxdk's C library (PDCLib + xboxrt) and lwIP's socket API. See ../README.md. */
#ifndef XCAT_LIBSMB2_NXDK_CONFIG_H
#define XCAT_LIBSMB2_NXDK_CONFIG_H

#define HAVE_ERRNO_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LINGER 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

#define PACKAGE "libsmb2"
#define PACKAGE_NAME "libsmb2"
#define PACKAGE_VERSION "6.2.0+xcat"
#define VERSION "6.2.0+xcat"

/* Byte order (x86): some of libsmb2's headers compare __BYTE_ORDER with __BIG_ENDIAN, which
 * would be true if neither were defined. */
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN

/* The helpers nxdk's C library lacks (NEED_GETLOGIN_R, NEED_GETPID, NEED_RANDOM,
 * NEED_SRANDOM: lib/compat.c) are switched on by the compiler flags in app/smb.mk, since
 * compat.c doesn't read this file. */

#endif
