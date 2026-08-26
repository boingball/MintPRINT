/*
 * MintPRINT alphanumeric/PRT: support.
 *
 * printer.device sends normal AmigaDOS PRT: character output through the
 * driver's ped_ConvFunc hook.  MintPRINT has no parallel/serial transport of
 * its own (the V44 driver deliberately declares PRTA_NoIO), so capture those
 * characters here and turn them into ordinary raster pages at DriverClose.
 * The pages then use exactly the same configured IPP document family as the
 * graphics path: JPEG, PWG Raster, PDF or PostScript.
 *
 * The command table remains deliberately conservative for the first text
 * implementation.  Plain characters, CR/LF, TAB, wrapping and form feed are
 * supported; most printer escape-language styling is still ignored.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/printer.h>
#include <devices/prtbase.h>
#include <intuition/preferences.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <proto/exec.h>
#include <proto/graphics.h>

#include "config.h"
#include "jpeg_writer.h"
#include "pwg_writer.h"
#include "pdf_writer.h"
#include "postscript_writer.h"
#include "ipp_client.h"
#include "media_size.h"
#include "spool.h"

#define MP_TEXT_BLOCK_BYTES 1024UL
#define MP_TEXT_MAX_COLUMNS 136UL
#define MP_TEXT_TAB_COLUMNS 8UL

#define MP_TEXT_FILE_JPEG ((CONST_STRPTR)"T:MintPRINT-text.jpg")
#define MP_TEXT_FILE_PWG  ((CONST_STRPTR)"T:MintPRINT-text.pwg")
#define MP_TEXT_FILE_PDF  ((CONST_STRPTR)"T:MintPRINT-text.pdf")
#define MP_TEXT_FILE_PS   ((CONST_STRPTR)"T:MintPRINT-text.ps")

enum {
    MP_TEXT_ENGINE_JPEG = 0,
    MP_TEXT_ENGINE_PWG = 1,
    MP_TEXT_ENGINE_PDF = 2,
    MP_TEXT_ENGINE_POSTSCRIPT = 3
};

struct MPTextBlock {
    struct MPTextBlock *next;
    ULONG used;
    UBYTE data[MP_TEXT_BLOCK_BYTES];
};

struct MPTextCursor {
    struct MPTextBlock *block;
    ULONG offset;
};

extern struct ExecBase *SysBase;
extern struct PrinterData *PD;
extern int PRT_STDARGS DriverOpen(struct IORequest *ior);
extern VOID PRT_STDARGS DriverClose(struct IORequest *ior);
extern LONG PRT_STDARGS DoSpecial(UWORD *command, UBYTE output_buffer[],
                                  BYTE *current_line_position,
                                  BYTE *current_line_spacing,
                                  BYTE *crlf_flag, STRPTR params);
extern VOID MintPRINTNoteVerticalAdvance(ULONG vmi_216ths);
extern VOID MintPRINTResetVerticalAdvances(void);

/* Required by proto/graphics.h library stubs.  Text output opens the library
 * only while DriverClose is rasterising captured PRT: data. */
struct GfxBase *GfxBase = NULL;

static struct MPTextBlock *g_text_head = NULL;
static struct MPTextBlock *g_text_tail = NULL;
static BOOL g_text_seen = FALSE;
static BOOL g_text_capture_failed = FALSE;
static BOOL g_text_last_was_cr = FALSE;

static struct MPConfig g_text_config;
static MPJpegEncoder g_text_jpeg;
static MPPwgEncoder g_text_pwg;
static MPPdfEncoder g_text_pdf;
static MPPostScriptEncoder g_text_postscript;

static char unsupported[] = "\377";

char *CommandTable[77] = {
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported, unsupported, unsupported, unsupported,
    unsupported, unsupported
};

static void mp_text_log(const char *text)
{
    if (g_text_config.debug)
        mp_spool_log(text);
}

