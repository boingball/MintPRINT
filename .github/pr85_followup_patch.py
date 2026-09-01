from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    s = p.read_text()
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"{path}: expected one anchor, found {n}: {old[:80]!r}")
    p.write_text(s.replace(old, new, 1))


def replace_all(path, old, new, expected):
    p = Path(path)
    s = p.read_text()
    n = s.count(old)
    if n != expected:
        raise SystemExit(f"{path}: expected {expected} anchors, found {n}: {old[:80]!r}")
    p.write_text(s.replace(old, new))


# ---------------------------------------------------------------------------
# JPEG: preserve the old 300-dpi API, but add an explicit-density entry point
# and use it everywhere the caller already knows the real capture DPI.
# ---------------------------------------------------------------------------
replace_once(
    "driver/jpeg_writer.h",
    "    unsigned long height;\n    unsigned long rows_in;",
    "    unsigned long height;\n    unsigned long dpi;\n    unsigned long rows_in;"
)
replace_once(
    "driver/jpeg_writer.h",
    "unsigned long mp_jpeg_scratch_size(unsigned long width);\nint mp_jpeg_begin(MPJpegEncoder *enc, unsigned long width, unsigned long height,\n",
    "unsigned long mp_jpeg_scratch_size(unsigned long width);\nint mp_jpeg_begin_dpi(MPJpegEncoder *enc, unsigned long width, unsigned long height,\n                      unsigned long dpi,\n                      unsigned char *scratch, unsigned long scratch_size,\n                      MPJpegWriteFn write_fn, void *write_ctx);\nint mp_jpeg_begin(MPJpegEncoder *enc, unsigned long width, unsigned long height,\n"
)

replace_once(
    "driver/jpeg_writer.c",
    "static int mp_write_headers(MPJpegEncoder *e)\n{\n    int i, t;\n    static const unsigned char jfif[14] = {\n        'J','F','I','F',0, 1,1, 1, 1,44, 1,44, 0,0\n    };\n\n    if (!mp_put_marker(e, 0xd8)) return 0;",
    "static int mp_write_headers(MPJpegEncoder *e)\n{\n    int i, t;\n    unsigned int density = (unsigned int)(e->dpi ? e->dpi : 300UL);\n    unsigned char jfif[14] = {\n        'J','F','I','F',0, 1,1, 1, 0,0, 0,0, 0,0\n    };\n\n    /* JFIF density is an unsigned 16-bit pixels-per-inch value. The raw\n     * JPEG engine used to hard-code 300 here even when the selected raster\n     * was 600 dpi, so a consumer using physical JPEG density rather than\n     * only IPP media/scaling could interpret the page at twice its intended\n     * size. Keep the metadata tied to the actual capture resolution. */\n    if (density == 0U) density = 300U;\n    if (density > 65535U) density = 65535U;\n    jfif[8] = (unsigned char)(density >> 8);\n    jfif[9] = (unsigned char)density;\n    jfif[10] = (unsigned char)(density >> 8);\n    jfif[11] = (unsigned char)density;\n\n    if (!mp_put_marker(e, 0xd8)) return 0;"
)
replace_once(
    "driver/jpeg_writer.c",
    "int mp_jpeg_begin(MPJpegEncoder *e, unsigned long width, unsigned long height,\n                  unsigned char *scratch, unsigned long scratch_size,\n                  MPJpegWriteFn write_fn, void *write_ctx)\n{",
    "int mp_jpeg_begin_dpi(MPJpegEncoder *e, unsigned long width, unsigned long height,\n                      unsigned long dpi,\n                      unsigned char *scratch, unsigned long scratch_size,\n                      MPJpegWriteFn write_fn, void *write_ctx)\n{"
)
replace_once(
    "driver/jpeg_writer.c",
    "    e->width = width;\n    e->height = height;\n    e->rows_in = 0;",
    "    e->width = width;\n    e->height = height;\n    e->dpi = dpi ? dpi : 300UL;\n    e->rows_in = 0;"
)
# Insert the compatibility wrapper immediately before write_scanline.
replace_once(
    "driver/jpeg_writer.c",
    "    return mp_write_headers(e);\n}\n\nint mp_jpeg_write_scanline(MPJpegEncoder *e, const unsigned char *rgb)",
    "    return mp_write_headers(e);\n}\n\nint mp_jpeg_begin(MPJpegEncoder *e, unsigned long width, unsigned long height,\n                  unsigned char *scratch, unsigned long scratch_size,\n                  MPJpegWriteFn write_fn, void *write_ctx)\n{\n    return mp_jpeg_begin_dpi(e, width, height, 300UL, scratch, scratch_size,\n                             write_fn, write_ctx);\n}\n\nint mp_jpeg_write_scanline(MPJpegEncoder *e, const unsigned char *rgb)"
)

