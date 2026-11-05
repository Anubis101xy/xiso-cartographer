# SMB sources for an XBE (docs/formats/smb.md, ADR 0018): libsmb2 with its nxdk port
# (third_party/libsmb2/xcat/README.md), the SMB client and NAS scan (platform/smb), and the
# console's random bytes for libsmb2 (platform/nxdk/src/nx_smb.c). Included by an app Makefile
# after it sets ROOT_DIR, CORE_DIR and PLATFORM_DIR, and before nxdk's Makefile; it adds to SRCS.
# core/src/smb.c, md4.c and netbios.c are in the app's SRCS: credentials.c and map.c use them.

SMB2_DIR := $(ROOT_DIR)/third_party/libsmb2
SMB2_SRCS := $(addprefix $(SMB2_DIR)/lib/, \
	aes.c aes_reference.c aes128ccm.c alloc.c asn1-ber.c compat.c errors.c hmac.c hmac-md5.c \
	init.c libsmb2.c md4c.c md5.c ntlmssp.c pdu.c quic.c smbdirect.c sha1.c sha224-256.c \
	sha384-512.c smb2-cmd-cancel.c smb2-cmd-close.c smb2-cmd-create.c smb2-cmd-echo.c \
	smb2-cmd-error.c smb2-cmd-flush.c smb2-cmd-ioctl.c smb2-cmd-lock.c smb2-cmd-logoff.c \
	smb2-cmd-negotiate.c smb2-cmd-notify-change.c smb2-cmd-oplock-break.c \
	smb2-cmd-query-directory.c smb2-cmd-query-info.c smb2-cmd-read.c smb2-cmd-session-setup.c \
	smb2-cmd-set-info.c smb2-cmd-tree-connect.c smb2-cmd-tree-disconnect.c smb2-cmd-write.c \
	smb2-data-file-info.c smb2-data-filesystem-info.c smb2-data-security-descriptor.c \
	smb2-data-reparse-point.c smb2-share-enum.c smb3-seal.c smb2-signing.c socket.c \
	spnego-wrapper.c sync.c timestamps.c unicode.c usha.c)
SMB_SRCS := $(ROOT_DIR)/platform/smb/src/smb_client.c $(ROOT_DIR)/platform/smb/src/smb_scan.c \
	$(PLATFORM_DIR)/src/nx_smb.c
SRCS += $(SMB2_SRCS) $(SMB_SRCS)

# libsmb2's public headers are SYSTEM, so they can't fail our -Werror.
CFLAGS += -I$(ROOT_DIR)/platform/smb/include -isystem $(SMB2_DIR)/include

# libsmb2 itself: its hand-written config for nxdk, the console's random bytes, -O2 (SMB3
# signing and the NT hash run on every packet or login), and its own warnings left to it (-w).
SMB2_OBJS := $(addsuffix .obj, $(basename $(SMB2_SRCS)))
$(SMB2_OBJS): CFLAGS += -O2 -w -DHAVE_CONFIG_H -D_U_= \
	-DSMB2_RANDOM_BYTES_HOOK=xc_nx_smb2_random \
	-DNEED_GETLOGIN_R -DNEED_GETPID -DNEED_RANDOM -DNEED_SRANDOM \
	-iquote $(SMB2_DIR)/xcat/nxdk -iquote $(SMB2_DIR)/lib -I$(SMB2_DIR)/include/smb2
