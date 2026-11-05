# 7z installs for an XBE (docs/formats/sevenz.md): core/src/sevenz.c and the LZMA SDK's 7z
# decoder subset (third_party/lzma, public domain), as cmake/lzma.cmake builds it.
# Included by an app Makefile after it sets ROOT_DIR and CORE_DIR, and before it includes
# nxdk's Makefile; it adds to SRCS.

LZMA_DIR := $(ROOT_DIR)/third_party/lzma/C
LZMA_SRCS := $(addprefix $(LZMA_DIR)/, 7zArcIn.c 7zBuf.c 7zDec.c 7zStream.c Bcj2.c Bra86.c \
	LzmaDec.c Lzma2Dec.c)
SEVENZ_SRCS := $(CORE_DIR)/src/sevenz.c
SRCS += $(SEVENZ_SRCS) $(LZMA_SRCS)

LZMA_FLAGS := -DZ7_NO_METHODS_FILTERS -isystem $(LZMA_DIR)

# Decoding at nxdk's default -O0 would be several times slower on the console's CPU; the SDK's
# own warnings aren't ours to fix (-w).
$(addsuffix .obj, $(basename $(LZMA_SRCS))): CFLAGS += -O2 -w $(LZMA_FLAGS)
$(addsuffix .obj, $(basename $(SEVENZ_SRCS))): CFLAGS += -O2 $(LZMA_FLAGS)
