from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    s = p.read_text()
    count = s.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 anchor, found {count}")
    p.write_text(s.replace(old, new, 1))


# ---------------------------------------------------------------------------
# Driver config: one-shot test-suite source and capture-only path.
# ---------------------------------------------------------------------------
replace_once(
    "driver/config.h",
    "#define MP_CONFIG_ENGINE_MAX 16\n\n#define MP_CONFIG_SOURCE_DEFAULTS 0\n#define MP_CONFIG_SOURCE_ENV      1\n#define MP_CONFIG_SOURCE_ENVARC   2\n",
    "#define MP_CONFIG_ENGINE_MAX 16\n#define MP_CONFIG_CAPTURE_PATH_MAX 160\n\n#define MP_CONFIG_SOURCE_DEFAULTS 0\n#define MP_CONFIG_SOURCE_ENV      1\n#define MP_CONFIG_SOURCE_ENVARC   2\n#define MP_CONFIG_SOURCE_TEST     3\n",
    "config.h constants",
)

replace_once(
    "driver/config.h",
    "    BOOL spool_keep;\n    /* IPP media-*-margin values, in hundredths of a millimetre. */\n",
    "    BOOL spool_keep;\n    /* Development-only output regression capture. This is honoured only\n"
    "     * when config.c loaded T:MintPRINT-testsuite.cfg, never from a\n"
    "     * normal ENV:/ENVARC: Unit0. capture_path is restricted to T:. */\n"
    "    BOOL capture_only;\n"
    "    char capture_path[MP_CONFIG_CAPTURE_PATH_MAX];\n"
    "    /* IPP media-*-margin values, in hundredths of a millimetre. */\n",
    "config.h capture fields",
)

replace_once(
    "driver/config.c",
    "    cfg->spool_keep = FALSE;\n    cfg->margin_left_100mm = 0;\n",
    "    cfg->spool_keep = FALSE;\n"
    "    cfg->capture_only = FALSE;\n"
    "    cfg->capture_path[0] = 0;\n"
    "    cfg->margin_left_100mm = 0;\n",
    "config defaults capture",
)

replace_once(
    "driver/config.c",
    "    fh = Open((CONST_STRPTR)\"ENV:MintPRINT/Unit0\", MODE_OLDFILE);\n"
    "    if (fh) {\n"
    "        source = MP_CONFIG_SOURCE_ENV;\n"
    "    } else {\n"
    "        fh = Open((CONST_STRPTR)\"ENVARC:MintPRINT/Unit0\", MODE_OLDFILE);\n"
    "        if (fh) source = MP_CONFIG_SOURCE_ENVARC;\n"
    "    }\n\n"
    "    if (!fh) return source;\n",
    "    /* MintPrint Settings' regression suite writes a complete, one-job\n"
    "     * override here. It deliberately wins over Unit0 so the suite never\n"
    "     * mutates the user's saved printer profile. DriverOpen removes it\n"
    "     * after loading; Init may read it first when the driver is cold, so\n"
    "     * config.c itself must not consume/delete it. */\n"
    "    fh = Open((CONST_STRPTR)\"T:MintPRINT-testsuite.cfg\", MODE_OLDFILE);\n"
    "    if (fh) {\n"
    "        source = MP_CONFIG_SOURCE_TEST;\n"
    "    } else {\n"
    "        fh = Open((CONST_STRPTR)\"ENV:MintPRINT/Unit0\", MODE_OLDFILE);\n"
    "        if (fh) {\n"
    "            source = MP_CONFIG_SOURCE_ENV;\n"
    "        } else {\n"
    "            fh = Open((CONST_STRPTR)\"ENVARC:MintPRINT/Unit0\", MODE_OLDFILE);\n"
    "            if (fh) source = MP_CONFIG_SOURCE_ENVARC;\n"
    "        }\n"
    "    }\n\n"
    "    if (!fh) return source;\n",
    "config test override open",
)

replace_once(
    "driver/config.c",
    "        if (mp_cfg_starts(g_config_line, \"SPOOL_KEEP=\")) {\n"
    "            value = g_config_line + 11;\n"
    "            cfg->spool_keep = (value[0] == '0') ? FALSE : TRUE;\n"
    "            continue;\n"
    "        }\n"
    "        if (mp_cfg_starts(g_config_line, \"PWG_SHEET_BACK=\")) {\n",
    "        if (mp_cfg_starts(g_config_line, \"SPOOL_KEEP=\")) {\n"
    "            value = g_config_line + 11;\n"
    "            cfg->spool_keep = (value[0] == '0') ? FALSE : TRUE;\n"
    "            continue;\n"
    "        }\n"
    "        if (mp_cfg_starts(g_config_line, \"CAPTURE_ONLY=\")) {\n"
    "            value = g_config_line + 13;\n"
    "            cfg->capture_only = (value[0] == '0') ? FALSE : TRUE;\n"
    "            continue;\n"
    "        }\n"
    "        if (mp_cfg_starts(g_config_line, \"CAPTURE_PATH=\")) {\n"
    "            value = g_config_line + 13;\n"
    "            /* Regression captures are intentionally confined to T:. */\n"
    "            if (value[0] == 'T' && value[1] == ':')\n"
    "                mp_cfg_copy(cfg->capture_path, sizeof(cfg->capture_path), value);\n"
    "            continue;\n"
    "        }\n"
    "        if (mp_cfg_starts(g_config_line, \"PWG_SHEET_BACK=\")) {\n",
    "config capture parser",
)

