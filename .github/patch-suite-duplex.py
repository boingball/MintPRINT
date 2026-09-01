from pathlib import Path

p = Path("src/MintPrintSettings.c")
s = p.read_text()

def rep(old, new, label):
    global s
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected 1 anchor, found {n}")
    s = s.replace(old, new, 1)

rep(
'''    BOOL bitmap_manual;
    BOOL device_open;
    BOOL active;
};

static struct MPTestPrintJob test_print_job;
''',
'''    BOOL bitmap_manual;
    BOOL device_open;
    BOOL active;
    int extra_pages;
};

static struct MPTestPrintJob test_print_job;
/* Normal Test Print keeps both at zero/FALSE. The regression suite sets them
 * only around mintprint_test_page(): the first prevents the suite's synthetic
 * matrix values being persisted over the user's Unit0, while the second asks
 * one already-open printer.device request to emit an additional page before
 * CloseDevice/DriverClose finalises the document. */
static int mp_test_print_extra_pages_requested = 0;
static BOOL mp_test_print_skip_config_save = FALSE;
''',
"test print state")

rep(
'''    test_print_job.active = FALSE;
    mp_set_test_print_enabled(win, TRUE);
}

static void mp_test_print_complete(struct Window *win)
''',
'''    test_print_job.active = FALSE;
    test_print_job.extra_pages = 0;
    mp_set_test_print_enabled(win, TRUE);
}

static void mp_test_print_mark_duplex_second_page(void)
{
    static const char marker[] = "DUPLEX PAGE 2";
    struct RastPort *rp = &test_print_job.rastport;

    if (!test_print_job.bitmap) return;

    /* Four deliberately different corner marks make X/Y reversal obvious in
     * raw PWG/URF captures. Keep them well inside the page border so they do
     * not depend on printer margins. The text removes any ambiguity about
     * which raster is the back/second page. */
    SetAPen(rp, 2); /* red: top-left */
    RectFill(rp, 10, 10, 34, 28);
    SetAPen(rp, 3); /* green: top-right */
    RectFill(rp, MP_TESTPAGE_WIDTH - 35, 34,
             MP_TESTPAGE_WIDTH - 11, 52);
    SetAPen(rp, 4); /* blue: bottom-left */
    RectFill(rp, 10, MP_TESTPAGE_HEIGHT - 53,
             34, MP_TESTPAGE_HEIGHT - 35);
    SetAPen(rp, 7); /* yellow: bottom-right */
    RectFill(rp, MP_TESTPAGE_WIDTH - 35, MP_TESTPAGE_HEIGHT - 29,
             MP_TESTPAGE_WIDTH - 11, MP_TESTPAGE_HEIGHT - 11);
    SetAPen(rp, 1);
    Move(rp, 104, 62);
    Text(rp, (STRPTR)marker, sizeof(marker) - 1);
}

static void mp_test_print_complete(struct Window *win)
''',
"duplex page marker helper")

rep(
'''    ioerr = WaitIO((struct IORequest *)test_print_job.request);
    request_error = (LONG)test_print_job.request->io_Error;
    /* CloseDevice may still submit a pending/duplex page. Finish that
     * before reporting completion, saving the request errors before the
     * release helper deletes the IORequest. */
    mp_test_print_release(win);
''',
'''    ioerr = WaitIO((struct IORequest *)test_print_job.request);
    request_error = (LONG)test_print_job.request->io_Error;

    /* A regression-suite duplex case stays inside this SAME OpenDevice /
     * DriverOpen bracket for page 2. That is what turns the capture into a
     * genuine multi-page PWG/URF job and exercises backside transforms and
     * URF page-count patching. WaitIO guarantees the first raster is no
     * longer using the bitmap, so it is safe to add asymmetric page-2 marks
     * before reusing the completed IODRPReq. */
    if (ioerr == 0 && request_error == 0 && test_print_job.extra_pages > 0) {
        --test_print_job.extra_pages;
        mp_test_print_mark_duplex_second_page();
        test_print_job.request->io_Error = 0;
        printf("Test Print: sending duplex page 2 through same printer.device job...\\n");
        SendIO((struct IORequest *)test_print_job.request);
        return;
    }

    /* CloseDevice may still submit/finalise a pending/duplex document. Finish
     * that before reporting completion, saving the request errors before the
     * release helper deletes the IORequest. */
    mp_test_print_release(win);
''',
"duplex second send")

