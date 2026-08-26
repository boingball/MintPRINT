from pathlib import Path

src = Path('src/MintPrintSettings.c')
s = src.read_text(encoding='utf-8')

# Local PNG decoder from MintAMP's proven LodePNG path.
if '#include "lodepng.h"\n' not in s:
    anchor = '#include "ipp_enum.h"\n'
    if anchor not in s:
        raise SystemExit('ipp_enum include anchor missing')
    s = s.replace(anchor, anchor + '#include "lodepng.h"\n', 1)

# graphics/scale.h and DataTypesBase were only needed by the printer-icon datatype path.
s = s.replace('#include <graphics/scale.h>\n', '', 1)
s = s.replace('struct Library *DataTypesBase = NULL;\n', '', 1)

# Replace printer icon storage with a tiny cached 32x32 RGBA image plus cached screen pens.
start = s.find('#define MP_PRINTER_ICON_LEFT')
end = s.find('/* Ink/toner status', start)
if start < 0 or end < 0:
    raise SystemExit('printer icon globals block not found')
s = s[:start] + '''#define MP_PRINTER_ICON_LEFT  420
#define MP_PRINTER_ICON_TOP   117
#define MP_PRINTER_ICON_SIZE   32
#define MP_PRINTER_ICON_TEMP  "T:MintPRINT-printer-icon.img"
#define MP_PRINTER_ICON_PIXELS (MP_PRINTER_ICON_SIZE * MP_PRINTER_ICON_SIZE)
#define MP_PRINTER_ICON_MAX_SOURCE_DIM 1024
static UBYTE mp_printer_icon_rgba[MP_PRINTER_ICON_PIXELS * 4];
static UBYTE mp_printer_icon_pens[MP_PRINTER_ICON_PIXELS];
static UBYTE mp_printer_icon_mask[MP_PRINTER_ICON_PIXELS];
static BOOL mp_printer_icon_valid = FALSE;
static BOOL mp_printer_icon_pens_valid = FALSE;

''' + s[end:]

# Replace clear helper.
start = s.find('static void mp_clear_printer_icon(void) {')
end = s.find('static BOOL mp_fetch_printer_icon_file', start)
if start < 0 or end < 0:
    raise SystemExit('printer icon clear/fetch boundary not found')
clear_fn = '''static void mp_clear_printer_icon(void) {
    mp_printer_icon_valid = FALSE;
    mp_printer_icon_pens_valid = FALSE;
    memset(mp_printer_icon_rgba, 0, sizeof(mp_printer_icon_rgba));
    memset(mp_printer_icon_mask, 0, sizeof(mp_printer_icon_mask));
    DeleteFile((CONST_STRPTR)MP_PRINTER_ICON_TEMP);
}

'''
s = s[:start] + clear_fn + s[end:]

# Replace datatype/BitMap loader with LodePNG RGBA decode + area-average downsample.
start = s.find('static BOOL mp_load_printer_icon_bitmap(void) {')
end = s.find('static void mp_refresh_printer_icon(void) {', start)
if start < 0 or end < 0:
    raise SystemExit('old printer icon bitmap loader not found')