replace_once(
    "driver/config.c",
    "    Close(fh);\n    return source;\n}\n",
    "    Close(fh);\n\n"
    "    /* CAPTURE_* is never a persisted Unit0 feature. Even a hand-edited\n"
    "     * ENV:/ENVARC: file containing those keys cannot disable printing.\n"
    "     * Only the dedicated T: test-suite override may enable it. */\n"
    "    if (source != MP_CONFIG_SOURCE_TEST) {\n"
    "        cfg->capture_only = FALSE;\n"
    "        cfg->capture_path[0] = 0;\n"
    "    } else if (cfg->capture_only) {\n"
    "        cfg->debug = TRUE;\n"
    "        mp_cfg_copy(cfg->spool, sizeof(cfg->spool), \"RAM\");\n"
    "        cfg->spool_keep = FALSE;\n"
    "    }\n"
    "    return source;\n}\n",
    "config capture source guard",
)


# ---------------------------------------------------------------------------
# Driver core: skip IPP entirely and retain named capture files.
# ---------------------------------------------------------------------------
replace_once(
    "driver/driver_core.c",
    "#define MP_DRIVER_SUBREV 14",
    "#define MP_DRIVER_SUBREV 15",
    "driver subrev",
)

replace_once(
    "driver/driver_core.c",
    "#define MP_SPOOL_PATH_MAX (MP_CONFIG_OPTION_MAX + 40) /* + \"MPSPOOL/\" + base name */",
    "#define MP_SPOOL_PATH_MAX (MP_CONFIG_CAPTURE_PATH_MAX + 8)",
    "driver path max",
)

replace_once(
    "driver/driver_core.c",
    "static CONST_STRPTR mp_job_filename(void)\n{\n    switch (g_engine) {",
    "static CONST_STRPTR mp_job_filename(void)\n{\n"
    "    if (g_config.capture_only && g_config.capture_path[0])\n"
    "        return (CONST_STRPTR)g_config.capture_path;\n"
    "    switch (g_engine) {",
    "capture filename",
)

replace_once(
    "driver/driver_core.c",
    "    if (source == MP_CONFIG_SOURCE_ENV)\n"
    "        mp_log_append(\"ENV\");\n"
    "    else if (source == MP_CONFIG_SOURCE_ENVARC)\n"
    "        mp_log_append(\"ENVARC\");\n"
    "    else\n"
    "        mp_log_append(\"defaults\");\n",
    "    if (source == MP_CONFIG_SOURCE_ENV)\n"
    "        mp_log_append(\"ENV\");\n"
    "    else if (source == MP_CONFIG_SOURCE_ENVARC)\n"
    "        mp_log_append(\"ENVARC\");\n"
    "    else if (source == MP_CONFIG_SOURCE_TEST)\n"
    "        mp_log_append(\"T: regression suite\");\n"
    "    else\n"
    "        mp_log_append(\"defaults\");\n",
    "config source log",
)

replace_once(
    "driver/driver_core.c",
    "    g_config_source = mp_spool_config_load(&g_config);\n"
    "    mp_build_spool_paths();\n"
    "    g_engine = mp_detect_engine(&g_config);\n",
    "    g_config_source = mp_spool_config_load(&g_config);\n"
    "    /* Init may have read this file first on a cold driver load. Consume\n"
    "     * it only here, after DriverOpen has definitely loaded the same\n"
    "     * capture configuration for the print request about to run. */\n"
    "    if (g_config_source == MP_CONFIG_SOURCE_TEST)\n"
    "        mp_spool_job_delete((CONST_STRPTR)\"T:MintPRINT-testsuite.cfg\");\n"
    "    mp_build_spool_paths();\n"
    "    g_engine = mp_detect_engine(&g_config);\n",
    "consume suite config in DriverOpen",
)

replace_once(
    "driver/driver_core.c",
    "    } else {\n"
    "        mp_write_job_status(\"SUBMITTING\", NULL);\n"
    "        ipp_rc = mp_spool_ipp_submit(&g_config, fname, fmt, &result);\n"
    "        mp_write_job_status(ipp_rc == 0 ? \"DONE\" : \"FAILED\",\n"
    "                            ipp_rc == 0 ? NULL : &result);\n"
    "    }\n"
    "    mp_log_ipp_result(\"IPP result error/http/status\", &result);\n",
    "    } else if (g_config.capture_only) {\n"
    "        /* Regression suite: the rendered document itself is the result.\n"
    "         * Never open a TCP connection, and report a synthetic success so\n"
    "         * normal page bookkeeping can finish. */\n"
    "        ipp_rc = 0;\n"
    "        result.error = 0;\n"
    "        result.http_status = 0;\n"
    "        result.ipp_status = 0;\n"
    "        result.document_bytes = g_job_file_bytes;\n"
    "        mp_log_text(\"Capture-only regression job retained; network submission skipped\");\n"
    "    } else {\n"
    "        mp_write_job_status(\"SUBMITTING\", NULL);\n"
    "        ipp_rc = mp_spool_ipp_submit(&g_config, fname, fmt, &result);\n"
    "        mp_write_job_status(ipp_rc == 0 ? \"DONE\" : \"FAILED\",\n"
    "                            ipp_rc == 0 ? NULL : &result);\n"
    "    }\n"
    "    mp_log_ipp_result(g_config.capture_only ?\n"
    "                      \"Capture result error/http/status\" :\n"
    "                      \"IPP result error/http/status\", &result);\n",
    "single page capture skip submit",
)

replace_once(
    "driver/driver_core.c",
    "    if (!g_config.debug && !mp_duplex_requested() && !g_job_status_path[0])\n"
    "        mp_spool_job_delete(fname);\n",
    "    if (!g_config.capture_only && !g_config.debug &&\n"
    "        !mp_duplex_requested() && !g_job_status_path[0])\n"
    "        mp_spool_job_delete(fname);\n",
    "single capture retention",
)

