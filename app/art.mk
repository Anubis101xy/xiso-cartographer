# A game's picture and details (docs/formats/art.md, ADR 0019): where they're looked for, the
# XBE's title image (XPR0), covers in JPEG and PNG through stb_image (third_party/stb, public
# domain or MIT), and XBMC4Gamers synopses. Included by an app Makefile after it sets ROOT_DIR
# and CORE_DIR, and before it includes nxdk's Makefile; it adds to SRCS.

STB_DIR := $(ROOT_DIR)/third_party/stb
ART_SRCS := $(CORE_DIR)/src/art.c $(CORE_DIR)/src/picture.c $(CORE_DIR)/src/synopsis.c \
	$(CORE_DIR)/src/xpr.c
STB_SRCS := $(CORE_DIR)/src/stb_image_impl.c
SRCS += $(ART_SRCS) $(STB_SRCS)

# Decoding a JPEG at nxdk's default -O0 would take a good part of a second on the console's CPU.
# stb's own warnings aren't ours to fix (-w); its asserts are off here (stb_image_impl.c), so a
# picture that trips one is refused rather than stopping the app.
# -fwrapv: a crafted JPEG overflows stb's int arithmetic, which must then wrap (cmake/stb.cmake).
$(addsuffix .obj, $(basename $(STB_SRCS))): CFLAGS += -O2 -w -fwrapv -isystem $(STB_DIR)
$(addsuffix .obj, $(basename $(ART_SRCS))): CFLAGS += -O2