replace_once(
    "driver/pdf_writer.c",
    "    if (!mp_jpeg_begin(&e->jpeg, width, height, scratch, scratch_size,\n                       mp_pdf_jpeg_write_fn, e)) {",
    "    if (!mp_jpeg_begin_dpi(&e->jpeg, width, height, dpi,\n                           scratch, scratch_size,\n                           mp_pdf_jpeg_write_fn, e)) {"
)
replace_once(
    "driver/postscript_writer.c",
    "    if (!mp_jpeg_begin(&e->jpeg, width, height, scratch, scratch_size,\n                       mp_ps_jpeg_write_fn, e)) {",
    "    if (!mp_jpeg_begin_dpi(&e->jpeg, width, height, dpi,\n                           scratch, scratch_size,\n                           mp_ps_jpeg_write_fn, e)) {"
)
replace_once(
    "driver/command_table.c",
    "            return mp_jpeg_begin(&g_text_jpeg, width, height,\n                                 scratch, scratch_bytes,\n                                 mp_text_file_write, NULL) ? TRUE : FALSE;",
    "            return mp_jpeg_begin_dpi(&g_text_jpeg, width, height, dpi,\n                                     scratch, scratch_bytes,\n                                     mp_text_file_write, NULL) ? TRUE : FALSE;"
)
replace_once(
    "driver/driver_core.c",
    "            if (!mp_jpeg_begin(&g_jpeg, width, height, g_jpeg_scratch,\n                               g_jpeg_scratch_bytes, mp_job_file_write, NULL)) {",
    "            if (!mp_jpeg_begin_dpi(&g_jpeg, width, height,\n                                   g_config.resolution, g_jpeg_scratch,\n                                   g_jpeg_scratch_bytes,\n                                   mp_job_file_write, NULL)) {"
)
replace_once(
    "driver/driver_core.c",
    "            if (!mp_jpeg_begin(&g_jpeg, width, final_height, g_jpeg_scratch,\n                               g_jpeg_scratch_bytes, mp_job_file_write, NULL)) {",
    "            if (!mp_jpeg_begin_dpi(&g_jpeg, width, final_height,\n                                   g_config.resolution, g_jpeg_scratch,\n                                   g_jpeg_scratch_bytes,\n                                   mp_job_file_write, NULL)) {"
)

