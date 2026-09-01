from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    s = p.read_text()
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"{path}: expected one anchor, found {n}: {old[:100]!r}")
    p.write_text(s.replace(old, new, 1))

# Standard release builds must not expose the developer regression-suite button.
# A deliberately separate MINTPRINT_RELEASE_TEST build keeps the functionality
# available without making it part of the public UI.
replace_once(
    "src/MintPrintSettings.c",
    '''    /* Developer regression capture. Stays on the existing button row
     * so the OS 2.x-safe window geometry does not grow again. */
    ng.ng_LeftEdge = 105;
    ng.ng_Width = 90;
    ng.ng_GadgetText = (STRPTR)"Test _Suite";
    ng.ng_GadgetID = GAD_TEST_SUITE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_',
        TAG_DONE);
    if (!gad) {
        printf("Failed to create test suite button\\n");
        return NULL;
    }

    // Enable/disable diagnostic logs and retained rendered jobs. Shares
''',
    '''#ifdef MINTPRINT_RELEASE_TEST
    /* Developer regression capture. Public release builds deliberately do
     * not create this gadget; make releasetest defines the build flag and
     * stages a separate QA bundle so it cannot be confused with the public
     * release drawer. */
    ng.ng_LeftEdge = 105;
    ng.ng_Width = 90;
    ng.ng_GadgetText = (STRPTR)"Test _Suite";
    ng.ng_GadgetID = GAD_TEST_SUITE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_',
        TAG_DONE);
    if (!gad) {
        printf("Failed to create test suite button\\n");
        return NULL;
    }
#endif

    // Enable/disable diagnostic logs and retained rendered jobs. Shares
''')

replace_once(
    "src/MintPrintSettings.c",
    '''    ng.ng_LeftEdge = 200;
    ng.ng_TopEdge = 198 + topborder;
    ng.ng_Width = 90;
''',
    '''#ifdef MINTPRINT_RELEASE_TEST
    ng.ng_LeftEdge = 200;
#else
    /* Four public-release controls are spaced evenly across the row when
     * the developer-only Test Suite gadget is absent. */
    ng.ng_LeftEdge = 140;
#endif
    ng.ng_TopEdge = 198 + topborder;
    ng.ng_Width = 90;
''')

replace_once(
    "src/MintPrintSettings.c",
    '''    // Save button - same action as File -> Save Driver Settings.
    ng.ng_LeftEdge = 300;
''',
    '''    // Save button - same action as File -> Save Driver Settings.
#ifdef MINTPRINT_RELEASE_TEST
    ng.ng_LeftEdge = 300;
#else
    ng.ng_LeftEdge = 270;
#endif
''')

replace_once(
    "src/MintPrintSettings.c",
    '''                        case GAD_TEST_SUITE:
                            mp_test_suite_start(win);
                            break;

                        case GAD_SAVE_BUTTON:
''',
    '''#ifdef MINTPRINT_RELEASE_TEST
                        case GAD_TEST_SUITE:
                            mp_test_suite_start(win);
                            break;
#endif

                        case GAD_SAVE_BUTTON:
''')

# Make a separate QA executable/bundle. `release` always uses the normal GUI;
# `releasetest` first stages that normal release, copies it to a different
# drawer, then replaces only the GUI in the copy with the flagged build.
replace_once(
    "Makefile",
    "RELEASE_DIR := release/MintPRINT\n",
    "RELEASE_DIR := release/MintPRINT\nRELEASETEST_DIR := release/MintPRINT-ReleaseTest\n")
replace_once(
    "Makefile",
    ".PHONY: all gui test check test-http test-dpi test-jpeg test-ipp-enum test-mdns test-postscript test-urf test-graphics-boundary driver driver31 driver-symbols driver-symbols31 release clean help\n",
    ".PHONY: all gui gui-test test check test-http test-dpi test-jpeg test-ipp-enum test-mdns test-postscript test-urf test-graphics-boundary driver driver31 driver-symbols driver-symbols31 release releasetest clean help\n")
replace_once(
    "Makefile",
    '''\t@echo "  make release  - build both drivers and the GUI, stage one distributable"\n\t@echo "                  bundle (release/MintPRINT/) with both drivers under"\n\t@echo "                  Drivers/ - MintPrintSettings picks the right one at runtime"\n''',
    '''\t@echo "  make release  - build both drivers and the public GUI, stage one distributable"\n\t@echo "                  bundle (release/MintPRINT/) with both drivers under"\n\t@echo "                  Drivers/ - MintPrintSettings picks the right one at runtime"\n\t@echo "  make releasetest - also stage release/MintPRINT-ReleaseTest/ with the"\n\t@echo "                     developer-only Test Suite button enabled"\n''')
replace_once(
    "Makefile",
    "gui: MintPrintSettings\n\n",
    "gui: MintPrintSettings\n\ngui-test: MintPrintSettingsReleaseTest\n\n")

