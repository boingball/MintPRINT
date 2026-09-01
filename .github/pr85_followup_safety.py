from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    s = p.read_text()
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"{path}: expected one anchor, found {n}: {old[:90]!r}")
    p.write_text(s.replace(old, new, 1))

# Never do DOS file I/O from the printer.device callback. Restore the driver's
# capture branch to document-only retention; MintPrintSettings (a real Process)
# will build/write the byte-exact IPP sidecar after each capture completes.
replace_once(
    "driver/driver_core.c",
    "    } else if (g_config.capture_only) {\n        /* Regression suite: retain both the rendered document and the exact\n         * IPP operation bytes that the normal submit path would prepend.\n         * mp_ipp_capture_request() is file-only and never opens a socket. */\n        ipp_rc = mp_ipp_capture_request(&g_config, fmt, fname);\n        result.error = ipp_rc;\n        result.http_status = 0;\n        result.ipp_status = 0;\n        result.document_bytes = g_job_file_bytes;\n        if (ipp_rc == 0)\n            mp_log_text(\"Capture-only document + IPP request retained; network submission skipped\");\n        else\n            mp_log_3(\"Capture IPP request sidecar failed rc/zero/zero\", ipp_rc, 0, 0);",
    "    } else if (g_config.capture_only) {\n        /* Regression suite: the rendered document itself is the result.\n         * Never open a TCP connection, and report a synthetic success so\n         * normal page bookkeeping can finish. The GUI Process creates the\n         * matching byte-exact IPP request sidecar after this capture ends. */\n        ipp_rc = 0;\n        result.error = 0;\n        result.http_status = 0;\n        result.ipp_status = 0;\n        result.document_bytes = g_job_file_bytes;\n        mp_log_text(\"Capture-only regression job retained; network submission skipped\");"
)
replace_once(
    "driver/driver_core.c",
    "            if (g_config.capture_only) {\n                ipp_rc = mp_ipp_capture_request(&g_config,\n                                                mp_document_format(),\n                                                mp_job_filename());\n                result.error = ipp_rc;\n                result.http_status = 0;\n                result.ipp_status = 0;\n                result.document_bytes = g_job_file_bytes;\n                if (ipp_rc == 0)\n                    mp_log_text(\"Capture-only duplex document + IPP request retained; network submission skipped\");\n                else\n                    mp_log_3(\"Capture duplex IPP sidecar failed rc/zero/zero\",\n                             ipp_rc, 0, 0);\n                mp_log_ipp_result(\"Capture duplex result error/http/status\",\n                                  &result);",
    "            if (g_config.capture_only) {\n                ipp_rc = 0;\n                result.error = 0;\n                result.http_status = 0;\n                result.ipp_status = 0;\n                result.document_bytes = g_job_file_bytes;\n                mp_log_text(\"Capture-only duplex job retained; network submission skipped\");\n                mp_log_ipp_result(\"Capture duplex result error/http/status\",\n                                  &result);"
)

# Add a GUI-side helper that constructs the same MPConfig fields as the
# temporary test config and asks ipp_client.c to serialize the real Print-Job
# operation bytes beside the already-finished document.
anchor = "static BOOL mp_test_suite_file_exists(CONST_STRPTR path, LONG *size_out)\n{"
helper = r'''static CONST_STRPTR mp_test_suite_document_format(const char *engine)
{
    if (strcmp(engine, "pwg-raster") == 0)
        return (CONST_STRPTR)"image/pwg-raster";
    if (strcmp(engine, "pdf") == 0)
        return (CONST_STRPTR)"application/pdf";
    if (strcmp(engine, "postscript") == 0)
        return (CONST_STRPTR)"application/postscript";
    if (strcmp(engine, "urf") == 0)
        return (CONST_STRPTR)"image/urf";
    return (CONST_STRPTR)"image/jpeg";
}

static LONG mp_test_suite_write_ipp_sidecar(const struct MPTestSuiteCase *c)
{
    struct MPConfig cfg;

    if (!c) return -1;
    mp_config_defaults(&cfg);
    mp_test_suite_copy_string(cfg.host, sizeof(cfg.host), "127.0.0.1");
    cfg.port = 631;
    mp_test_suite_copy_string(cfg.path, sizeof(cfg.path), "/ipp/print");
    mp_test_suite_copy_string(cfg.media, sizeof(cfg.media), c->media);
    mp_test_suite_copy_string(cfg.source, sizeof(cfg.source), c->source);
    mp_test_suite_copy_string(cfg.color, sizeof(cfg.color), c->color);
    mp_test_suite_copy_string(cfg.quality, sizeof(cfg.quality), c->quality);
    mp_test_suite_copy_string(cfg.scaling, sizeof(cfg.scaling), c->scaling);
    mp_test_suite_copy_string(cfg.sides, sizeof(cfg.sides), c->sides);

    return mp_ipp_capture_request(&cfg,
                                  mp_test_suite_document_format(c->engine),
                                  (CONST_STRPTR)g_test_suite.output_path);
}

'''
replace_once("src/MintPrintSettings.c", anchor, helper + anchor)

