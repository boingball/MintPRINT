CROSS   ?= m68k-amigaos-
CC       = $(CROSS)gcc
HOSTCC  ?= cc
NM       = $(CROSS)nm
CFLAGS  ?= -Os -m68000 -Wall -Wextra -fomit-frame-pointer -fno-builtin

IFF_DIR := Archive/Old JPEG Decode
IFF_DIR_ESC := Archive/Old\ JPEG\ Decode
DRIVER_BUILD := build/driver
DRIVER_OUT := $(DRIVER_BUILD)/MintPRINT
DRIVER31_BUILD := build/driver31
DRIVER31_OUT := $(DRIVER31_BUILD)/MintPRINT
TEST_BUILD := build/tests
RELEASE_DIR := release/MintPRINT

.PHONY: all gui test check test-http test-dpi test-jpeg test-ipp-enum test-postscript test-urf driver driver31 driver-symbols driver-symbols31 release clean help

all: gui

help:
	@echo "MintPRINT targets:"
	@echo "  make gui      - build MintPrint Settings (setup/test GUI)"
	@echo "  make test-http - run host-side HTTP response parser tests"
	@echo "  make test-dpi  - run host-side DPI option tests"
	@echo "  make test-jpeg - run host-side JPEG AAN forward-DCT tests"
	@echo "  make test-ipp-enum - run host-side IPP enum decode tests"
	@echo "  make driver   - build the experimental DEVS:Printers/MintPRINT driver"
	@echo "  make driver31 - build the AmigaOS 3.1-compatible classic printer driver"
	@echo "  make driver-symbols - show ABI symbols used by the driver"
	@echo "  make driver-symbols31 - show ABI symbols used by the OS3.1 driver"
	@echo "  make test-postscript - host-test and Ghostscript-validate the PostScript writer"
	@echo "  make test-urf - run host-side Apple Raster (URF) writer tests"
	@echo "  make release  - build both drivers and the GUI, stage one distributable"
	@echo "                  bundle (release/MintPRINT/) with both drivers under"
	@echo "                  Drivers/ - MintPrintSettings picks the right one at runtime"
	@echo "  make test     - run host-side geometry regression tests"
	@echo "  make check    - run every host-side test (test target plus HTTP,"
	@echo "                  IPP-enum and Ghostscript-validated PostScript) -"
	@echo "                  what CI runs"
	@echo "  make clean"

gui: MintPrintSettings

MintPrintSettings: src/MintPrintSettings.c src/http_response.c src/http_response.h src/dpi_options.c src/dpi_options.h src/ipp_enum.c src/ipp_enum.h driver/media_size.c driver/media_size.h src/lodepng.c src/lodepng.h $(IFF_DIR_ESC)/iff-loader.c $(IFF_DIR_ESC)/iff-loader.h
	$(CC) -O2 -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS -DLODEPNG_NO_COMPILE_ERROR_TEXT -I"$(IFF_DIR)" -Isrc -Idriver -o $@ src/MintPrintSettings.c src/http_response.c src/dpi_options.c src/ipp_enum.c src/lodepng.c driver/media_size.c "$(IFF_DIR)/iff-loader.c" -lamiga -lm

$(TEST_BUILD):
	mkdir -p $@

test-http: | $(TEST_BUILD)
	$(HOSTCC) -std=c89 -Wall -Wextra -pedantic -Isrc \
		tests/test_http_response.c src/http_response.c -o $(TEST_BUILD)/test_http_response
	$(TEST_BUILD)/test_http_response

test-dpi: | $(TEST_BUILD)
	$(HOSTCC) -std=c89 -Wall -Wextra -Werror -Isrc \
		tests/test_dpi_options.c src/dpi_options.c -o $(TEST_BUILD)/test_dpi_options
	$(TEST_BUILD)/test_dpi_options

test-jpeg: | $(TEST_BUILD)
	$(HOSTCC) -std=c89 -Wall -Wextra -Werror -Idriver \
		tests/test_jpeg_writer.c driver/jpeg_writer.c -o $(TEST_BUILD)/test_jpeg_writer
	$(TEST_BUILD)/test_jpeg_writer