static void mp_text_free_capture(void)
{
    struct MPTextBlock *block = g_text_head;

    while (block) {
        struct MPTextBlock *next = block->next;
        FreeMem(block, (ULONG)sizeof(*block));
        block = next;
    }

    g_text_head = NULL;
    g_text_tail = NULL;
    g_text_seen = FALSE;
    g_text_capture_failed = FALSE;
    g_text_last_was_cr = FALSE;
}

static BOOL mp_text_append(UBYTE c)
{
    struct MPTextBlock *block;

    if (!g_text_tail || g_text_tail->used == MP_TEXT_BLOCK_BYTES) {
        block = (struct MPTextBlock *)AllocMem((ULONG)sizeof(*block),
                                               MEMF_PUBLIC | MEMF_CLEAR);
        if (!block) {
            g_text_capture_failed = TRUE;
            return FALSE;
        }
        if (g_text_tail)
            g_text_tail->next = block;
        else
            g_text_head = block;
        g_text_tail = block;
    }

    g_text_tail->data[g_text_tail->used++] = c;
    return TRUE;
}

/*
 * Character conversion hook for printer.device CMD_WRITE/PRT: traffic.
 * Returning zero means "do not add this character to printer.device's
 * primitive output buffer".  That is exactly what MintPRINT wants: retain the
 * character for IPP rasterisation instead of leaking it to parallel.device or
 * serial.device (especially important for the pre-V44/OS3.1 driver, which
 * cannot declare PRTA_NoIO).
 *
 * ESC/CSI/0xff are left to printer.device's command parser so standard Amiga
 * printer control sequences can still reach TextDoSpecial().
 */
LONG PRT_STDARGS ConvFunc(UBYTE *buf, UBYTE c, LONG crlf_flag)
{
    (void)buf;
    (void)crlf_flag;

    if (c == 0x1b || c == 0x9b || c == 0xff)
        return -1;

    if (g_text_capture_failed)
        return 0;

    if (c == '\r') {
        if (!mp_text_append('\n'))
            return 0;
        g_text_last_was_cr = TRUE;
        return 0;
    }

    if (c == '\n') {
        MintPRINTNoteVerticalAdvance(0);
        if (g_text_last_was_cr) {
            g_text_last_was_cr = FALSE;
            return 0;
        }
        mp_text_append('\n');
        return 0;
    }

    g_text_last_was_cr = FALSE;

    if (c == '\f')
        MintPRINTResetVerticalAdvances();

    if (c == '\f' || c == '\t' || (c >= 0x20 && c != 0x7f)) {
        if (mp_text_append(c) && c != '\f' && c != '\t' && c != ' ')
            g_text_seen = TRUE;
    }

    return 0;
}

/* printer.device can express line movement as the standard aIND/aNEL
 * commands (ESC D / ESC E) instead of literal LF/CRLF bytes.  Preserve those
 * two layout operations in our captured text stream; all other command-table
 * operations still use the existing graphics-era DoSpecial no-op for now. */
LONG PRT_STDARGS TextDoSpecial(UWORD *command, UBYTE output_buffer[],
                               BYTE *current_line_position,
                               BYTE *current_line_spacing,
                               BYTE *crlf_flag, STRPTR params)
{
    if (command && (*command == aIND || *command == aNEL)) {
        ULONG vmi = current_line_spacing ?
                    (ULONG)(UBYTE)*current_line_spacing : 0UL;
        MintPRINTNoteVerticalAdvance(vmi);
        g_text_last_was_cr = FALSE;
        if (!g_text_capture_failed)
            mp_text_append('\n');
        return 0;
    }

    return DoSpecial(command, output_buffer, current_line_position,
                     current_line_spacing, crlf_flag, params);
}

static BOOL mp_text_next(struct MPTextCursor *cursor, UBYTE *out)
{
    while (cursor->block) {
        if (cursor->offset < cursor->block->used) {
            *out = cursor->block->data[cursor->offset++];
            return TRUE;
        }
        cursor->block = cursor->block->next;
        cursor->offset = 0;
    }
    return FALSE;
}