replace_once(
    "driver/driver_core.c",
    "        if (!g_duplex_job_failed) {\n"
    "            mp_write_job_status(\"SUBMITTING\", NULL);\n"
    "            ipp_rc = mp_spool_ipp_submit(&g_config, mp_job_filename(),\n"
    "                                          mp_document_format(), &result);\n"
    "            mp_log_ipp_result(\"IPP duplex Print-Job error/http/status\",\n"
    "                              &result);\n"
    "            if (ipp_rc != 0) g_duplex_job_failed = TRUE;\n"
    "            mp_write_job_status(ipp_rc == 0 ? \"DONE\" : \"FAILED\",\n"
    "                                ipp_rc == 0 ? NULL : &result);\n"
    "        } else {\n",
    "        if (!g_duplex_job_failed) {\n"
    "            if (g_config.capture_only) {\n"
    "                ipp_rc = 0;\n"
    "                result.error = 0;\n"
    "                result.http_status = 0;\n"
    "                result.ipp_status = 0;\n"
    "                result.document_bytes = g_job_file_bytes;\n"
    "                mp_log_text(\"Capture-only duplex job retained; network submission skipped\");\n"
    "                mp_log_ipp_result(\"Capture duplex result error/http/status\",\n"
    "                                  &result);\n"
    "            } else {\n"
    "                mp_write_job_status(\"SUBMITTING\", NULL);\n"
    "                ipp_rc = mp_spool_ipp_submit(&g_config, mp_job_filename(),\n"
    "                                              mp_document_format(), &result);\n"
    "                mp_log_ipp_result(\"IPP duplex Print-Job error/http/status\",\n"
    "                                  &result);\n"
    "                mp_write_job_status(ipp_rc == 0 ? \"DONE\" : \"FAILED\",\n"
    "                                    ipp_rc == 0 ? NULL : &result);\n"
    "            }\n"
    "            if (ipp_rc != 0) g_duplex_job_failed = TRUE;\n"
    "        } else {\n",
    "duplex capture skip submit",
)

replace_once(
    "driver/driver_core.c",
    "        if (!g_config.debug && !g_job_status_path[0])\n"
    "            mp_spool_job_delete(mp_job_filename());\n",
    "        if (!g_config.capture_only && !g_config.debug && !g_job_status_path[0])\n"
    "            mp_spool_job_delete(mp_job_filename());\n",
    "duplex capture retention",
)


# Version markers for both printer.device ABIs.
for tag in ("driver/printertag.s", "driver/printertag_classic.s"):
    p = Path(tag)
    s = p.read_text()
    old = '$VER: MintPRINT 41.14 (31.08.2026)'
    if s.count(old) != 1:
        raise SystemExit(f"{tag}: version anchor count {s.count(old)}")
    p.write_text(s.replace(old, '$VER: MintPRINT 41.15 (01.09.2026)', 1))


# ---------------------------------------------------------------------------
# Settings GUI: Test Suite button + asynchronous 32-case coverage runner.
# ---------------------------------------------------------------------------
replace_once(
    "src/MintPrintSettings.c",
    "#define GAD_VIEW_SPOOL 22\n",
    "#define GAD_VIEW_SPOOL 22\n#define GAD_TEST_SUITE 23\n",
    "suite gadget id",
)

