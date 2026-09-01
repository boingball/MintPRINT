from pathlib import Path
p = Path('src/MintPrintSettings.c')
s = p.read_text()

old = '''static void mp_test_print_release(struct Window *win)
'''
new = '''/* Shared Test Print canvas dimensions. Keep these before the async completion
 * helpers as the regression suite's page-2 marker also uses them. */
#define MP_TESTPAGE_WIDTH  320
#define MP_TESTPAGE_HEIGHT 453
#define MP_TESTPAGE_DEPTH  3   /* 8 pens: 2^3 */
#define MP_TESTPAGE_COLORS 8

static void mp_test_print_release(struct Window *win)
'''
if s.count(old) != 1:
    raise SystemExit(f'early test page define anchor: {s.count(old)}')
s = s.replace(old, new, 1)

old = '''#define MP_TESTPAGE_WIDTH  320
#define MP_TESTPAGE_HEIGHT 453
#define MP_TESTPAGE_DEPTH  3   /* 8 pens: 2^3 */
#define MP_TESTPAGE_COLORS 8

/* PostScript alone still gets an exact small physical target
'''
new = '''/* PostScript alone still gets an exact small physical target
'''
if s.count(old) != 1:
    raise SystemExit(f'late test page define anchor: {s.count(old)}')
s = s.replace(old, new, 1)
p.write_text(s)
print('moved Test Print canvas constants before duplex marker helper')
