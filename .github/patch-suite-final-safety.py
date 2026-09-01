from pathlib import Path
p = Path('src/MintPrintSettings.c')
s = p.read_text()

def rep(old, new, label):
    global s
    n = s.count(old)
    if n != 1:
        raise SystemExit(f'{label}: expected 1 anchor, found {n}')
    s = s.replace(old, new, 1)

# Keep the function valid for older m68k GCC dialects: no declaration after
# the statement that consumes the one-shot extra-page request.
rep(
'''static BOOL mintprint_test_page(struct Window *win) {
    ULONG mode_id = 0;
    int requested_extra_pages = mp_test_print_extra_pages_requested;

    /* Consume this up front so an early-return failure cannot leak a duplex
     * request into a later ordinary Test Print. */
    mp_test_print_extra_pages_requested = 0;
    LONG left = 16, right = MP_TESTPAGE_WIDTH - 17;
''',
'''static BOOL mintprint_test_page(struct Window *win) {
    ULONG mode_id = 0;
    int requested_extra_pages = mp_test_print_extra_pages_requested;
    LONG left = 16, right = MP_TESTPAGE_WIDTH - 17;
''',
'c89 declaration order part 1')
rep(
'''    int num_info_lines = 0;
    BOOL is_postscript;

    if (test_print_job.active) {
''',
'''    int num_info_lines = 0;
    BOOL is_postscript;

    /* Consume this after declarations so the function stays friendly to the
     * older GCC dialect used by classic AmigaOS builds. An early return can
     * therefore never leak a suite duplex request into a later normal test. */
    mp_test_print_extra_pages_requested = 0;

    if (test_print_job.active) {
''',
'c89 declaration order part 2')

rep(
'''                        case GAD_SAVE_BUTTON:
                            if (save_driver_config(win))
                                printf("MintPRINT Unit%d saved to ENV: and ENVARC:\\n", current_unit_index);
                            else
                                printf("Failed to save MintPRINT Unit%d settings\\n", current_unit_index);
                            break;
''',
'''                        case GAD_SAVE_BUTTON:
                            if (g_test_suite.active) {
                                printf("Save is disabled while Test Suite is running.\\n");
                            } else if (save_driver_config(win)) {
                                printf("MintPRINT Unit%d saved to ENV: and ENVARC:\\n", current_unit_index);
                            } else {
                                printf("Failed to save MintPRINT Unit%d settings\\n", current_unit_index);
                            }
                            break;
''',
'save button suite guard')

rep(
'''                                    case 0: // Save Settings
                                        save_print_mode();
                                        if (save_driver_config(win))
                                            printf("MintPRINT Unit%d saved to ENV: and ENVARC:\\n", current_unit_index);
                                        else
                                            printf("Failed to save MintPRINT Unit%d settings\\n", current_unit_index);
                                        break;
''',
'''                                    case 0: // Save Settings
                                        if (g_test_suite.active) {
                                            printf("Save is disabled while Test Suite is running.\\n");
                                        } else {
                                            save_print_mode();
                                            if (save_driver_config(win))
                                                printf("MintPRINT Unit%d saved to ENV: and ENVARC:\\n", current_unit_index);
                                            else
                                                printf("Failed to save MintPRINT Unit%d settings\\n", current_unit_index);
                                        }
                                        break;
''',
'menu save suite guard')

p.write_text(s)
print('final suite C89 and Unit0 save guards applied')