static BOOL mp_text_has_more(const struct MPTextCursor *cursor)
{
    struct MPTextBlock *block = cursor->block;
    ULONG offset = cursor->offset;

    while (block) {
        if (offset < block->used)
            return TRUE;
        block = block->next;
        offset = 0;
    }
    return FALSE;
}

static int mp_text_engine(const struct MPConfig *cfg)
{
    if (cfg->engine[0] == 'p' && cfg->engine[1] == 'w')
        return MP_TEXT_ENGINE_PWG;
    if (cfg->engine[0] == 'p' && cfg->engine[1] == 'd')
        return MP_TEXT_ENGINE_PDF;
    if (cfg->engine[0] == 'p' && cfg->engine[1] == 'o')
        return MP_TEXT_ENGINE_POSTSCRIPT;
    return MP_TEXT_ENGINE_JPEG;
}

static CONST_STRPTR mp_text_filename(int engine)
{
    switch (engine) {
        case MP_TEXT_ENGINE_PWG: return MP_TEXT_FILE_PWG;
        case MP_TEXT_ENGINE_PDF: return MP_TEXT_FILE_PDF;
        case MP_TEXT_ENGINE_POSTSCRIPT: return MP_TEXT_FILE_PS;
        default: return MP_TEXT_FILE_JPEG;
    }
}

static CONST_STRPTR mp_text_format(int engine)
{
    switch (engine) {
        case MP_TEXT_ENGINE_PWG: return (CONST_STRPTR)"image/pwg-raster";
        case MP_TEXT_ENGINE_PDF: return (CONST_STRPTR)"application/pdf";
        case MP_TEXT_ENGINE_POSTSCRIPT:
            return (CONST_STRPTR)"application/postscript";
        default: return (CONST_STRPTR)"image/jpeg";
    }
}

static long mp_text_file_write(void *ctx, const unsigned char *data,
                               unsigned long length)
{
    (void)ctx;
    if (!mp_spool_job_write((const UBYTE *)data, (ULONG)length))
        return -1;
    return (long)length;
}

static ULONG mp_text_page_width(ULONG dpi, ULONG *height_out)
{
    ULONG x100;
    ULONG y100;
    ULONG width;
    ULONG height;

    if (!mp_media_dimensions_100mm(g_text_config.media, &x100, &y100)) {
        x100 = 21000UL;
        y100 = 29700UL;
    }

    width = (x100 * dpi + 1270UL) / 2540UL;
    height = (y100 * dpi + 1270UL) / 2540UL;

    if (!width || !height || width > 65535UL || height > 65535UL) {
        width = (21000UL * dpi + 1270UL) / 2540UL;
        height = (29700UL * dpi + 1270UL) / 2540UL;
    }

    *height_out = height;
    return width;
}

static ULONG mp_text_cpi(void)
{
    if (!PD)
        return 10UL;
    if (PD->pd_Preferences.PrintPitch == ELITE)
        return 12UL;
    if (PD->pd_Preferences.PrintPitch == FINE)
        return 15UL;
    return 10UL;
}

static ULONG mp_text_lpi(void)
{
    if (PD && PD->pd_Preferences.PrintSpacing == EIGHT_LPI)
        return 8UL;
    return 6UL;
}

static ULONG mp_text_parse_page(struct MPTextCursor *cursor,
                                UBYTE *lines, UBYTE *lengths,
                                ULONG max_lines, ULONG max_columns,
                                BOOL *form_feed)
{
    ULONG line = 0;
    ULONG col = 0;
    UBYTE c;

    *form_feed = FALSE;

    while (line < max_lines && mp_text_next(cursor, &c)) {
        UBYTE *dst = lines + line * (max_columns + 1UL);

        if (c == '\f') {
            if (col) {
                lengths[line] = (UBYTE)col;
                ++line;
            }
            *form_feed = TRUE;
            break;
        }

        if (c == '\n') {
            lengths[line] = (UBYTE)col;
            ++line;
            col = 0;
            continue;
        }

        if (c == '\t') {
            ULONG next_tab = ((col / MP_TEXT_TAB_COLUMNS) + 1UL) *
                             MP_TEXT_TAB_COLUMNS;
            if (next_tab > max_columns)
                next_tab = max_columns;
            while (col < next_tab)
                dst[col++] = ' ';
        } else if (c >= 0x20 && c != 0x7f) {
            dst[col++] = c;
        }

        if (col >= max_columns) {
            lengths[line] = (UBYTE)col;
            ++line;
            col = 0;
        }
    }

    if (line < max_lines && col) {
        lengths[line] = (UBYTE)col;
        ++line;
    }

    return line;
}