suite_code = r'''

/* ---------------------------------------------------------------------
 * Output regression capture suite
 *
 * This is a developer/test facility, not another way to print. It runs the
 * normal printer.device DUMPRPORT test page through a bounded coverage
 * matrix and asks driver 41.15+ to retain each finished document under T:
 * without performing the IPP submission. The matrix is deliberately not a
 * Cartesian product: that would create hundreds/thousands of page-sized
 * files in T: (normally RAM:). Instead every engine sees every scaling mode,
 * while DPI, colour, quality and A4/Letter are crossed through those cases;
 * PWG/URF then get their duplex-specific cases too. A manifest records every
 * exact input so the captured files can be audited off-Amiga afterwards.
 * ------------------------------------------------------------------- */
#define MP_TEST_SUITE_CASES 32
#define MP_TEST_SUITE_PATH_MAX 160

struct MPTestSuiteCase {
    char engine[16];
    int resolution;
    char media[32];
    char source[24];
    char color[16];
    char quality[16];
    char scaling[16];
    char sides[24];
    char sheet_back[16];
    ULONG margin_100mm;
};

struct MPTestSuiteState {
    BOOL active;
    int current;
    char drawer[80];
    char output_path[MP_TEST_SUITE_PATH_MAX];
    char log_path[MP_TEST_SUITE_PATH_MAX];
    char old_engine[32];
    char old_media[MAX_ATTR_LEN];
    char old_source[MAX_ATTR_LEN];
    char old_color[MAX_ATTR_LEN];
    char old_quality[MAX_ATTR_LEN];
    char old_scaling[MAX_ATTR_LEN];
    char old_sides[MAX_ATTR_LEN];
    char old_selected_quality[sizeof(selected_quality)];
    char old_selected_scaling[sizeof(selected_scaling)];
    char old_selected_print_mode[sizeof(selected_print_mode)];
    char old_sheet_back[MAX_ATTR_LEN];
    int old_resolution;
    BOOL old_engine_explicit;
    BOOL old_resolution_explicit;
};

static struct MPTestSuiteState g_test_suite;
static void apply_driver_config_to_gadgets(struct Window *win);

static void mp_test_suite_copy_string(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static const char *mp_test_suite_engine_short(const char *engine)
{
    if (strcmp(engine, "pwg-raster") == 0) return "pwg";
    if (strcmp(engine, "postscript") == 0) return "ps";
    return engine;
}

static const char *mp_test_suite_extension(const char *engine)
{
    if (strcmp(engine, "pwg-raster") == 0) return "pwg";
    if (strcmp(engine, "postscript") == 0) return "ps";
    if (strcmp(engine, "urf") == 0) return "urf";
    if (strcmp(engine, "pdf") == 0) return "pdf";
    return "jpg";
}

static void mp_test_suite_case(int index, struct MPTestSuiteCase *c)
{
    static const char *engines[5] = {
        "jpeg", "pwg-raster", "pdf", "postscript", "urf"
    };
    static const char *scalings[5] = {
        "auto", "auto-fit", "fit", "fill", "none"
    };
    static const int dpis[5] = { 300, 600, 300, 600, 300 };
    static const char *colors[5] = {
        "color", "monochrome", "color", "monochrome", "color"
    };
    static const char *qualities[5] = {
        "normal", "draft", "high", "normal", "draft"
    };
    static const char *media[5] = {
        "iso_a4_210x297mm", "iso_a4_210x297mm", "na_letter_8.5x11in",
        "na_letter_8.5x11in", "iso_a4_210x297mm"
    };

    memset(c, 0, sizeof(*c));
    strcpy(c->source, "auto");
    strcpy(c->sides, "one-sided");
    strcpy(c->sheet_back, "normal");

    if (index < 25) {
        int e = index / 5;
        int v = index % 5;
        strcpy(c->engine, engines[e]);
        c->resolution = dpis[v];
        strcpy(c->media, media[v]);
        strcpy(c->color, colors[v]);
        strcpy(c->quality, qualities[v]);
        strcpy(c->scaling, scalings[v]);
        return;
    }

    c->resolution = 300;
    strcpy(c->media, "iso_a4_210x297mm");
    strcpy(c->color, "color");
    strcpy(c->quality, "normal");
    strcpy(c->scaling, "auto-fit");

    if (index < 29) {
        static const char *backs[4] = {
            "normal", "rotated", "flipped", "manual-tumble"
        };
        int v = index - 25;
        strcpy(c->engine, "pwg-raster");
        strcpy(c->sides, (v & 1) ? "two-sided-short-edge" :
                                  "two-sided-long-edge");
        strcpy(c->sheet_back, backs[v]);
    } else if (index < 31) {
        strcpy(c->engine, "urf");
        strcpy(c->sides, index == 29 ? "two-sided-long-edge" :
                                      "two-sided-short-edge");
    } else {
        /* Explicit non-zero imageable margins: catches the PostScript
         * placement/margin class of regression without multiplying every
         * engine's matrix. 4.40 mm is representative of real queried data. */
        strcpy(c->engine, "postscript");
        strcpy(c->scaling, "fit");
        c->margin_100mm = 440;
    }
}

static BOOL mp_test_suite_make_drawer(char *out, size_t cap)
{
    int n;
    for (n = 0; n < 100; ++n) {
        BPTR lock;
        if (n == 0)
            snprintf(out, cap, "T:MintPRINT-TestSuite");
        else
            snprintf(out, cap, "T:MintPRINT-TestSuite-%d", n + 1);
        lock = Lock((CONST_STRPTR)out, ACCESS_READ);
        if (lock) {
            UnLock(lock);
            continue;
        }
        lock = CreateDir((CONST_STRPTR)out);
        if (lock) {
            UnLock(lock);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL mp_test_suite_append(const char *text)
{
    char path[MP_TEST_SUITE_PATH_MAX];
    BPTR fh;
    snprintf(path, sizeof(path), "%s/manifest.txt", g_test_suite.drawer);
    fh = Open((CONST_STRPTR)path, MODE_READWRITE);
    if (!fh) fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!fh) return FALSE;
    Seek(fh, 0, OFFSET_END);
    FPuts(fh, (STRPTR)text);
    Close(fh);
    return TRUE;
}

static BOOL mp_test_suite_copy_file(CONST_STRPTR src, CONST_STRPTR dst)
{
    BPTR in, out;
    UBYTE buf[1024];
    LONG got;

    in = Open(src, MODE_OLDFILE);
    if (!in) return FALSE;
    out = Open(dst, MODE_NEWFILE);
    if (!out) {
        Close(in);
        return FALSE;
    }
    while ((got = Read(in, buf, sizeof(buf))) > 0) {
        if (Write(out, buf, got) != got) {
            Close(out);
            Close(in);
            return FALSE;
        }
    }
    Close(out);
    Close(in);
    return got == 0;
}

static void mp_test_suite_set_button(struct Window *win, BOOL enabled)
{
    struct Gadget *g = find_gadget_by_id(GAD_TEST_SUITE);
    if (g && win)
        GT_SetGadgetAttrs(g, win, NULL,
                          GA_Disabled, enabled ? FALSE : TRUE,
                          TAG_DONE);
}

static void mp_test_suite_snapshot(void)
{
    mp_test_suite_copy_string(g_test_suite.old_engine,
                              sizeof(g_test_suite.old_engine),
                              driver_engine_buffer);
    mp_test_suite_copy_string(g_test_suite.old_media,
                              sizeof(g_test_suite.old_media), driver_media_buffer);
    mp_test_suite_copy_string(g_test_suite.old_source,
                              sizeof(g_test_suite.old_source), driver_source_buffer);
    mp_test_suite_copy_string(g_test_suite.old_color,
                              sizeof(g_test_suite.old_color), driver_color_buffer);
    mp_test_suite_copy_string(g_test_suite.old_quality,
                              sizeof(g_test_suite.old_quality), driver_quality_buffer);
    mp_test_suite_copy_string(g_test_suite.old_scaling,
                              sizeof(g_test_suite.old_scaling), driver_scaling_buffer);
    mp_test_suite_copy_string(g_test_suite.old_sides,
                              sizeof(g_test_suite.old_sides), driver_sides_buffer);
    mp_test_suite_copy_string(g_test_suite.old_selected_quality,
                              sizeof(g_test_suite.old_selected_quality), selected_quality);
    mp_test_suite_copy_string(g_test_suite.old_selected_scaling,
                              sizeof(g_test_suite.old_selected_scaling), selected_scaling);
    mp_test_suite_copy_string(g_test_suite.old_selected_print_mode,
                              sizeof(g_test_suite.old_selected_print_mode), selected_print_mode);
    mp_test_suite_copy_string(g_test_suite.old_sheet_back,
                              sizeof(g_test_suite.old_sheet_back), pwg_sheet_back_value);
    g_test_suite.old_resolution = driver_resolution;
    g_test_suite.old_engine_explicit = driver_engine_explicit;
    g_test_suite.old_resolution_explicit = driver_resolution_explicit;
}

static void mp_test_suite_restore(struct Window *win)
{
    mp_test_suite_copy_string(driver_engine_buffer, sizeof(driver_engine_buffer),
                              g_test_suite.old_engine);
    mp_test_suite_copy_string(driver_media_buffer, sizeof(driver_media_buffer),
                              g_test_suite.old_media);
    mp_test_suite_copy_string(driver_source_buffer, sizeof(driver_source_buffer),
                              g_test_suite.old_source);
    mp_test_suite_copy_string(driver_color_buffer, sizeof(driver_color_buffer),
                              g_test_suite.old_color);
    mp_test_suite_copy_string(driver_quality_buffer, sizeof(driver_quality_buffer),
                              g_test_suite.old_quality);
    mp_test_suite_copy_string(driver_scaling_buffer, sizeof(driver_scaling_buffer),
                              g_test_suite.old_scaling);
    mp_test_suite_copy_string(driver_sides_buffer, sizeof(driver_sides_buffer),
                              g_test_suite.old_sides);
    mp_test_suite_copy_string(selected_quality, sizeof(selected_quality),
                              g_test_suite.old_selected_quality);
    mp_test_suite_copy_string(selected_scaling, sizeof(selected_scaling),
                              g_test_suite.old_selected_scaling);
    mp_test_suite_copy_string(selected_print_mode, sizeof(selected_print_mode),
                              g_test_suite.old_selected_print_mode);
    mp_test_suite_copy_string(pwg_sheet_back_value, sizeof(pwg_sheet_back_value),
                              g_test_suite.old_sheet_back);
    driver_resolution = g_test_suite.old_resolution;
    driver_engine_explicit = g_test_suite.old_engine_explicit;
    driver_resolution_explicit = g_test_suite.old_resolution_explicit;
    if (win) apply_driver_config_to_gadgets(win);
}

static void mp_test_suite_finish(struct Window *win, BOOL aborted)
{
    char line[256];
    if (!g_test_suite.active) return;
    DeleteFile((CONST_STRPTR)"T:MintPRINT-testsuite.cfg");
    snprintf(line, sizeof(line), "RESULT suite=%s completed=%d/%d\n",
             aborted ? "ABORTED" : "COMPLETE", g_test_suite.current,
             MP_TEST_SUITE_CASES);
    mp_test_suite_append(line);
    mp_test_suite_restore(win);
    g_test_suite.active = FALSE;
    mp_test_suite_set_button(win, TRUE);
    if (aborted)
        printf("Test Suite aborted; captures kept in %s\n", g_test_suite.drawer);
    else {
        printf("Test Suite complete: %d captures in %s\n",
               MP_TEST_SUITE_CASES, g_test_suite.drawer);
        printf("Upload that drawer (or archive it) for regression review.\n");
    }
}

static void mp_test_suite_apply_case(const struct MPTestSuiteCase *c)
{
    mp_test_suite_copy_string(driver_engine_buffer, sizeof(driver_engine_buffer), c->engine);
    mp_test_suite_copy_string(driver_media_buffer, sizeof(driver_media_buffer), c->media);
    mp_test_suite_copy_string(driver_source_buffer, sizeof(driver_source_buffer), c->source);
    mp_test_suite_copy_string(driver_color_buffer, sizeof(driver_color_buffer), c->color);
    mp_test_suite_copy_string(driver_quality_buffer, sizeof(driver_quality_buffer), c->quality);
    mp_test_suite_copy_string(driver_scaling_buffer, sizeof(driver_scaling_buffer), c->scaling);
    mp_test_suite_copy_string(driver_sides_buffer, sizeof(driver_sides_buffer), c->sides);
    mp_test_suite_copy_string(selected_quality, sizeof(selected_quality), c->quality);
    mp_test_suite_copy_string(selected_scaling, sizeof(selected_scaling), c->scaling);
    mp_test_suite_copy_string(selected_print_mode, sizeof(selected_print_mode), c->color);
    mp_test_suite_copy_string(pwg_sheet_back_value, sizeof(pwg_sheet_back_value), c->sheet_back);
    driver_resolution = c->resolution;
    driver_engine_explicit = TRUE;
    driver_resolution_explicit = TRUE;
}

static BOOL mp_test_suite_write_config(const struct MPTestSuiteCase *c)
{
    BPTR fh;
    char line[256];
    fh = Open((CONST_STRPTR)"T:MintPRINT-testsuite.cfg", MODE_NEWFILE);
    if (!fh) return FALSE;

    FPuts(fh, "HOST=127.0.0.1\nPORT=631\nPATH=/ipp/print\n");
    FPuts(fh, "DEBUG=1\nSPOOL=RAM\nSPOOL_KEEP=0\n");
    FPuts(fh, "CAPTURE_ONLY=1\n");
    snprintf(line, sizeof(line), "CAPTURE_PATH=%s\n", g_test_suite.output_path); FPuts(fh, line);
    snprintf(line, sizeof(line), "ENGINE=%s\n", c->engine); FPuts(fh, line);
    snprintf(line, sizeof(line), "RESOLUTION=%d\n", c->resolution); FPuts(fh, line);
    snprintf(line, sizeof(line), "MEDIA=%s\n", c->media); FPuts(fh, line);
    snprintf(line, sizeof(line), "SOURCE=%s\n", c->source); FPuts(fh, line);
    snprintf(line, sizeof(line), "COLOR=%s\n", c->color); FPuts(fh, line);
    snprintf(line, sizeof(line), "QUALITY=%s\n", c->quality); FPuts(fh, line);
    snprintf(line, sizeof(line), "SCALING=%s\n", c->scaling); FPuts(fh, line);
    snprintf(line, sizeof(line), "SIDES=%s\n", c->sides); FPuts(fh, line);
    snprintf(line, sizeof(line), "PWG_SHEET_BACK=%s\n", c->sheet_back); FPuts(fh, line);
    snprintf(line, sizeof(line), "MARGIN_LEFT=%lu\nMARGIN_RIGHT=%lu\n"
             "MARGIN_TOP=%lu\nMARGIN_BOTTOM=%lu\n",
             (unsigned long)c->margin_100mm, (unsigned long)c->margin_100mm,
             (unsigned long)c->margin_100mm, (unsigned long)c->margin_100mm);
    FPuts(fh, line);
    Close(fh);
    return TRUE;
}

static void mp_test_suite_paths(const struct MPTestSuiteCase *c)
{
    const char *short_engine = mp_test_suite_engine_short(c->engine);
    const char *ext = mp_test_suite_extension(c->engine);
    if (strcmp(c->sides, "one-sided") != 0) {
        snprintf(g_test_suite.output_path, sizeof(g_test_suite.output_path),
                 "%s/%03d-%s-%s-%s.%s", g_test_suite.drawer,
                 g_test_suite.current + 1, short_engine,
                 strstr(c->sides, "short") ? "short" : "long",
                 c->sheet_back, ext);
    } else {
        snprintf(g_test_suite.output_path, sizeof(g_test_suite.output_path),
                 "%s/%03d-%s-%s-%ddpi-%s-%s.%s", g_test_suite.drawer,
                 g_test_suite.current + 1, short_engine, c->scaling,
                 c->resolution, c->color, c->quality, ext);
    }
    snprintf(g_test_suite.log_path, sizeof(g_test_suite.log_path),
             "%s/%03d-driver.log", g_test_suite.drawer,
             g_test_suite.current + 1);
}

static BOOL mp_test_suite_capture_exists(LONG *size_out)
{
    BPTR lock;
    struct FileInfoBlock fib;
    if (size_out) *size_out = 0;
    lock = Lock((CONST_STRPTR)g_test_suite.output_path, ACCESS_READ);
    if (!lock) return FALSE;
    memset(&fib, 0, sizeof(fib));
    if (Examine(lock, &fib) && size_out) *size_out = fib.fib_Size;
    UnLock(lock);
    return TRUE;
}

static void mp_test_suite_run_current(struct Window *win)
{
    while (g_test_suite.active && g_test_suite.current < MP_TEST_SUITE_CASES) {
        struct MPTestSuiteCase c;
        char line[512];

        mp_test_suite_case(g_test_suite.current, &c);
        mp_test_suite_paths(&c);
        mp_test_suite_apply_case(&c);
        DeleteFile((CONST_STRPTR)"T:MintPRINT-driver.log");

        snprintf(line, sizeof(line),
                 "CASE %03d file=%s engine=%s dpi=%d media=%s source=%s "
                 "color=%s quality=%s scaling=%s sides=%s sheet-back=%s "
                 "margin100mm=%lu\n",
                 g_test_suite.current + 1, g_test_suite.output_path,
                 c.engine, c.resolution, c.media, c.source, c.color,
                 c.quality, c.scaling, c.sides, c.sheet_back,
                 (unsigned long)c.margin_100mm);
        mp_test_suite_append(line);

        if (!mp_test_suite_write_config(&c)) {
            mp_test_suite_append("ERROR could-not-write-test-config\n");
            ++g_test_suite.current;
            continue;
        }

        printf("Test Suite %d/%d: %s %s %ddpi\n",
               g_test_suite.current + 1, MP_TEST_SUITE_CASES,
               c.engine, c.scaling, c.resolution);
        if (mintprint_test_page(win)) return;

        DeleteFile((CONST_STRPTR)"T:MintPRINT-testsuite.cfg");
        mp_test_suite_append("ERROR test-print-could-not-start\n");
        ++g_test_suite.current;
    }

    if (g_test_suite.active)
        mp_test_suite_finish(win, FALSE);
}

static void mp_test_suite_advance(struct Window *win)
{
    LONG bytes = 0;
    char line[256];

    if (!g_test_suite.active) return;

    mp_test_suite_copy_file((CONST_STRPTR)"T:MintPRINT-driver.log",
                            (CONST_STRPTR)g_test_suite.log_path);
    if (mp_test_suite_capture_exists(&bytes) && bytes > 0) {
        snprintf(line, sizeof(line), "CASE-RESULT %03d OK bytes=%ld log=%s\n",
                 g_test_suite.current + 1, bytes, g_test_suite.log_path);
    } else {
        snprintf(line, sizeof(line), "CASE-RESULT %03d ERROR missing-or-empty-output log=%s\n",
                 g_test_suite.current + 1, g_test_suite.log_path);
    }
    mp_test_suite_append(line);
    ++g_test_suite.current;
    mp_test_suite_run_current(win);
}

static void mp_test_suite_start(struct Window *win)
{
    struct MPDriverVersion ver = {0, 0};
    struct EasyStruct es;
    char manifest[512];

    if (g_test_suite.active) {
        printf("Test Suite is already running.\n");
        return;
    }
    if (test_print_job.active) {
        printf("Wait for the current Test Print to finish first.\n");
        return;
    }

    /* Hard safety gate: 41.14 and older do not understand CAPTURE_ONLY and
     * would treat the temporary file as an ordinary printer configuration. */
    if (!mp_read_driver_version(MINTPRINT_DRIVER_DEST, &ver) ||
        ver.version < 41 || (ver.version == 41 && ver.revision < 15)) {
        printf("Test Suite requires installed MintPRINT driver 41.15 or newer.\n");
        printf("Update DEVS:Printers/MintPRINT from this build before running it.\n");
        return;
    }

    es.es_StructSize = sizeof(struct EasyStruct);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)"MintPRINT Output Test Suite";
    es.es_TextFormat = (UBYTE *)
        "Run 32 capture-only regression jobs?\n\n"
        "No job is submitted to the printer. Output documents and one\n"
        "driver log per case are retained under a new T: drawer.\n\n"
        "T: is usually RAM:, so allow sufficient free memory.";
    es.es_GadgetFormat = (UBYTE *)"Run Suite|Cancel";
    if (!EasyRequest(win, &es, NULL)) return;

    memset(&g_test_suite, 0, sizeof(g_test_suite));
    if (!mp_test_suite_make_drawer(g_test_suite.drawer,
                                   sizeof(g_test_suite.drawer))) {
        printf("Test Suite: could not create a capture drawer in T:.\n");
        return;
    }

    mp_test_suite_snapshot();
    g_test_suite.active = TRUE;
    g_test_suite.current = 0;
    mp_test_suite_set_button(win, FALSE);

    snprintf(manifest, sizeof(manifest),
             "MintPRINT output regression suite\n"
             "driver=%u.%u cases=%d capture-only=1 network-submit=disabled\n"
             "matrix=5 engines x 5 scaling modes plus PWG/URF duplex and PostScript margin coverage\n"
             "drawer=%s\n\n",
             (unsigned)ver.version, (unsigned)ver.revision,
             MP_TEST_SUITE_CASES, g_test_suite.drawer);
    mp_test_suite_append(manifest);
    printf("Test Suite capture drawer: %s\n", g_test_suite.drawer);
    mp_test_suite_run_current(win);
}
'''