test-ipp-enum: | $(TEST_BUILD)
	$(HOSTCC) -std=c89 -Wall -Wextra -Werror -Isrc \
		tests/test_ipp_enum.c src/ipp_enum.c -o $(TEST_BUILD)/test_ipp_enum
	$(TEST_BUILD)/test_ipp_enum

$(DRIVER_BUILD):
	mkdir -p $@

$(DRIVER31_BUILD):
	mkdir -p $@

$(DRIVER_BUILD)/printertag.o: driver/printertag.s | $(DRIVER_BUILD)
	$(CC) -m68000 -c $< -o $@

$(DRIVER_BUILD)/driver_core.o: driver/driver_core.c | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DRIVER_BUILD)/command_table.o: driver/command_table.c | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DRIVER_BUILD)/config.o: driver/config.c driver/config.h | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DRIVER_BUILD)/media_size.o: driver/media_size.c driver/media_size.h | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DRIVER_BUILD)/jpeg_writer.o: driver/jpeg_writer.c driver/jpeg_writer.h | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DRIVER_BUILD)/pwg_writer.o: driver/pwg_writer.c driver/pwg_writer.h | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DRIVER_BUILD)/pdf_writer.o: driver/pdf_writer.c driver/pdf_writer.h driver/jpeg_writer.h | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DRIVER_BUILD)/postscript_writer.o: driver/postscript_writer.c driver/postscript_writer.h driver/jpeg_writer.h | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DRIVER_BUILD)/urf_writer.o: driver/urf_writer.c driver/urf_writer.h | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DRIVER_BUILD)/ipp_client.o: driver/ipp_client.c driver/ipp_client.h src/http_response.h | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(DRIVER_BUILD)/http_response.o: src/http_response.c src/http_response.h | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(DRIVER_BUILD)/spool.o: driver/spool.c driver/spool.h | $(DRIVER_BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DRIVER_OUT): $(DRIVER_BUILD)/printertag.o $(DRIVER_BUILD)/driver_core.o $(DRIVER_BUILD)/command_table.o $(DRIVER_BUILD)/config.o $(DRIVER_BUILD)/media_size.o $(DRIVER_BUILD)/jpeg_writer.o $(DRIVER_BUILD)/pwg_writer.o $(DRIVER_BUILD)/pdf_writer.o $(DRIVER_BUILD)/postscript_writer.o $(DRIVER_BUILD)/urf_writer.o $(DRIVER_BUILD)/ipp_client.o $(DRIVER_BUILD)/http_response.o $(DRIVER_BUILD)/spool.o
	$(CC) -m68000 -nostartfiles -Wl,-Map,$(DRIVER_BUILD)/MintPRINT.map \
		-o $@ $^ -lamiga

# AmigaOS 3.1 compatibility driver.
#
# printer.device V40 does not understand the V44 extended PED/tag interface
# (PRTA_NoIO / PRTA_8BitGuns).  The classic printer tag therefore exposes the
# pre-V44 PrinterExtendedData layout and the Render shim expands printer.device's
# native 4-bit-per-gun Y/M/C/B intensities to the 8-bit values used internally
# by the existing JPEG/PostScript/PWG/PDF pipeline.
#
# Only driver_core.c is rebuilt with Render renamed.  Everything below the
# printer.device ABI boundary is shared bit-for-bit with the normal driver.
$(DRIVER31_BUILD)/printertag.o: driver/printertag_classic.s | $(DRIVER31_BUILD)
	$(CC) -m68000 -c $< -o $@

$(DRIVER31_BUILD)/driver_core.o: driver/driver_core.c | $(DRIVER31_BUILD)
	$(CC) $(CFLAGS) -DRender=MintPRINT_RenderCore -c $< -o $@