static BOOL mp_text_bit(struct BitMap *bitmap, ULONG x, ULONG y)
{
    UBYTE *row;

    row = (UBYTE *)bitmap->Planes[0] + y * (ULONG)bitmap->BytesPerRow;
    return (row[x >> 3] & (UBYTE)(0x80U >> (x & 7UL))) ? TRUE : FALSE;
}

static BOOL mp_text_begin_encoder(int engine, ULONG width, ULONG height,
                                  UBYTE *scratch, ULONG scratch_bytes)
{
    ULONG page_x = 0;
    ULONG page_y = 0;
    ULONG dpi = g_text_config.resolution ? g_text_config.resolution : 300UL;

    switch (engine) {
        case MP_TEXT_ENGINE_PWG:
            return mp_pwg_begin_page(&g_text_pwg, width, height, 0, 0, dpi,
                                     TRUE, FALSE, FALSE, 1, 1,
                                     scratch, scratch_bytes,
                                     mp_text_file_write, NULL) ? TRUE : FALSE;
        case MP_TEXT_ENGINE_PDF:
            return mp_pdf_begin(&g_text_pdf, width, height, dpi,
                                scratch, scratch_bytes,
                                mp_text_file_write, NULL) ? TRUE : FALSE;
        case MP_TEXT_ENGINE_POSTSCRIPT:
            mp_media_page_points(g_text_config.media, width, height,
                                 &page_x, &page_y);
            mp_postscript_set_scaling(g_text_config.scaling);
            return mp_postscript_begin(&g_text_postscript, width, height,
                                       page_x, page_y, dpi,
                                       scratch, scratch_bytes,
                                       mp_text_file_write, NULL) ? TRUE : FALSE;
        default:
            return mp_jpeg_begin(&g_text_jpeg, width, height,
                                 scratch, scratch_bytes,
                                 mp_text_file_write, NULL) ? TRUE : FALSE;
    }
}

static BOOL mp_text_write_scanline(int engine, const UBYTE *rgb)
{
    switch (engine) {
        case MP_TEXT_ENGINE_PWG:
            return mp_pwg_write_scanline(&g_text_pwg, rgb) ? TRUE : FALSE;
        case MP_TEXT_ENGINE_PDF:
            return mp_pdf_write_scanline(&g_text_pdf, rgb) ? TRUE : FALSE;
        case MP_TEXT_ENGINE_POSTSCRIPT:
            return mp_postscript_write_scanline(&g_text_postscript, rgb) ? TRUE : FALSE;
        default:
            return mp_jpeg_write_scanline(&g_text_jpeg, rgb) ? TRUE : FALSE;
    }
}

static BOOL mp_text_finish_encoder(int engine)
{
    switch (engine) {
        case MP_TEXT_ENGINE_PWG:
            return mp_pwg_finish(&g_text_pwg) ? TRUE : FALSE;
        case MP_TEXT_ENGINE_PDF:
            return mp_pdf_finish(&g_text_pdf) ? TRUE : FALSE;
        case MP_TEXT_ENGINE_POSTSCRIPT:
            return mp_postscript_finish(&g_text_postscript) ? TRUE : FALSE;
        default:
            return mp_jpeg_finish(&g_text_jpeg) ? TRUE : FALSE;
    }
}

