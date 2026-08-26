# MintPRINT top-level wrapper.
#
# Keep the existing Makefile as the build source of truth.  This GNUmakefile
# delegates normal targets to it and only post-processes packaged drawer icons
# so their saved Workbench windows are not the tiny 265x64 geometry in the
# source artwork.  Source .info files are never modified.

# These are real command targets in Makefile.  They need an explicit recipe
# here rather than relying on the catch-all % rule: .PHONY targets deliberately
# skip GNU make's implicit-rule search, so a phony `driver` would otherwise
# never reach `%:` (while a non-phony `driver` is shadowed by the real driver/
# source directory in this repository).
FORWARD_TARGETS := gui test test-http test-dpi test-jpeg test-ipp-enum \
                   test-postscript test-urf driver driver31 driver-symbols \
                   driver-symbols31 help

.PHONY: all release release31 release-all clean $(FORWARD_TARGETS)

all:
	$(MAKE) -f Makefile all

$(FORWARD_TARGETS):
	$(MAKE) -f Makefile $@

release:
	$(MAKE) -f Makefile release
	@# Classic Amiga drawer icons store the NewWindow height as a big-endian
	@# WORD at byte offset 84 (DiskObject header 78 + 6 bytes into DrawerData).
	@if [ -f release/MintPRINT.info ]; then \
		printf '\000\170' | dd of=release/MintPRINT.info bs=1 seek=84 conv=notrunc 2>/dev/null; \
		echo "Set drawer window height to 120: release/MintPRINT.info"; \
	fi

release31:
	$(MAKE) -f Makefile release31
	@if [ -f release/MintPRINT-OS31.info ]; then \
		printf '\000\170' | dd of=release/MintPRINT-OS31.info bs=1 seek=84 conv=notrunc 2>/dev/null; \
		echo "Set drawer window height to 120: release/MintPRINT-OS31.info"; \
	fi

release-all:
	$(MAKE) -f Makefile release-all
	@for icon in release/MintPRINT.info release/MintPRINT-OS31.info; do \
		if [ -f "$$icon" ]; then \
			printf '\000\170' | dd of="$$icon" bs=1 seek=84 conv=notrunc 2>/dev/null; \
			echo "Set drawer window height to 120: $$icon"; \
		fi; \
	done

clean:
	$(MAKE) -f Makefile clean

# Keep arbitrary/internal Makefile targets reachable too.  Public command
# targets above are explicit so directory-name collisions cannot intercept
# them.
%:
	$(MAKE) -f Makefile $@