# JPEG host regression: retain the first header bytes and assert 600 dpi is
# encoded as 0x0258 in both JFIF density fields.
replace_once(
    "tests/test_jpeg_writer.c",
    "struct TestSink {\n    unsigned long bytes;\n    unsigned long calls;\n};",
    "struct TestSink {\n    unsigned long bytes;\n    unsigned long calls;\n    unsigned char header[32];\n    unsigned long header_used;\n};"
)
replace_once(
    "tests/test_jpeg_writer.c",
    "    (void)data;\n    sink->bytes += len;\n    ++sink->calls;\n    return (long)len;",
    "    if (sink->header_used < sizeof(sink->header)) {\n        unsigned long room = sizeof(sink->header) - sink->header_used;\n        unsigned long copy = len < room ? len : room;\n        memcpy(sink->header + sink->header_used, data, copy);\n        sink->header_used += copy;\n    }\n    sink->bytes += len;\n    ++sink->calls;\n    return (long)len;"
)
replace_all(
    "tests/test_jpeg_writer.c",
    "    sink.calls = 0;\n",
    "    sink.calls = 0;\n    sink.header_used = 0;\n",
    2
)
insert_test = r'''
static void test_jfif_density(void)
{
    MPJpegEncoder enc;
    struct TestSink sink;
    unsigned char scratch[16 * 16 * 3];

    memset(&sink, 0, sizeof(sink));
    assert(mp_jpeg_begin_dpi(&enc, 16, 16, 600UL,
                             scratch, sizeof(scratch),
                             test_sink_write, &sink));
    /* SOI(2) + APP0 marker(2) + length(2) + JFIF data. Units byte is file
     * offset 13; X/Y density are offsets 14..17. */
    assert(sink.header_used >= 18UL);
    assert(sink.header[13] == 1); /* pixels per inch */
    assert(sink.header[14] == 0x02 && sink.header[15] == 0x58);
    assert(sink.header[16] == 0x02 && sink.header[17] == 0x58);
}

'''
replace_once(
    "tests/test_jpeg_writer.c",
    "static void test_flat_encoder_fast_path(void)\n{",
    insert_test + "static void test_flat_encoder_fast_path(void)\n{"
)
replace_once(
    "tests/test_jpeg_writer.c",
    "    test_flat_encoder_fast_path();\n    test_nonflat_encoder_still_uses_dct();",
    "    test_jfif_density();\n    test_flat_encoder_fast_path();\n    test_nonflat_encoder_still_uses_dct();"
)

# ---------------------------------------------------------------------------
# IPP sidecar: capture the exact IPP Print-Job operation bytes that would
# precede the document, but never open a socket. This uses the same private
# builders as the real submit path so media-col/enums/keywords are byte-level
# inspectable in the uploaded suite drawer.
# ---------------------------------------------------------------------------
replace_once(
    "driver/ipp_client.h",
    "LONG mp_ipp_print_document(const struct MPConfig *cfg, CONST_STRPTR filename,\n                           CONST_STRPTR document_format,\n                           struct MPIPPResult *result);",
    "/* Capture the exact IPP Print-Job operation body (attributes through\n * end-of-attributes, excluding document bytes) next to document_filename as\n * <document_filename>.ipp. No socket is opened. Used by the regression suite. */\nLONG mp_ipp_capture_request(const struct MPConfig *cfg,\n                            CONST_STRPTR document_format,\n                            CONST_STRPTR document_filename);\n\nLONG mp_ipp_print_document(const struct MPConfig *cfg, CONST_STRPTR filename,\n                           CONST_STRPTR document_format,\n                           struct MPIPPResult *result);"
)