rep(
'''static BOOL mintprint_test_page(struct Window *win) {
    ULONG mode_id = 0;
''',
'''static BOOL mintprint_test_page(struct Window *win) {
    ULONG mode_id = 0;
    int requested_extra_pages = mp_test_print_extra_pages_requested;

    /* Consume this up front so an early-return failure cannot leak a duplex
     * request into a later ordinary Test Print. */
    mp_test_print_extra_pages_requested = 0;
''',
"consume extra pages")

rep(
'''    /* Test the settings the user is looking at, and make them the live Unit0. */
    if (!save_driver_config(win)) {
        printf("Test Print: could not save Unit0 settings\\n");
        return FALSE;
    }
''',
'''    /* Ordinary Test Print tests what the user is looking at and makes it
     * live Unit0. The regression suite supplies its one-shot T: config
     * directly and MUST NOT overwrite the user's persistent Unit0 while it
     * cycles synthetic matrix values through these same in-memory fields. */
    if (!mp_test_print_skip_config_save && !save_driver_config(win)) {
        printf("Test Print: could not save Unit0 settings\\n");
        return FALSE;
    }
''',
"skip suite Unit0 save")

rep(
'''    test_print_job.active = TRUE;
    mp_set_test_print_enabled(win, FALSE);
''',
'''    test_print_job.extra_pages = requested_extra_pages;
    test_print_job.active = TRUE;
    mp_set_test_print_enabled(win, FALSE);
''',
"assign extra pages")

rep(
'''        printf("Test Suite %d/%d: %s %s %ddpi\\n",
               g_test_suite.current + 1, MP_TEST_SUITE_CASES,
               c.engine, c.scaling, c.resolution);
        if (mintprint_test_page(win)) return;

        DeleteFile((CONST_STRPTR)"T:MintPRINT-testsuite.cfg");
''',
'''        printf("Test Suite %d/%d: %s %s %ddpi\\n",
               g_test_suite.current + 1, MP_TEST_SUITE_CASES,
               c.engine, c.scaling, c.resolution);
        mp_test_print_skip_config_save = TRUE;
        mp_test_print_extra_pages_requested =
            (strcmp(c.sides, "one-sided") != 0) ? 1 : 0;
        {
            BOOL started = mintprint_test_page(win);
            mp_test_print_skip_config_save = FALSE;
            mp_test_print_extra_pages_requested = 0;
            if (started) return;
        }

        DeleteFile((CONST_STRPTR)"T:MintPRINT-testsuite.cfg");
''',
"suite launch flags")

rep(
'''            mp_test_print_complete(win);
            if (g_test_suite.active) mp_test_suite_advance(win);
''',
'''            mp_test_print_complete(win);
            /* A duplex suite case re-sends page 2 from the completion helper.
             * Advance only after that second request has also completed and
             * mp_test_print_release() has closed the device/document. */
            if (g_test_suite.active && !test_print_job.active)
                mp_test_suite_advance(win);
''',
"suite completion gate")

p.write_text(s)

# Keep docs and PR-facing changelog precise about real two-page duplex coverage.
p = Path("docs/MINTPRINT_PREFS.md")
s = p.read_text()
old = "PWG/URF receive additional duplex cases and\nPostScript receives an explicit non-zero-margin case."
new = "PWG/URF receive additional **two-page** duplex cases (page 2 carries\nasymmetric corner marks so backside rotation/flipping can be verified) and\nPostScript receives an explicit non-zero-margin case."
if s.count(old) != 1:
    raise SystemExit(f"prefs duplex docs anchor: {s.count(old)}")
p.write_text(s.replace(old, new, 1))

p = Path("CHANGELOG.md")
s = p.read_text()
old = "  making any IPP submission. The suite is hard-gated on driver 41.15+ so an\n"
new = "  making any IPP submission. Duplex cases are genuine two-page documents with\n  asymmetric page-2 marks for backside transform inspection. The suite is\n  hard-gated on driver 41.15+ so an\n"
if s.count(old) != 1:
    raise SystemExit(f"changelog duplex anchor: {s.count(old)}")
p.write_text(s.replace(old, new, 1))

print("two-page duplex suite and non-persistent Unit0 fixes applied")
