# Zip installs for an XBE (ADR 0009): the zip reader (core/src/zip.c, zip_read.c), sequential
# extraction (core/src/seq.c) and miniz's inflater, as cmake/miniz.cmake builds it.
# Included by an app Makefile after it sets ROOT_DIR and CORE_DIR, and before it includes
# nxdk's Makefile; it adds to SRCS.

MINIZ_DIR := $(ROOT_DIR)/third_party/miniz
MINIZ_SRC := $(MINIZ_DIR)/miniz_tinfl.c
ZIP_SRCS := $(CORE_DIR)/src/zip.c $(CORE_DIR)/src/zip_read.c $(CORE_DIR)/src/zip_folder.c \
	$(CORE_DIR)/src/seq.c
SRCS += $(ZIP_SRCS) $(MINIZ_SRC)

# Same defines as cmake/miniz.cmake: only the inflater, no allocation, stdio or time. Only
# zip_read.c and miniz itself include miniz's headers. nxdk's SDL2_image has an older
# miniz.h on the include path, so ours comes first for quoted includes (-iquote).
MINIZ_FLAGS := -DMINIZ_NO_MALLOC -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS \
	-DMINIZ_NO_ZLIB_APIS -DMINIZ_NO_DEFLATE_APIS -iquote $(MINIZ_DIR) -iquote $(ROOT_DIR)/cmake/miniz

# nxdk compiles without optimisation by default; Deflate at -O0 would be several times slower
# on the console's CPU. miniz's own warnings aren't ours to fix (-w).
$(addsuffix .obj, $(basename $(MINIZ_SRC))): CFLAGS += -O2 -w $(MINIZ_FLAGS)
$(addsuffix .obj, $(basename $(ZIP_SRCS))): CFLAGS += -O2 -I$(CORE_DIR)/src $(MINIZ_FLAGS)