loader = r'''static BOOL mp_load_printer_icon_rgba(void) {
    static const UBYTE png_signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    BPTR file;
    LONG file_size;
    UBYTE *png_data = NULL;
    unsigned char *decoded = NULL;
    unsigned png_w = 0;
    unsigned png_h = 0;
    unsigned err;
    int draw_w;
    int draw_h;
    int off_x;
    int off_y;
    int dx;
    int dy;

    file = Open((CONST_STRPTR)MP_PRINTER_ICON_TEMP, MODE_OLDFILE);
    if (!file)
        return FALSE;

    if (Seek(file, 0, OFFSET_END) == -1) {
        Close(file);
        return FALSE;
    }
    file_size = Seek(file, 0, OFFSET_BEGINNING);
    if (file_size < 24 || file_size > MAX_BUFFER) {
        Close(file);
        return FALSE;
    }

    png_data = AllocVec((ULONG)file_size, MEMF_ANY);
    if (!png_data) {
        Close(file);
        return FALSE;
    }
    if (Read(file, png_data, file_size) != file_size) {
        Close(file);
        FreeVec(png_data);
        return FALSE;
    }
    Close(file);

    /* Reject non-PNG and decompression-bomb dimensions before LodePNG
     * allocates width*height*4.  PNG's IHDR width/height live at bytes
     * 16..23 and are big-endian. */
    if (memcmp(png_data, png_signature, sizeof(png_signature)) != 0) {
        FreeVec(png_data);
        return FALSE;
    }
    png_w = ((unsigned)png_data[16] << 24) |
            ((unsigned)png_data[17] << 16) |
            ((unsigned)png_data[18] << 8) |
            (unsigned)png_data[19];
    png_h = ((unsigned)png_data[20] << 24) |
            ((unsigned)png_data[21] << 16) |
            ((unsigned)png_data[22] << 8) |
            (unsigned)png_data[23];
    if (png_w == 0 || png_h == 0 ||
        png_w > MP_PRINTER_ICON_MAX_SOURCE_DIM ||
        png_h > MP_PRINTER_ICON_MAX_SOURCE_DIM) {
        FreeVec(png_data);
        return FALSE;
    }

    err = lodepng_decode32(&decoded, &png_w, &png_h,
                           png_data, (size_t)file_size);
    FreeVec(png_data);
    if (err || !decoded)
        return FALSE;

    memset(mp_printer_icon_rgba, 0, sizeof(mp_printer_icon_rgba));
    memset(mp_printer_icon_mask, 0, sizeof(mp_printer_icon_mask));

    draw_w = MP_PRINTER_ICON_SIZE;
    draw_h = MP_PRINTER_ICON_SIZE;
    if (png_w > png_h)
        draw_h = (int)((png_h * MP_PRINTER_ICON_SIZE) / png_w);
    else if (png_h > png_w)
        draw_w = (int)((png_w * MP_PRINTER_ICON_SIZE) / png_h);
    if (draw_w < 1) draw_w = 1;
    if (draw_h < 1) draw_h = 1;
    off_x = (MP_PRINTER_ICON_SIZE - draw_w) / 2;
    off_y = (MP_PRINTER_ICON_SIZE - draw_h) / 2;

    /* Area-average each destination pixel.  The source Brother icon is
     * normally 128x128, so this is just a cheap 4x4 average and gives a
     * much nicer tiny icon than nearest-neighbour.  RGB is accumulated
     * premultiplied by alpha so transparent coloured pixels cannot bleed
     * a red/black matte into the edges. */
    for (dy = 0; dy < draw_h; ++dy) {
        unsigned sy0 = (unsigned)((dy * (int)png_h) / draw_h);
        unsigned sy1 = (unsigned)(((dy + 1) * (int)png_h) / draw_h);
        int dx2;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > png_h) sy1 = png_h;

        for (dx2 = 0; dx2 < draw_w; ++dx2) {
            unsigned sx0 = (unsigned)((dx2 * (int)png_w) / draw_w);
            unsigned sx1 = (unsigned)(((dx2 + 1) * (int)png_w) / draw_w);
            unsigned sy;
            ULONG sum_a = 0;
            ULONG sum_ra = 0;
            ULONG sum_ga = 0;
            ULONG sum_ba = 0;
            ULONG samples = 0;
            int dest = (off_y + dy) * MP_PRINTER_ICON_SIZE + (off_x + dx2);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > png_w) sx1 = png_w;

            for (sy = sy0; sy < sy1; ++sy) {
                unsigned sx;
                for (sx = sx0; sx < sx1; ++sx) {
                    const unsigned char *p = decoded + ((sy * png_w + sx) * 4U);
                    ULONG a = p[3];
                    sum_a += a;
                    sum_ra += (ULONG)p[0] * a;
                    sum_ga += (ULONG)p[1] * a;
                    sum_ba += (ULONG)p[2] * a;
                    ++samples;
                }
            }

            if (samples && sum_a) {
                UBYTE *d = mp_printer_icon_rgba + dest * 4;
                d[0] = (UBYTE)(sum_ra / sum_a);
                d[1] = (UBYTE)(sum_ga / sum_a);
                d[2] = (UBYTE)(sum_ba / sum_a);
                d[3] = (UBYTE)(sum_a / samples);
                mp_printer_icon_mask[dest] = d[3] ? 1 : 0;
            }
        }
    }

    free(decoded); /* matching allocator used by lodepng.c */
    mp_printer_icon_valid = TRUE;
    mp_printer_icon_pens_valid = FALSE;
    return TRUE;
}

'''
s = s[:start] + loader + s[end:]

# Refresh uses our decoder now.
s = s.replace('        mp_load_printer_icon_bitmap();',
              '        mp_load_printer_icon_rgba();', 1)

# Replace drawing path with cached pen mapping and proper alpha compositing.
start = s.find('static void mp_draw_printer_icon(void) {')
end = s.find('// Updated query_printer_attributes', start)
if start < 0 or end < 0:
    raise SystemExit('printer icon draw function boundary not found')