replace_once(
    "src/MintPrintSettings.c",
    "    SendIO((struct IORequest *)test_print_job.request);\n"
    "    return TRUE;\n"
    "}\n\n"
    "static void apply_driver_config_to_gadgets(struct Window *win) {",
    "    SendIO((struct IORequest *)test_print_job.request);\n"
    "    return TRUE;\n"
    "}\n" + suite_code + "\n\n"
    "static void apply_driver_config_to_gadgets(struct Window *win) {",
    "insert suite runner",
)

# Button row: compact five controls into the same 520px window width.
replace_once(
    "src/MintPrintSettings.c",
    "    ng.ng_LeftEdge = 10;\n"
    "    ng.ng_TopEdge = 198 + topborder;\n"
    "    ng.ng_Width = 110;\n"
    "    ng.ng_Height = 12;\n"
    "    ng.ng_GadgetText = (STRPTR)\"_Test Print\";\n"
    "    ng.ng_GadgetID = GAD_PRINT_BUTTON;\n"
    "    gad = CreateGadget(BUTTON_KIND, gad, &ng,\n"
    "        GT_Underscore, '_',\n"
    "        TAG_DONE);\n"
    "    if (!gad) {\n"
    "        printf(\"Failed to create print button\\n\");\n"
    "        return NULL;\n"
    "    }\n\n"
    "    // Enable/disable diagnostic logs and retained rendered jobs. Shares\n",
    "    ng.ng_LeftEdge = 10;\n"
    "    ng.ng_TopEdge = 198 + topborder;\n"
    "    ng.ng_Width = 90;\n"
    "    ng.ng_Height = 12;\n"
    "    ng.ng_GadgetText = (STRPTR)\"_Test Print\";\n"
    "    ng.ng_GadgetID = GAD_PRINT_BUTTON;\n"
    "    gad = CreateGadget(BUTTON_KIND, gad, &ng,\n"
    "        GT_Underscore, '_',\n"
    "        TAG_DONE);\n"
    "    if (!gad) {\n"
    "        printf(\"Failed to create print button\\n\");\n"
    "        return NULL;\n"
    "    }\n\n"
    "    /* Developer regression capture. Stays on the existing button row\n"
    "     * so the OS 2.x-safe window geometry does not grow again. */\n"
    "    ng.ng_LeftEdge = 105;\n"
    "    ng.ng_Width = 90;\n"
    "    ng.ng_GadgetText = (STRPTR)\"Test _Suite\";\n"
    "    ng.ng_GadgetID = GAD_TEST_SUITE;\n"
    "    gad = CreateGadget(BUTTON_KIND, gad, &ng,\n"
    "        GT_Underscore, '_',\n"
    "        TAG_DONE);\n"
    "    if (!gad) {\n"
    "        printf(\"Failed to create test suite button\\n\");\n"
    "        return NULL;\n"
    "    }\n\n"
    "    // Enable/disable diagnostic logs and retained rendered jobs. Shares\n",
    "add suite button",
)