ipp_capture = r'''
LONG mp_ipp_capture_request(const struct MPConfig *cfg,
                            CONST_STRPTR document_format,
                            CONST_STRPTR document_filename)
{
    static UBYTE ipp[1024];
    static char uri[192];
    static char output_path[MP_CONFIG_CAPTURE_PATH_MAX + 8];
    ULONG io = 0;
    ULONG up = 0;
    ULONG op = 0;
    BPTR fh;

    if (!DOSBase || !cfg || !cfg->host[0] || cfg->port == 0 ||
        cfg->path[0] != '/' || !document_format || !document_filename)
        return -1;

    uri[0] = 0;
    if (!mp_append(uri, sizeof(uri), &up, "ipp://") ||
        !mp_append(uri, sizeof(uri), &up, cfg->host))
        return -2;
    if (cfg->port != 631 &&
        (!mp_append(uri, sizeof(uri), &up, ":") ||
         !mp_append_ulong(uri, sizeof(uri), &up, cfg->port)))
        return -2;
    if (!mp_append(uri, sizeof(uri), &up, cfg->path))
        return -2;

    if (!mp_put8(ipp, sizeof(ipp), &io, 1) ||
        !mp_put8(ipp, sizeof(ipp), &io, 1) ||
        !mp_put16(ipp, sizeof(ipp), &io, 0x0002) ||
        !mp_put32(ipp, sizeof(ipp), &io, 1) ||
        !mp_put8(ipp, sizeof(ipp), &io, 0x01) ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x47,
                     "attributes-charset", "utf-8") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x48,
                     "attributes-natural-language", "en") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x45, "printer-uri", uri) ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x42,
                     "requesting-user-name", "Amiga") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x42,
                     "job-name", "MintPRINT AmigaOS") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x49,
                     "document-format", (const char *)document_format))
        return -3;

    if (cfg->media[0] || cfg->source[0] || cfg->color[0] || cfg->quality[0] ||
        cfg->scaling[0] || cfg->sides[0]) {
        ULONG quality_enum = mp_quality_enum(cfg->quality);
        ULONG media_x = 0, media_y = 0;
        int use_media_col = cfg->media[0] && cfg->source[0] &&
                            mp_media_dimensions_100mm(cfg->media,
                                                      &media_x, &media_y);

        if (!mp_put8(ipp, sizeof(ipp), &io, 0x02)) return -3;
        if (use_media_col) {
            if (!mp_media_col_attr(ipp, sizeof(ipp), &io, media_x, media_y,
                                   cfg->source)) return -3;
        } else if (cfg->media[0] &&
                   !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44,
                                "media", cfg->media)) return -3;
        if (cfg->color[0] &&
            !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44,
                         "print-color-mode", cfg->color)) return -3;
        if (quality_enum &&
            !mp_ipp_enum_attr(ipp, sizeof(ipp), &io,
                              "print-quality", quality_enum)) return -3;
        if (cfg->scaling[0] &&
            !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44,
                         "print-scaling", cfg->scaling)) return -3;
        if (cfg->sides[0] &&
            !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44,
                         "sides", cfg->sides)) return -3;
    }
    if (!mp_put8(ipp, sizeof(ipp), &io, 0x03)) return -3;

    output_path[0] = 0;
    if (!mp_append(output_path, sizeof(output_path), &op,
                   (const char *)document_filename) ||
        !mp_append(output_path, sizeof(output_path), &op, ".ipp"))
        return -4;

    fh = Open((CONST_STRPTR)output_path, MODE_NEWFILE);
    if (!fh) return -5;
    if (Write(fh, ipp, (LONG)io) != (LONG)io) {
        Close(fh);
        return -6;
    }
    Close(fh);
    return 0;
}

'''
replace_once(
    "driver/ipp_client.c",
    "LONG mp_ipp_print_document(const struct MPConfig *cfg, CONST_STRPTR filename,\n",
    ipp_capture + "LONG mp_ipp_print_document(const struct MPConfig *cfg, CONST_STRPTR filename,\n"
)

# Driver capture branches now require the sidecar to be successfully written.
replace_once(
    "driver/driver_core.c",
    "#define MP_DRIVER_SUBREV 15",
    "#define MP_DRIVER_SUBREV 16"
)
replace_once(
    "driver/driver_core.c",
    "    } else if (g_config.capture_only) {\n        /* Regression suite: the rendered document itself is the result.\n         * Never open a TCP connection, and report a synthetic success so\n         * normal page bookkeeping can finish. */\n        ipp_rc = 0;\n        result.error = 0;\n        result.http_status = 0;\n        result.ipp_status = 0;\n        result.document_bytes = g_job_file_bytes;\n        mp_log_text(\"Capture-only regression job retained; network submission skipped\");",
    "    } else if (g_config.capture_only) {\n        /* Regression suite: retain both the rendered document and the exact\n         * IPP operation bytes that the normal submit path would prepend.\n         * mp_ipp_capture_request() is file-only and never opens a socket. */\n        ipp_rc = mp_ipp_capture_request(&g_config, fmt, fname);\n        result.error = ipp_rc;\n        result.http_status = 0;\n        result.ipp_status = 0;\n        result.document_bytes = g_job_file_bytes;\n        if (ipp_rc == 0)\n            mp_log_text(\"Capture-only document + IPP request retained; network submission skipped\");\n        else\n            mp_log_3(\"Capture IPP request sidecar failed rc/zero/zero\", ipp_rc, 0, 0);"
)
replace_once(
    "driver/driver_core.c",
    "            if (g_config.capture_only) {\n                ipp_rc = 0;\n                result.error = 0;\n                result.http_status = 0;\n                result.ipp_status = 0;\n                result.document_bytes = g_job_file_bytes;\n                mp_log_text(\"Capture-only duplex job retained; network submission skipped\");\n                mp_log_ipp_result(\"Capture duplex result error/http/status\",\n                                  &result);",
    "            if (g_config.capture_only) {\n                ipp_rc = mp_ipp_capture_request(&g_config,\n                                                mp_document_format(),\n                                                mp_job_filename());\n                result.error = ipp_rc;\n                result.http_status = 0;\n                result.ipp_status = 0;\n                result.document_bytes = g_job_file_bytes;\n                if (ipp_rc == 0)\n                    mp_log_text(\"Capture-only duplex document + IPP request retained; network submission skipped\");\n                else\n                    mp_log_3(\"Capture duplex IPP sidecar failed rc/zero/zero\",\n                             ipp_rc, 0, 0);\n                mp_log_ipp_result(\"Capture duplex result error/http/status\",\n                                  &result);"
)

