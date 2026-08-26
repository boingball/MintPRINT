from pathlib import Path

p = Path('src/MintPrintSettings.c')
s = p.read_text(encoding='utf-8')

for inc in (
    '#include <proto/datatypes.h>\n',
    '#include <datatypes/pictureclass.h>\n',
    '#include <datatypes/datatypesclass.h>\n',
):
    s = s.replace(inc, '', 1)

old = ''' * Fetch the first HTTP URI only. picture.datatype performs format decode
 * and remapping (Brother's AirPrint icon is PNG), then graphics.library
 * scales the bitmap into a small 32x32 preview beside the duplex hint.
 * This is deliberately optional: unsupported URI/image format/download
 * failure simply leaves the preview blank.
'''
new = ''' * Fetch the first HTTP URI only.  PNG decoding is handled internally by
 * the same LodePNG decoder used by MintAMP, producing RGBA pixels with real
 * alpha.  MintPRINT area-averages that image down to 32x32, composites the
 * translucent edge pixels against the GUI background, then maps the result
 * to the current screen's pens.  No picture.datatype is required.
 * This is deliberately optional: unsupported URI/download/decode failure
 * simply leaves the preview blank.
'''
if old not in s:
    raise SystemExit('old datatype comment not found')
s = s.replace(old, new, 1)

s = s.replace('    int dx;\n    int dy;\n', '    int dy;\n', 1)

p.write_text(s, encoding='utf-8')
print('LodePNG cleanup staged')