replace_once(
    "src/MintPrintSettings.c",
    "    ng.ng_LeftEdge = 160;\n"
    "    ng.ng_TopEdge = 198 + topborder;\n"
    "    ng.ng_Width = 110;\n",
    "    ng.ng_LeftEdge = 200;\n"
    "    ng.ng_TopEdge = 198 + topborder;\n"
    "    ng.ng_Width = 90;\n",
    "compact debug",
)
replace_once(
    "src/MintPrintSettings.c",
    "    ng.ng_LeftEdge = 304;\n"
    "    ng.ng_Width = 90;\n",
    "    ng.ng_LeftEdge = 300;\n"
    "    ng.ng_Width = 90;\n",
    "compact save",
)
replace_once(
    "src/MintPrintSettings.c",
    "    ng.ng_LeftEdge = 408;\n"
    "    ng.ng_Width = 90;\n",
    "    ng.ng_LeftEdge = 400;\n"
    "    ng.ng_Width = 90;\n",
    "compact exit",
)

# Advance the suite only after CloseDevice has completed the previous capture.
replace_once(
    "src/MintPrintSettings.c",
    "        if (test_print_job.active && test_print_job.port &&\n"
    "            (received_signals & (1L << test_print_job.port->mp_SigBit))) {\n"
    "            mp_test_print_complete(win);\n"
    "        }\n",
    "        if (test_print_job.active && test_print_job.port &&\n"
    "            (received_signals & (1L << test_print_job.port->mp_SigBit))) {\n"
    "            mp_test_print_complete(win);\n"
    "            if (g_test_suite.active) mp_test_suite_advance(win);\n"
    "        }\n",
    "suite async advance",
)