# ---------------------------------------------------------------------------
# Settings suite: 33 cases, full transform-sign coverage, .ipp verification,
# and capture-specific completion wording.
# ---------------------------------------------------------------------------
replace_once(
    "src/MintPrintSettings.c",
    "static BOOL mp_test_print_skip_config_save = FALSE;",
    "static BOOL mp_test_print_skip_config_save = FALSE;\nstatic BOOL mp_test_suite_capture_mode = FALSE;"
)
replace_once(
    "src/MintPrintSettings.c",
    "    } else {\n        /* A real OS 2.04 capture returned no I/O error despite the driver\n         * logging an IPP connection timeout during Render(4). A completed\n         * raster request therefore cannot confirm delivery, and even IPP\n         * acceptance would not prove that paper has physically printed. */\n        printf(\"Test Print request finished; delivery is not confirmed\\n\");\n        printf(\"Check printer output, retained job status or Debug driver log\\n\");\n    }",
    "    } else if (mp_test_suite_capture_mode) {\n        printf(\"Test Suite capture complete; no printer job was sent\\n\");\n    } else {\n        /* A real OS 2.04 capture returned no I/O error despite the driver\n         * logging an IPP connection timeout during Render(4). A completed\n         * raster request therefore cannot confirm delivery, and even IPP\n         * acceptance would not prove that paper has physically printed. */\n        printf(\"Test Print request finished; delivery is not confirmed\\n\");\n        printf(\"Check printer output, retained job status or Debug driver log\\n\");\n    }"
)
replace_once(
    "src/MintPrintSettings.c",
    " * matrix and asks driver 41.15+ to retain each finished document under T:",
    " * matrix and asks driver 41.16+ to retain each finished document under T:"
)
replace_once(
    "src/MintPrintSettings.c",
    "#define MP_TEST_SUITE_CASES 32",
    "#define MP_TEST_SUITE_CASES 33"
)
replace_once(
    "src/MintPrintSettings.c",
    "    char log_path[MP_TEST_SUITE_PATH_MAX];",
    "    char log_path[MP_TEST_SUITE_PATH_MAX];\n    char ipp_path[MP_TEST_SUITE_PATH_MAX + 8];"
)
old_cases = r'''    if (index < 29) {
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
'''
new_cases = r'''    if (index < 30) {
        /* Five PWG cases cover every sheet-back keyword AND every actual
         * CrossFeed/Feed sign pair: +/+, -/-, +/- and -/+. The old four-case
         * alternation accidentally paired rotated with short-edge, where
         * rotated is intentionally a no-op, so -/+ was never exercised. */
        static const char *backs[5] = {
            "normal", "rotated", "flipped", "flipped", "manual-tumble"
        };
        static const char *sides[5] = {
            "two-sided-long-edge",  /* normal:        +/+ */
            "two-sided-long-edge",  /* rotated:       -/- */
            "two-sided-long-edge",  /* flipped:       +/- */
            "two-sided-short-edge", /* flipped:       -/+ */
            "two-sided-short-edge"  /* manual-tumble: -/- */
        };
        int v = index - 25;
        strcpy(c->engine, "pwg-raster");
        strcpy(c->sides, sides[v]);
        strcpy(c->sheet_back, backs[v]);
    } else if (index < 32) {
        strcpy(c->engine, "urf");
        strcpy(c->sides, index == 30 ? "two-sided-long-edge" :
                                      "two-sided-short-edge");
    } else {
'''
replace_once("src/MintPrintSettings.c", old_cases, new_cases)
replace_once(
    "src/MintPrintSettings.c",
    "    snprintf(g_test_suite.log_path, sizeof(g_test_suite.log_path),\n             \"%s/%03d-driver.log\", g_test_suite.drawer,\n             g_test_suite.current + 1);",
    "    snprintf(g_test_suite.log_path, sizeof(g_test_suite.log_path),\n             \"%s/%03d-driver.log\", g_test_suite.drawer,\n             g_test_suite.current + 1);\n    snprintf(g_test_suite.ipp_path, sizeof(g_test_suite.ipp_path),\n             \"%s.ipp\", g_test_suite.output_path);"
)
replace_once(
    "src/MintPrintSettings.c",
    "static BOOL mp_test_suite_capture_exists(LONG *size_out)\n{\n    BPTR lock;\n    struct FileInfoBlock fib;\n    if (size_out) *size_out = 0;\n    lock = Lock((CONST_STRPTR)g_test_suite.output_path, ACCESS_READ);",
    "static BOOL mp_test_suite_file_exists(CONST_STRPTR path, LONG *size_out)\n{\n    BPTR lock;\n    struct FileInfoBlock fib;\n    if (size_out) *size_out = 0;\n    lock = Lock(path, ACCESS_READ);"
)
replace_once(
    "src/MintPrintSettings.c",
    "                 \"CASE %03d file=%s engine=%s dpi=%d media=%s source=%s \"\n                 \"color=%s quality=%s scaling=%s sides=%s sheet-back=%s \"\n                 \"margin100mm=%lu\\n\",\n                 g_test_suite.current + 1, g_test_suite.output_path,",
    "                 \"CASE %03d file=%s ipp=%s engine=%s dpi=%d media=%s source=%s \"\n                 \"color=%s quality=%s scaling=%s sides=%s sheet-back=%s \"\n                 \"margin100mm=%lu\\n\",\n                 g_test_suite.current + 1, g_test_suite.output_path,\n                 g_test_suite.ipp_path,"
)
replace_once(
    "src/MintPrintSettings.c",
    "static void mp_test_suite_advance(struct Window *win)\n{\n    LONG bytes = 0;\n    char line[256];",
    "static void mp_test_suite_advance(struct Window *win)\n{\n    LONG bytes = 0;\n    LONG ipp_bytes = 0;\n    BOOL output_ok;\n    BOOL ipp_ok;\n    char line[320];"
)
replace_once(
    "src/MintPrintSettings.c",
    "    if (mp_test_suite_capture_exists(&bytes) && bytes > 0) {\n        snprintf(line, sizeof(line), \"CASE-RESULT %03d OK bytes=%ld log=%s\\n\",\n                 g_test_suite.current + 1, bytes, g_test_suite.log_path);\n    } else {\n        snprintf(line, sizeof(line), \"CASE-RESULT %03d ERROR missing-or-empty-output log=%s\\n\",\n                 g_test_suite.current + 1, g_test_suite.log_path);\n    }",
    "    output_ok = mp_test_suite_file_exists(\n                    (CONST_STRPTR)g_test_suite.output_path, &bytes) && bytes > 0;\n    ipp_ok = mp_test_suite_file_exists(\n                 (CONST_STRPTR)g_test_suite.ipp_path, &ipp_bytes) && ipp_bytes > 0;\n    if (output_ok && ipp_ok) {\n        snprintf(line, sizeof(line),\n                 \"CASE-RESULT %03d OK bytes=%ld ipp-bytes=%ld log=%s\\n\",\n                 g_test_suite.current + 1, bytes, ipp_bytes,\n                 g_test_suite.log_path);\n    } else {\n        snprintf(line, sizeof(line),\n                 \"CASE-RESULT %03d ERROR output=%s ipp=%s log=%s\\n\",\n                 g_test_suite.current + 1,\n                 output_ok ? \"ok\" : \"missing-or-empty\",\n                 ipp_ok ? \"ok\" : \"missing-or-empty\",\n                 g_test_suite.log_path);\n    }"
)
replace_once(
    "src/MintPrintSettings.c",
    "    /* Hard safety gate: 41.14 and older do not understand CAPTURE_ONLY and\n     * would treat the temporary file as an ordinary printer configuration. */\n    if (!mp_read_driver_version(MINTPRINT_DRIVER_DEST, &ver) ||\n        ver.version < 41 || (ver.version == 41 && ver.revision < 15)) {\n        printf(\"Test Suite requires installed MintPRINT driver 41.15 or newer.\\n\");",
    "    /* 41.15 introduced safe CAPTURE_ONLY. 41.16 additionally emits the\n     * byte-exact IPP request sidecar that this suite now requires per case. */\n    if (!mp_read_driver_version(MINTPRINT_DRIVER_DEST, &ver) ||\n        ver.version < 41 || (ver.version == 41 && ver.revision < 16)) {\n        printf(\"Test Suite requires installed MintPRINT driver 41.16 or newer.\\n\");"
)
replace_once(
    "src/MintPrintSettings.c",
    "        \"Run 32 capture-only regression jobs?\\n\\n\"",
    "        \"Run 33 capture-only regression jobs?\\n\\n\""
)
replace_once(
    "src/MintPrintSettings.c",
    "    g_test_suite.active = TRUE;\n    g_test_suite.current = 0;",
    "    g_test_suite.active = TRUE;\n    mp_test_suite_capture_mode = TRUE;\n    g_test_suite.current = 0;"
)
replace_once(
    "src/MintPrintSettings.c",
    "    g_test_suite.active = FALSE;\n    mp_test_suite_set_button(win, TRUE);",
    "    g_test_suite.active = FALSE;\n    mp_test_suite_capture_mode = FALSE;\n    mp_test_suite_set_button(win, TRUE);"
)
replace_once(
    "src/MintPrintSettings.c",
    "             \"matrix=5 engines x 5 scaling modes plus PWG/URF duplex and PostScript margin coverage\\n\"",
    "             \"matrix=5 engines x 5 scaling modes plus PWG transform/URF duplex and PostScript margin coverage\\n\""
)