static ULONG mp_text_scratch_size(int engine, ULONG width)
{
    switch (engine) {
        case MP_TEXT_ENGINE_PWG: return mp_pwg_scratch_size(width);
        case MP_TEXT_ENGINE_PDF: return mp_pdf_scratch_size(width);
        case MP_TEXT_ENGINE_POSTSCRIPT: return mp_postscript_scratch_size(width);
        default: return mp_jpeg_scratch_size(width);
    }
}

static BOOL mp_text_print_page(int engine, ULONG width, ULONG height,
                               ULONG dpi, ULONG left_px, ULONG cell_width,
                               ULONG line_height, ULONG glyph_height,
                               ULONG max_columns, UBYTE *lines,
                               UBYTE *lengths, ULONG line_count)
{
    struct BitMap bitmap;
    struct RastPort rp;
    struct TextFont *font;
    struct MPConfig submit_config;
    struct MPIPPResult result;
    PLANEPTR plane = NULL;
    UBYTE *rgb = NULL;
    UBYTE *scratch = NULL;
    ULONG rgb_bytes;
    ULONG scratch_bytes;
    ULONG source_width;
    ULONG source_height;
    ULONG top_margin;
    ULONG y;
    LONG ipp_rc;
    BOOL ok = FALSE;
    CONST_STRPTR filename = mp_text_filename(engine);

    if (!GfxBase || !GfxBase->DefaultFont || !width || !height)
        return FALSE;

    font = GfxBase->DefaultFont;
    source_height = (ULONG)font->tf_YSize;
    source_width = max_columns * (ULONG)font->tf_XSize;
    if (!source_width || !source_height)
        return FALSE;

    InitBitMap(&bitmap, 1, (LONG)source_width, (LONG)source_height);
    plane = AllocRaster((ULONG)source_width, (ULONG)source_height);
    if (!plane)
        goto done;
    bitmap.Planes[0] = plane;

    InitRastPort(&rp);
    rp.BitMap = &bitmap;
    SetFont(&rp, font);
    SetAPen(&rp, 1);
    SetBPen(&rp, 0);
    SetDrMd(&rp, JAM1);

    rgb_bytes = width * 3UL;
    rgb = (UBYTE *)AllocMem(rgb_bytes, MEMF_PUBLIC);
    if (!rgb)
        goto done;

    scratch_bytes = mp_text_scratch_size(engine, width);
    if (!scratch_bytes)
        goto done;
    scratch = (UBYTE *)AllocMem(scratch_bytes, MEMF_PUBLIC);
    if (!scratch)
        goto done;

    if (!mp_spool_job_open(filename))
        goto done;

    if (!mp_text_begin_encoder(engine, width, height, scratch, scratch_bytes)) {
        mp_spool_job_close();
        if (!g_text_config.debug)
            mp_spool_job_delete(filename);
        goto done;
    }

    top_margin = dpi / 4UL;

    for (y = 0; y < height; ++y) {
        ULONG i;
        LONG rel_y = (LONG)y - (LONG)top_margin;

        for (i = 0; i < rgb_bytes; ++i)
            rgb[i] = 255;

        if (rel_y >= 0 && line_height &&
            (ULONG)rel_y / line_height < line_count) {
            ULONG line_index = (ULONG)rel_y / line_height;
            ULONG inside = (ULONG)rel_y % line_height;
            ULONG glyph_top = (line_height > glyph_height) ?
                              (line_height - glyph_height) / 2UL : 0;

            if (inside == glyph_top) {
                UBYTE *line = lines + line_index * (max_columns + 1UL);
                SetRast(&rp, 0);
                if (lengths[line_index]) {
                    Move(&rp, 0, (LONG)font->tf_Baseline);
                    Text(&rp, (CONST_STRPTR)line, (ULONG)lengths[line_index]);
                    WaitBlit();
                }
            }

            if (inside >= glyph_top && inside < glyph_top + glyph_height &&
                lengths[line_index]) {
                ULONG source_y = ((inside - glyph_top) * source_height) /
                                 glyph_height;
                ULONG char_index;
                ULONG glyph_width = (cell_width * 3UL) / 4UL;
                ULONG glyph_xoff;

                if (!glyph_width)
                    glyph_width = 1;
                glyph_xoff = (cell_width > glyph_width) ?
                             (cell_width - glyph_width) / 2UL : 0;

                for (char_index = 0;
                     char_index < (ULONG)lengths[line_index];
                     ++char_index) {
                    ULONG gx;
                    ULONG out_base = left_px + char_index * cell_width;
                    ULONG src_base = char_index * (ULONG)font->tf_XSize;

                    if (out_base >= width)
                        break;

                    for (gx = 0; gx < glyph_width; ++gx) {
                        ULONG out_x = out_base + glyph_xoff + gx;
                        ULONG src_x;
                        ULONG out;

                        if (out_x >= width)
                            break;
                        src_x = src_base +
                                (gx * (ULONG)font->tf_XSize) / glyph_width;
                        if (src_x < source_width &&
                            mp_text_bit(&bitmap, src_x, source_y)) {
                            out = out_x * 3UL;
                            rgb[out] = 0;
                            rgb[out + 1UL] = 0;
                            rgb[out + 2UL] = 0;
                        }
                    }
                }
            }
        }

        if (!mp_text_write_scanline(engine, rgb)) {
            mp_text_log("MintPRINT: PRT text encoder row failed");
            mp_spool_job_close();
            if (!g_text_config.debug)
                mp_spool_job_delete(filename);
            goto done;
        }
    }

    if (!mp_text_finish_encoder(engine)) {
        mp_text_log("MintPRINT: PRT text encoder finish failed");
        mp_spool_job_close();
        if (!g_text_config.debug)
            mp_spool_job_delete(filename);
        goto done;
    }

    mp_spool_job_close();

    submit_config = g_text_config;
    /* The first PRT implementation submits one IPP job per text page.  Do not
     * advertise duplex on those independent one-page jobs; doing so would
     * create a blank reverse side on many printers.  Multi-page PWG duplex can
     * be added once the basic PRT path has real-hardware coverage. */
    submit_config.sides[0] = 0;

    ipp_rc = mp_spool_ipp_submit(&submit_config, filename,
                                 mp_text_format(engine), &result);
    if (ipp_rc != 0) {
        mp_text_log("MintPRINT: PRT text IPP submission failed");
        goto submitted;
    }

    mp_text_log("MintPRINT: PRT text page submitted");
    ok = TRUE;

submitted:
    if (!g_text_config.debug)
        mp_spool_job_delete(filename);

done:
    if (scratch)
        FreeMem(scratch, scratch_bytes);
    if (rgb)
        FreeMem(rgb, rgb_bytes);
    if (plane)
        FreeRaster(plane, (ULONG)source_width, (ULONG)source_height);
    return ok;
}