regular_dep = '''MintPrintSettings: src/MintPrintSettings.c src/http_response.c src/http_response.h src/dpi_options.c src/dpi_options.h src/ipp_enum.c src/ipp_enum.h src/mdns_endpoint.c src/mdns_endpoint.h driver/media_size.c driver/media_size.h driver/ipp_client.c driver/ipp_client.h src/lodepng.c src/lodepng.h $(IFF_DIR_ESC)/iff-loader.c $(IFF_DIR_ESC)/iff-loader.h
\t$(CC) -O2 -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS -DLODEPNG_NO_COMPILE_ERROR_TEXT -I"$(IFF_DIR)" -Isrc -Idriver -o $@ src/MintPrintSettings.c src/http_response.c src/dpi_options.c src/ipp_enum.c src/mdns_endpoint.c src/lodepng.c driver/media_size.c driver/ipp_client.c "$(IFF_DIR)/iff-loader.c" -lamiga -lm
'''
test_target = regular_dep + '''
MintPrintSettingsReleaseTest: src/MintPrintSettings.c src/http_response.c src/http_response.h src/dpi_options.c src/dpi_options.h src/ipp_enum.c src/ipp_enum.h src/mdns_endpoint.c src/mdns_endpoint.h driver/media_size.c driver/media_size.h driver/ipp_client.c driver/ipp_client.h src/lodepng.c src/lodepng.h $(IFF_DIR_ESC)/iff-loader.c $(IFF_DIR_ESC)/iff-loader.h
\t$(CC) -O2 -DMINTPRINT_RELEASE_TEST -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS -DLODEPNG_NO_COMPILE_ERROR_TEXT -I"$(IFF_DIR)" -Isrc -Idriver -o $@ src/MintPrintSettings.c src/http_response.c src/dpi_options.c src/ipp_enum.c src/mdns_endpoint.c src/lodepng.c driver/media_size.c driver/ipp_client.c "$(IFF_DIR)/iff-loader.c" -lamiga -lm
'''
replace_once("Makefile", regular_dep, test_target)

replace_once(
    "Makefile",
    '''\t@echo "drawer (not inside it) per Aminet convention: name it to match"\n\t@echo "whatever .lha/.zip archive you make of $(RELEASE_DIR)/."\n\nclean:\n\trm -rf build release MintPrintSettings\n''',
    '''\t@echo "drawer (not inside it) per Aminet convention: name it to match"\n\t@echo "whatever .lha/.zip archive you make of $(RELEASE_DIR)/."\n\n# Developer/QA bundle. Keep the ordinary release drawer pristine, then clone\n# it and replace only MintPrintSettings with the flagged build that exposes\n# the capture Test Suite button. Never package this drawer for Aminet/users.\nreleasetest: release MintPrintSettingsReleaseTest\n\trm -rf $(RELEASETEST_DIR)\n\tcp -a $(RELEASE_DIR) $(RELEASETEST_DIR)\n\tcp MintPrintSettingsReleaseTest $(RELEASETEST_DIR)/MintPrintSettings\n\t@echo\n\t@echo "Release-test bundle staged in $(RELEASETEST_DIR)/"\n\t@echo "  QA ONLY: Test Suite button is enabled in MintPrintSettings"\n\t@echo "  Public release remains unchanged in $(RELEASE_DIR)/"\n\nclean:\n\trm -rf build release MintPrintSettings MintPrintSettingsReleaseTest\n''')

# Developer documentation follows the now-hidden UI and the 41.16/33-case suite.
replace_once(
    "docs/MINTPRINT_PREFS.md",
    '''MintPrint Settings includes a **Test Suite** button for developer/regression
testing. It requires the installed `DEVS:Printers/MintPRINT` driver to be
**41.15 or newer**; Settings refuses to start the suite with an older driver
because older builds do not understand capture-only mode.

The suite creates a fresh `T:MintPRINT-TestSuite` drawer (or `-2`, `-3`, ...
if one already exists) and runs 32 normal `printer.device` Test Print
renders. **No IPP Print-Job is submitted to the printer.** Driver 41.15's
temporary capture-only config retains each JPEG, PWG Raster, PDF, PostScript
or Apple Raster document in that drawer instead. A matching driver log and
`manifest.txt` describe the exact engine, resolution, media, colour, quality,
scaling, sides, sheet-back transform and margin settings used for every case.
''',
    '''The public `make release` build deliberately **does not show** the
**Test Suite** button. It is a developer/regression facility, not an end-user
printing control. Build `make releasetest` to create the separate
`release/MintPRINT-ReleaseTest/` QA bundle with the button enabled; the normal
`release/MintPRINT/` drawer remains the public build.

The suite requires the installed `DEVS:Printers/MintPRINT` driver to be
**41.16 or newer**. It creates a fresh `T:MintPRINT-TestSuite` drawer (or `-2`,
`-3`, ... if one already exists) and runs 33 normal `printer.device` Test Print
renders. **No IPP Print-Job is submitted to the printer.** Capture-only mode
retains each JPEG, PWG Raster, PDF, PostScript or Apple Raster document plus a
byte-exact `.ipp` request sidecar in that drawer. A matching driver log and
`manifest.txt` describe the exact engine, resolution, media, colour, quality,
scaling, sides, sheet-back transform and margin settings used for every case.
''')

replace_once(
    "CHANGELOG.md",
    '''  CrossFeed/Feed transform sign combination is exercised. Suite completion
  text now explicitly says no printer job was sent.
''',
    '''  CrossFeed/Feed transform sign combination is exercised. Suite completion
  text now explicitly says no printer job was sent. The Test Suite button is
  hidden from ordinary `make release` builds; `make releasetest` stages a
  separate QA-only bundle with the developer control enabled.
''')

print("release-test gating patch applied")