draw_fn = r'''static UBYTE mp_printer_icon_nearest_pen(const ULONG *palette,
                                         int pen_count,
                                         UBYTE r, UBYTE g, UBYTE b) {
    int i;
    int best = 0;
    ULONG best_distance = 0xffffffffUL;

    for (i = 0; i < pen_count; ++i) {
        LONG pr = (LONG)((palette[i * 3 + 0] >> 24) & 0xffUL);
        LONG pg = (LONG)((palette[i * 3 + 1] >> 24) & 0xffUL);
        LONG pb = (LONG)((palette[i * 3 + 2] >> 24) & 0xffUL);
        LONG dr = (LONG)r - pr;
        LONG dg = (LONG)g - pg;
        LONG db = (LONG)b - pb;
        ULONG distance = (ULONG)(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
            if (distance == 0)
                break;
        }
    }
    return (UBYTE)best;
}

static void mp_draw_printer_icon(void) {
    ULONG screen_palette[3 * 256];
    struct ColorMap *cm;
    struct RastPort *rp;
    int screen_pen_count;
    int left = MP_PRINTER_ICON_LEFT;
    int top = g_topborder + MP_PRINTER_ICON_TOP;
    int i;
    LONG last_pen = -1;

    if (!window || !screen)
        return;

    rp = window->RPort;
    cm = screen->ViewPort.ColorMap;
    if (!cm || cm->Count == 0)
        return;

    screen_pen_count = (int)cm->Count;
    if (screen_pen_count > 256)
        screen_pen_count = 256;
    GetRGB32(cm, 0, (ULONG)screen_pen_count, screen_palette);

    SetDrMd(rp, JAM1);
    SetAPen(rp, 0);
    RectFill(rp, left - 1, top - 1,
             left + MP_PRINTER_ICON_SIZE, top + MP_PRINTER_ICON_SIZE);

    if (!mp_printer_icon_valid)
        return;

    /* Convert RGBA to the current screen's pens once per downloaded icon,
     * not on every refresh.  Partial alpha is composited against pen 0,
     * which is exactly the background we just cleared the icon box with. */
    if (!mp_printer_icon_pens_valid) {
        UBYTE bg_r = (UBYTE)((screen_palette[0] >> 24) & 0xffUL);
        UBYTE bg_g = (UBYTE)((screen_palette[1] >> 24) & 0xffUL);
        UBYTE bg_b = (UBYTE)((screen_palette[2] >> 24) & 0xffUL);

        for (i = 0; i < MP_PRINTER_ICON_PIXELS; ++i) {
            const UBYTE *p = mp_printer_icon_rgba + i * 4;
            ULONG a = p[3];
            UBYTE r;
            UBYTE g;
            UBYTE b;

            if (a == 0) {
                mp_printer_icon_mask[i] = 0;
                mp_printer_icon_pens[i] = 0;
                continue;
            }

            r = (UBYTE)(((ULONG)p[0] * a + (ULONG)bg_r * (255UL - a) + 127UL) / 255UL);
            g = (UBYTE)(((ULONG)p[1] * a + (ULONG)bg_g * (255UL - a) + 127UL) / 255UL);
            b = (UBYTE)(((ULONG)p[2] * a + (ULONG)bg_b * (255UL - a) + 127UL) / 255UL);
            mp_printer_icon_pens[i] = mp_printer_icon_nearest_pen(screen_palette,
                                                                  screen_pen_count,
                                                                  r, g, b);
            mp_printer_icon_mask[i] = 1;
        }
        mp_printer_icon_pens_valid = TRUE;
    }

    for (i = 0; i < MP_PRINTER_ICON_PIXELS; ++i) {
        int x;
        int y;
        UBYTE pen;
        if (!mp_printer_icon_mask[i])
            continue;
        pen = mp_printer_icon_pens[i];
        if ((LONG)pen != last_pen) {
            SetAPen(rp, pen);
            last_pen = (LONG)pen;
        }
        x = i % MP_PRINTER_ICON_SIZE;
        y = i / MP_PRINTER_ICON_SIZE;
        WritePixel(rp, left + x, top + y);
    }
}

'''
s = s[:start] + draw_fn + s[end:]

# Remove DataTypesBase shutdown block if still present.
s = s.replace('    if (DataTypesBase) {\n        CloseLibrary(DataTypesBase);\n        DataTypesBase = NULL;\n    }\n', '', 1)

src.write_text(s, encoding='utf-8')

# Build MintPrintSettings with the decoder-only subset of LodePNG.
mk = Path('Makefile')
m = mk.read_text(encoding='utf-8')
old_dep = 'driver/media_size.c driver/media_size.h $(IFF_DIR_ESC)/iff-loader.c $(IFF_DIR_ESC)/iff-loader.h\n\t$(CC) -O2 -I"$(IFF_DIR)" -Isrc -Idriver -o $@ src/MintPrintSettings.c src/http_response.c src/dpi_options.c src/ipp_enum.c driver/media_size.c "$(IFF_DIR)/iff-loader.c" -lamiga -lm\n'
new_dep = 'driver/media_size.c driver/media_size.h src/lodepng.c src/lodepng.h $(IFF_DIR_ESC)/iff-loader.c $(IFF_DIR_ESC)/iff-loader.h\n\t$(CC) -O2 -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS -DLODEPNG_NO_COMPILE_ERROR_TEXT -I"$(IFF_DIR)" -Isrc -Idriver -o $@ src/MintPrintSettings.c src/http_response.c src/dpi_options.c src/ipp_enum.c src/lodepng.c driver/media_size.c "$(IFF_DIR)/iff-loader.c" -lamiga -lm\n'
if old_dep not in m:
    raise SystemExit('Makefile GUI recipe anchor missing')
m = m.replace(old_dep, new_dep, 1)
mk.write_text(m, encoding='utf-8')

print('LodePNG printer icon patch staged')