static BOOL mp_text_print_document(void)
{
    struct MPTextCursor cursor;
    ULONG dpi;
    ULONG width;
    ULONG height;
    ULONG cpi;
    ULONG lpi;
    ULONG cell_width;
    ULONG line_height;
    ULONG glyph_height;
    ULONG left_column;
    ULONG right_column;
    ULONG left_px;
    ULONG max_columns;
    ULONG max_lines;
    ULONG line_stride;
    UBYTE *lines = NULL;
    UBYTE *lengths = NULL;
    int engine;
    BOOL all_ok = TRUE;

    if (!g_text_seen || !g_text_head)
        return TRUE;

    dpi = g_text_config.resolution ? g_text_config.resolution : 300UL;
    width = mp_text_page_width(dpi, &height);
    cpi = mp_text_cpi();
    lpi = mp_text_lpi();
    cell_width = (dpi + cpi / 2UL) / cpi;
    line_height = (dpi + lpi / 2UL) / lpi;
    glyph_height = (line_height * 4UL) / 5UL;
    if (!cell_width) cell_width = 1;
    if (!line_height) line_height = 1;
    if (!glyph_height) glyph_height = 1;

    left_column = (PD && PD->pd_Preferences.PrintLeftMargin) ?
                  (ULONG)PD->pd_Preferences.PrintLeftMargin : 1UL;
    right_column = (PD && PD->pd_Preferences.PrintRightMargin) ?
                   (ULONG)PD->pd_Preferences.PrintRightMargin : 80UL;
    if (left_column < 1UL)
        left_column = 1UL;
    if (right_column < left_column)
        right_column = left_column + 79UL;
    if (right_column > MP_TEXT_MAX_COLUMNS)
        right_column = MP_TEXT_MAX_COLUMNS;

    left_px = (left_column - 1UL) * cell_width;
    if (left_px >= width)
        left_px = dpi / 4UL;

    max_columns = right_column - left_column + 1UL;
    if (left_px + max_columns * cell_width > width)
        max_columns = (width - left_px) / cell_width;
    if (!max_columns)
        max_columns = 1UL;
    if (max_columns > MP_TEXT_MAX_COLUMNS)
        max_columns = MP_TEXT_MAX_COLUMNS;

    if (height > dpi / 2UL)
        max_lines = (height - dpi / 2UL) / line_height;
    else
        max_lines = height / line_height;
    if (!max_lines)
        max_lines = 1UL;
    if (max_lines > 255UL)
        max_lines = 255UL;

    line_stride = max_columns + 1UL;
    lines = (UBYTE *)AllocMem(max_lines * line_stride,
                              MEMF_PUBLIC | MEMF_CLEAR);
    lengths = (UBYTE *)AllocMem(max_lines, MEMF_PUBLIC | MEMF_CLEAR);
    if (!lines || !lengths) {
        mp_text_log("MintPRINT: PRT text page buffer allocation failed");
        all_ok = FALSE;
        goto done;
    }

    engine = mp_text_engine(&g_text_config);
    if (g_text_config.sides[0] == 't')
        mp_text_log("MintPRINT: PRT text beta uses one-sided page jobs");

    cursor.block = g_text_head;
    cursor.offset = 0;

    while (mp_text_has_more(&cursor)) {
        ULONG line_count;
        ULONG i;
        BOOL form_feed;

        for (i = 0; i < max_lines * line_stride; ++i)
            lines[i] = 0;
        for (i = 0; i < max_lines; ++i)
            lengths[i] = 0;

        line_count = mp_text_parse_page(&cursor, lines, lengths,
                                        max_lines, max_columns, &form_feed);

        if (!line_count && !form_feed)
            break;

        if (!mp_text_print_page(engine, width, height, dpi, left_px,
                                cell_width, line_height, glyph_height,
                                max_columns, lines, lengths, line_count)) {
            all_ok = FALSE;
            break;
        }
    }

done:
    if (lengths)
        FreeMem(lengths, max_lines);
    if (lines)
        FreeMem(lines, max_lines * line_stride);
    return all_ok;
}

/* These wrappers let one driver support both printer.device graphics dumps and
 * PRT:/CMD_WRITE text.  Graphics state remains owned by driver_core.c. */
int PRT_STDARGS TextDriverOpen(struct IORequest *ior)
{
    mp_text_free_capture();
    return DriverOpen(ior);
}

VOID PRT_STDARGS TextDriverClose(struct IORequest *ior)
{
    /* Finish any graphics page first so the spool process has no open job file
     * when text starts its own encoder/submission sequence. */
    DriverClose(ior);

    mp_config_defaults(&g_text_config);
    mp_spool_config_load(&g_text_config);

    if (g_text_capture_failed) {
        mp_text_log("MintPRINT: PRT text capture ran out of memory");
    } else if (g_text_seen) {
        GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
        if (!GfxBase) {
            mp_text_log("MintPRINT: PRT text could not open graphics.library");
        } else {
            mp_text_log("MintPRINT: PRT text render begin");
            if (!mp_text_print_document())
                mp_text_log("MintPRINT: PRT text render failed");
            CloseLibrary((struct Library *)GfxBase);
            GfxBase = NULL;
        }
    }

    mp_text_free_capture();
}