$(DRIVER31_BUILD)/classic_render_shim.o: driver/classic_render_shim.c | $(DRIVER31_BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(DRIVER31_OUT): $(DRIVER31_BUILD)/printertag.o $(DRIVER31_BUILD)/classic_render_shim.o $(DRIVER31_BUILD)/driver_core.o $(DRIVER_BUILD)/command_table.o $(DRIVER_BUILD)/config.o $(DRIVER_BUILD)/media_size.o $(DRIVER_BUILD)/jpeg_writer.o $(DRIVER_BUILD)/pwg_writer.o $(DRIVER_BUILD)/pdf_writer.o $(DRIVER_BUILD)/postscript_writer.o $(DRIVER_BUILD)/urf_writer.o $(DRIVER_BUILD)/ipp_client.o $(DRIVER_BUILD)/http_response.o $(DRIVER_BUILD)/spool.o
	$(CC) -m68000 -nostartfiles -Wl,-Map,$(DRIVER31_BUILD)/MintPRINT.map \
		-o $@ $^ -lamiga

# The printer tag assembly expects classic Amiga leading-underscore C symbols.
# This target makes ABI mismatches obvious before installing anything on AmigaOS.
driver-symbols: $(DRIVER_BUILD)/driver_core.o $(DRIVER_BUILD)/command_table.o
	$(NM) $(DRIVER_BUILD)/driver_core.o | grep -E '(_Init|_Expunge|_DriverOpen|_DriverClose|_DoSpecial|_Render|_DriverTags|_PEDData)' || true
	$(NM) $(DRIVER_BUILD)/command_table.o | grep -E '_CommandTable' || true

driver-symbols31: $(DRIVER31_BUILD)/driver_core.o $(DRIVER31_BUILD)/classic_render_shim.o $(DRIVER_BUILD)/command_table.o
	$(NM) $(DRIVER31_BUILD)/driver_core.o | grep -E '(_Init|_Expunge|_DriverOpen|_DriverClose|_DoSpecial|_MintPRINT_RenderCore|_DriverTags)' || true
	$(NM) $(DRIVER31_BUILD)/classic_render_shim.o | grep -E '(_Render|_MintPRINT_RenderCore)' || true
	$(NM) $(DRIVER_BUILD)/command_table.o | grep -E '_CommandTable' || true

test: test-dpi test-jpeg test-urf | $(TEST_BUILD)
	$(HOSTCC) -std=c89 -Wall -Wextra -Werror -Idriver \
		tests/test_media_size.c driver/media_size.c driver/pwg_writer.c \
		-o $(TEST_BUILD)/test_media_size
	$(TEST_BUILD)/test_media_size

test-urf: | $(TEST_BUILD)
	$(HOSTCC) -std=c89 -Wall -Wextra -Werror -Idriver \
		tests/test_urf_writer.c driver/urf_writer.c -o $(TEST_BUILD)/test_urf_writer
	$(TEST_BUILD)/test_urf_writer

# Everything host-testable in one target: `test` (DPI/JPEG/URF/media-size)
# plus the three suites it leaves out (HTTP response parsing, IPP-enum
# decoding, and the Ghostscript-validated PostScript writer, which needs
# `gs` on PATH). This is what CI runs; it never touches the m68k cross
# toolchain, so it needs no Amiga SDK to work.
check: test test-http test-ipp-enum test-postscript

driver: $(DRIVER_OUT)
	@echo
	@echo "Built experimental printer driver: $(DRIVER_OUT)"
	@echo "Read docs/PRINTER_DEVICE_SPIKE.md before installing it."

driver31: $(DRIVER31_OUT)
	@echo
	@echo "Built AmigaOS 3.1 compatibility driver: $(DRIVER31_OUT)"
	@echo "Requires a bsdsocket.library-compatible TCP/IP stack (Roadshow/AmiTCP/Miami etc.)."
	@echo "See docs/OS31_SUPPORT.md before installing it."

POSTSCRIPT_TEST := $(TEST_BUILD)/test_postscript_writer
POSTSCRIPT_TEST_PS := $(TEST_BUILD)/test-postscript.ps

$(POSTSCRIPT_TEST): tests/test_postscript_writer.c driver/postscript_writer.c driver/postscript_writer.h driver/jpeg_writer.c driver/jpeg_writer.h | $(TEST_BUILD)
	$(HOSTCC) -std=c90 -pedantic -Wall -Wextra -Idriver \
		tests/test_postscript_writer.c driver/postscript_writer.c driver/jpeg_writer.c \
		-o $@

test-postscript: $(POSTSCRIPT_TEST)
	$(POSTSCRIPT_TEST) $(POSTSCRIPT_TEST_PS)
	gs -q -dNOPAUSE -dBATCH -sDEVICE=nullpage $(POSTSCRIPT_TEST_PS)

ART_DIR := art

# Stages ONE distributable bundle containing both driver builds under
# Drivers/. MintPrintSettings detects this machine's AmigaOS/printer.device
# generation at runtime (mp_driver_src_path() in src/MintPrintSettings.c)
# and installs whichever Drivers/MintPRINT-<variant>/MintPRINT applies, so
# there is no separate "which bundle do I download" step any more; the
# Install script (see Install at the repository root) offers the same
# auto-detection for anyone who prefers the classic Amiga install flow.
#
# Icons are copied from $(ART_DIR)/ if present there, matching AmigaOS
# icon placement: a drawer's icon lives in its PARENT directory (so
# MintPRINT.info lands next to $(RELEASE_DIR), not inside it), while an
# application's icon sits right next to its binary. The driver binaries
# deliberately get no icon at all - they are printer.device driver
# segments, not runnable programs, and double-clicking one is unsafe.
release: gui driver driver31
	mkdir -p $(RELEASE_DIR)
	cp MintPrintSettings $(RELEASE_DIR)/
	cp docs/MintPrintSettings.guide $(RELEASE_DIR)/
	mkdir -p $(RELEASE_DIR)/Art
	cp $(ART_DIR)/single.iff $(ART_DIR)/longside.iff $(ART_DIR)/shortside.iff $(RELEASE_DIR)/Art/
	mkdir -p $(RELEASE_DIR)/Drivers/MintPRINT-V44 $(RELEASE_DIR)/Drivers/MintPRINT-OS31
	cp $(DRIVER_OUT) $(RELEASE_DIR)/Drivers/MintPRINT-V44/MintPRINT
	cp $(DRIVER31_OUT) $(RELEASE_DIR)/Drivers/MintPRINT-OS31/MintPRINT
	cp Install release/Install
	cp Aminet/MintPRINT.readme release/MintPRINT.readme
	@if [ -f $(ART_DIR)/Install.info ]; then \
		cp $(ART_DIR)/Install.info release/Install.info; \
		echo "Copied $(ART_DIR)/Install.info -> release/Install.info"; \
	else \
		echo "No $(ART_DIR)/Install.info found - Install script will have no icon (Workbench can't run it without one)"; \
	fi
	@if [ -f $(ART_DIR)/MintPrintSettings.info ]; then \
		cp $(ART_DIR)/MintPrintSettings.info $(RELEASE_DIR)/; \
		echo "Copied $(ART_DIR)/MintPrintSettings.info -> $(RELEASE_DIR)/"; \
	else \
		echo "No $(ART_DIR)/MintPrintSettings.info found - application will have no icon"; \
	fi
	@if [ -f $(ART_DIR)/MintPRINT.info ]; then \
		cp $(ART_DIR)/MintPRINT.info release/MintPRINT.info; \
		echo "Copied $(ART_DIR)/MintPRINT.info -> release/MintPRINT.info (drawer icon)"; \
	else \
		echo "No $(ART_DIR)/MintPRINT.info found - release drawer will have no icon"; \
	fi
	@echo
	@echo "Release bundle staged in $(RELEASE_DIR)/:"
	@echo "  MintPrintSettings         - run this; it detects your AmigaOS/"
	@echo "                              printer.device version and offers to"
	@echo "                              install the matching driver below"
	@echo "  Drivers/MintPRINT-V44/    - driver for AmigaOS 3.2, 3.5, 3.9 (V44+)"
	@echo "  Drivers/MintPRINT-OS31/   - classic driver for AmigaOS 3.0/3.1"
	@echo "  MintPrintSettings.info    - if $(ART_DIR)/ had one"
	@echo "  MintPrintSettings.guide   - Help menu > MintPrint Settings Help..."
	@echo
	@echo "release/Install           - classic AmigaDOS Installer script, an"
	@echo "                            alternative to running MintPrintSettings"
	@echo "release/Install.info      - if $(ART_DIR)/ had one; Workbench needs"
	@echo "                            this to run Install by double-clicking it"
	@echo "release/MintPRINT.info    - the drawer's own icon, if $(ART_DIR)/ had one"
	@echo "release/MintPRINT.readme  - the Aminet readme, staged next to the"
	@echo "drawer (not inside it) per Aminet convention: name it to match"
	@echo "whatever .lha/.zip archive you make of $(RELEASE_DIR)/."

clean:
	rm -rf build release MintPrintSettings