replace_once(
    "src/MintPrintSettings.c",
    "static void mp_test_suite_advance(struct Window *win)\n{\n    LONG bytes = 0;\n    LONG ipp_bytes = 0;\n    BOOL output_ok;\n    BOOL ipp_ok;\n    char line[320];",
    "static void mp_test_suite_advance(struct Window *win)\n{\n    struct MPTestSuiteCase c;\n    LONG bytes = 0;\n    LONG ipp_bytes = 0;\n    LONG ipp_rc = -1;\n    BOOL output_ok;\n    BOOL ipp_ok;\n    char line[320];"
)
replace_once(
    "src/MintPrintSettings.c",
    "    output_ok = mp_test_suite_file_exists(\n                    (CONST_STRPTR)g_test_suite.output_path, &bytes) && bytes > 0;\n    ipp_ok = mp_test_suite_file_exists(\n                 (CONST_STRPTR)g_test_suite.ipp_path, &ipp_bytes) && ipp_bytes > 0;",
    "    output_ok = mp_test_suite_file_exists(\n                    (CONST_STRPTR)g_test_suite.output_path, &bytes) && bytes > 0;\n    mp_test_suite_case(g_test_suite.current, &c);\n    if (output_ok)\n        ipp_rc = mp_test_suite_write_ipp_sidecar(&c);\n    ipp_ok = ipp_rc == 0 && mp_test_suite_file_exists(\n                 (CONST_STRPTR)g_test_suite.ipp_path, &ipp_bytes) && ipp_bytes > 0;"
)
replace_once(
    "src/MintPrintSettings.c",
    "                 \"CASE-RESULT %03d ERROR output=%s ipp=%s log=%s\\n\",\n                 g_test_suite.current + 1,\n                 output_ok ? \"ok\" : \"missing-or-empty\",\n                 ipp_ok ? \"ok\" : \"missing-or-empty\",\n                 g_test_suite.log_path);",
    "                 \"CASE-RESULT %03d ERROR output=%s ipp=%s ipp-rc=%ld log=%s\\n\",\n                 g_test_suite.current + 1,\n                 output_ok ? \"ok\" : \"missing-or-empty\",\n                 ipp_ok ? \"ok\" : \"missing-or-empty\", ipp_rc,\n                 g_test_suite.log_path);"
)

# Clarify the public helper comment: it is intentionally a Process-side test
# facility; it performs AmigaDOS file I/O but never network I/O.
replace_once(
    "driver/ipp_client.h",
    "/* Capture the exact IPP Print-Job operation body (attributes through\n * end-of-attributes, excluding document bytes) next to document_filename as\n * <document_filename>.ipp. No socket is opened. Used by the regression suite. */",
    "/* Capture the exact IPP Print-Job operation body (attributes through\n * end-of-attributes, excluding document bytes) next to document_filename as\n * <document_filename>.ipp. No socket is opened. This performs AmigaDOS file\n * I/O and must therefore be called from a Process (MintPrintSettings does so\n * after each regression capture), never from a printer.device callback Task. */"
)

print("sidecar generation moved safely to Settings Process")