replace_once(
    "src/MintPrintSettings.c",
    "                        case GAD_PRINT_BUTTON:\n"
    "                        {\n"
    "                            GT_RefreshWindow(win, NULL);\n"
    "                            mintprint_test_page(win);\n"
    "                        }\n"
    "                        break;\n\n"
    "                        case GAD_SAVE_BUTTON:",
    "                        case GAD_PRINT_BUTTON:\n"
    "                        {\n"
    "                            GT_RefreshWindow(win, NULL);\n"
    "                            mintprint_test_page(win);\n"
    "                        }\n"
    "                        break;\n\n"
    "                        case GAD_TEST_SUITE:\n"
    "                            mp_test_suite_start(win);\n"
    "                            break;\n\n"
    "                        case GAD_SAVE_BUTTON:",
    "suite event",
)

replace_once(
    "src/MintPrintSettings.c",
    "    if (test_print_job.active)\n"
    "        mp_test_print_cancel(win);\n\n"
    "    /* The main window is closing",
    "    if (test_print_job.active)\n"
    "        mp_test_print_cancel(win);\n"
    "    if (g_test_suite.active)\n"
    "        mp_test_suite_finish(win, TRUE);\n\n"
    "    /* The main window is closing",
    "suite close cleanup",
)


# ---------------------------------------------------------------------------
# Documentation / changelog.
# ---------------------------------------------------------------------------
replace_once(
    "docs/MINTPRINT_PREFS.md",
    "## Make and model\n",
    "## Output regression Test Suite\n\n"
    "MintPrint Settings includes a **Test Suite** button for developer/regression\n"
    "testing. It requires the installed `DEVS:Printers/MintPRINT` driver to be\n"
    "**41.15 or newer**; Settings refuses to start the suite with an older driver\n"
    "because older builds do not understand capture-only mode.\n\n"
    "The suite creates a fresh `T:MintPRINT-TestSuite` drawer (or `-2`, `-3`, ...\n"
    "if one already exists) and runs 32 normal `printer.device` Test Print\n"
    "renders. **No IPP Print-Job is submitted to the printer.** Driver 41.15's\n"
    "temporary capture-only config retains each JPEG, PWG Raster, PDF, PostScript\n"
    "or Apple Raster document in that drawer instead. A matching driver log and\n"
    "`manifest.txt` describe the exact engine, resolution, media, colour, quality,\n"
    "scaling, sides, sheet-back transform and margin settings used for every case.\n\n"
    "This is a bounded coverage matrix rather than the Cartesian product of every\n"
    "setting: all five engines are exercised with all five scaling modes while\n"
    "300/600 DPI, colour/monochrome, draft/normal/high quality and A4/Letter are\n"
    "crossed through those cases; PWG/URF receive additional duplex cases and\n"
    "PostScript receives an explicit non-zero-margin case. This keeps the result\n"
    "set practical for `T:`, which is normally RAM:, while still touching every\n"
    "important output path. The suite never modifies saved Unit0 settings and\n"
    "restores the live Settings controls when it finishes or is aborted.\n\n"
    "After the run, archive/upload the complete TestSuite drawer when reporting a\n"
    "rendering regression; the manifest makes each binary output reproducible.\n\n"
    "## Make and model\n",
    "prefs suite docs",
)

replace_once(
    "CHANGELOG.md",
    "# Changelog\n",
    "# Changelog\n\n"
    "- **Capture-only output regression suite (driver 41.15).** MintPrint Settings\n"
    "  can now run a 32-case coverage matrix through all five document engines and\n"
    "  retain the generated jobs plus per-case logs/manifest under `T:` without\n"
    "  making any IPP submission. The suite is hard-gated on driver 41.15+ so an\n"
    "  older driver can never accidentally turn the regression run into real print\n"
    "  jobs.\n",
    "changelog suite entry",
)

print("output regression suite patch applied")