# Version markers for both driver ABIs.
replace_once(
    "driver/printertag.s",
    ".asciz  \"$VER: MintPRINT 41.15 (01.09.2026)\"",
    ".asciz  \"$VER: MintPRINT 41.16 (01.09.2026)\""
)
replace_once(
    "driver/printertag_classic.s",
    ".asciz  \"$VER: MintPRINT 41.15 (01.09.2026)\"",
    ".asciz  \"$VER: MintPRINT 41.16 (01.09.2026)\""
)

# Changelog: keep the merged 41.15 entry as history, add the follow-up above it.
replace_once(
    "CHANGELOG.md",
    "## Unreleased\n\n- **Capture-only output regression suite (driver 41.15).**",
    "## Unreleased\n\n- **Regression-suite follow-up and JPEG density metadata (driver 41.16).**\n  Raw JPEG now writes the selected capture DPI into its JFIF X/Y density\n  fields instead of always claiming 300 dpi; embedded JPEG streams in PDF\n  and PostScript receive the same accurate metadata. The capture suite now\n  writes a byte-exact `.ipp` sidecar for every rendered document using the\n  same Print-Job attribute builder as the live submission path, verifies the\n  sidecar exists, and expands PWG duplex coverage to 33 total cases so every\n  CrossFeed/Feed transform sign combination is exercised. Suite completion\n  text now explicitly says no printer job was sent.\n- **Capture-only output regression suite (driver 41.15).**"
)

print("follow-up patch applied")
