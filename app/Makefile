# XBE build (nxdk). Run through the top-level Makefile: `make xbe` inside the
# nxdk environment, or `make docker-xbe` from any machine with Docker.
#
# Output: app/bin/default.xbe, its fonts\ and ui\ folders (app/ui/ui-assets.mk),
# app/bin/samples/small.iso (bundled test image) and app/xiso-cartographer.iso (bootable in
# xemu; contains them all).

XBE_TITLE = xiso-cartographer
GEN_XISO = $(XBE_TITLE).iso

ROOT_DIR := $(CURDIR)/..
CORE_DIR := $(ROOT_DIR)/core
PLATFORM_DIR := $(ROOT_DIR)/platform/nxdk

# libxcat is compiled straight into the XBE: core/ is portable C with no
# platform dependencies, so it needs no separate library build here.
SRCS = $(CURDIR)/main.c \
       $(CORE_DIR)/src/archiveorg.c \
       $(CORE_DIR)/src/catalog.c \
       $(CORE_DIR)/src/crc32.c \
       $(CORE_DIR)/src/credentials.c \
       $(CORE_DIR)/src/error.c \
       $(CORE_DIR)/src/extract.c \
       $(CORE_DIR)/src/fatx.c \
       $(CORE_DIR)/src/http.c \
       $(CORE_DIR)/src/http_client.c \
       $(CORE_DIR)/src/install.c \
       $(CORE_DIR)/src/arc_folder.c \
       $(CORE_DIR)/src/folder_image.c \
       $(CORE_DIR)/src/installed.c \
       $(CORE_DIR)/src/journal.c \
       $(CORE_DIR)/src/json.c \
       $(CORE_DIR)/src/login.c \
       $(CORE_DIR)/src/map.c \
       $(CORE_DIR)/src/map_edit.c \
       $(CORE_DIR)/src/md4.c \
       $(CORE_DIR)/src/netbios.c \
       $(CORE_DIR)/src/settings.c \
       $(CORE_DIR)/src/smb.c \
       $(CORE_DIR)/src/source.c \
       $(CORE_DIR)/src/text.c \
       $(CORE_DIR)/src/ui_script.c \
       $(CORE_DIR)/src/url.c \
       $(CORE_DIR)/src/version.c \
       $(CORE_DIR)/src/xbe.c \
       $(CORE_DIR)/src/xdvdfs.c \
       $(PLATFORM_DIR)/src/nx_install.c \
       $(PLATFORM_DIR)/src/nx_net.c \
       $(PLATFORM_DIR)/src/nx_platform.c

CFLAGS += -I$(CORE_DIR)/include -I$(PLATFORM_DIR)/include -std=c11 -Wall -Wextra -Werror

# BearSSL: the whole-image hashes of installs (core/src/hash.c, journal.md §7), and HTTPS.
include $(CURDIR)/tls.mk

# Zip archives (ADR 0009): core/src/install.c installs their images.
include $(CURDIR)/zip.mk
include $(CURDIR)/sevenz.mk

# NAS shared folders over SMB (ADR 0018): libsmb2, the SMB client and the NAS scan.
include $(CURDIR)/smb.mk

# The screens (M8, ADR 0015): app/ui's sources, SDL2 and SDL_ttf; and the screens only this XBE
# has: the startup screen, and the UI script's player (it needs core/src/ui_script.c).
include $(ROOT_DIR)/app/ui/ui.mk
SRCS += $(ROOT_DIR)/app/ui/script.c $(ROOT_DIR)/app/ui/startup_view.c

NXDK_DIR ?= $(ROOT_DIR)/third_party/nxdk

# The walkthrough's disc and the title image's stamp (below) go with `make clean` too; set
# before nxdk's Makefile reads it.
CLEANRULES += clean-walkthrough clean-title-image

# TCP tuning shared by every XBE (lwIP options).
include $(ROOT_DIR)/app/lwip.mk

include $(NXDK_DIR)/Makefile

# The fonts and images next to the XBE, and -O2 for app/ui.
include $(ROOT_DIR)/app/ui/ui-assets.mk

# Bundled synthetic test image (the tools/fixtures "small" fixture: dummy data, never a
# game), so the M2 install can be tried without copying anything to the console first.
SAMPLE_ISO = $(OUTPUT_DIR)/samples/small.iso
$(SAMPLE_ISO): $(ROOT_DIR)/tools/fixtures/fixtures.py $(ROOT_DIR)/tools/fixtures/xisogen.py
	@mkdir -p $(OUTPUT_DIR)/samples
	@echo "[ SAMPLE   ] $@"
	$(VE)python3 $(ROOT_DIR)/tools/fixtures/fixtures.py --out $(OUTPUT_DIR)/samples --only small $(QUIET)
	$(VE)rm -f $(OUTPUT_DIR)/samples/small.json $(OUTPUT_DIR)/samples/small.tsv

all: $(SAMPLE_ISO)
$(GEN_XISO): $(SAMPLE_ISO)

# The XBE's title image, which dashboards show (docs/formats/xbe-title-image.md): cxbe can't add
# one, so tools/ui/xbe_title_image.py puts app/assets/title-image.png into default.xbe after cxbe
# made it. The stamp says this default.xbe has it; a new image or tool makes cxbe run again
# first, since the tool refuses an XBE that already has one.
TITLE_IMAGE = $(ROOT_DIR)/app/assets/title-image.png
TITLE_TOOL = $(ROOT_DIR)/tools/ui/xbe_title_image.py $(ROOT_DIR)/tools/ui/png2bmp.py
TITLE_STAMP = $(CURDIR)/title-image.stamp
$(OUTPUT_DIR)/default.xbe: $(TITLE_IMAGE) $(TITLE_TOOL)
$(TITLE_STAMP): $(OUTPUT_DIR)/default.xbe
	@echo "[ TITLE    ] $(TITLE_IMAGE) into default.xbe"
	$(VE)python3 -I $(ROOT_DIR)/tools/ui/xbe_title_image.py $(OUTPUT_DIR)/default.xbe $(TITLE_IMAGE)
	@touch $@

all: $(TITLE_STAMP)
$(GEN_XISO): $(TITLE_STAMP)

.PHONY: clean-title-image
clean-title-image:
	$(VE)rm -f $(TITLE_STAMP)

# The M8 exit test's scripted walkthrough (docs/testing.md): the same disc with
# tools/ui/walkthrough.txt next to the XBE as ui-script.txt, which the app plays
# (docs/formats/ui-script.md). For xemu and testing only; never shipped.
WALK_XISO = $(XBE_TITLE)-walkthrough.iso
WALK_DIR = $(CURDIR)/walkthrough-bin
$(WALK_XISO): $(GEN_XISO) $(ROOT_DIR)/tools/ui/walkthrough.txt
	@echo "[ XISO     ] $@"
	$(VE)rm -rf $(WALK_DIR) && cp -r $(OUTPUT_DIR) $(WALK_DIR)
	$(VE)cp $(ROOT_DIR)/tools/ui/walkthrough.txt $(WALK_DIR)/ui-script.txt
	$(VE)$(EXTRACT_XISO) -c $(WALK_DIR) $(XISO_FLAGS) "$@" $(QUIET)
	$(VE)rm -rf $(WALK_DIR)

all: $(WALK_XISO)

.PHONY: clean-walkthrough
clean-walkthrough:
	$(VE)rm -rf $(WALK_XISO) $(WALK_DIR)
