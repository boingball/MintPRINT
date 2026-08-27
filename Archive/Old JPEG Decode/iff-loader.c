
// iff-loader.c - Fixed version with endian handling, HAM6/8, RLE, CLUT, EHB, 24-bit support, and debug output
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <graphics/modeid.h> // For HAM_KEY and EXTRA_HALFBRITE
#include "iff-loader.h"

#define ID_FORM 0x464F524D  // 'FORM'
#define ID_ILBM 0x494C424D  // 'ILBM'
#define ID_BMHD 0x424D4844  // 'BMHD'
#define ID_CMAP 0x434D4150  // 'CMAP'
#define ID_BODY 0x424F4459  // 'BODY'
#define ID_CAMG 0x43414D47  // 'CAMG'

struct IFFChunk {
    ULONG id;
    ULONG size;
};

struct BMHDChunk {
    UWORD w, h;
    WORD x, y;
    UBYTE nPlanes;
    UBYTE masking;
    UBYTE compression;
    UBYTE pad1;
    UWORD transparentColor;
    UBYTE xAspect, yAspect;
    WORD pageWidth, pageHeight;
};

// These are correct for Amiga (big-endian); no conversion needed on Amiga-side fread
static inline ULONG read_be_ulong(ULONG val) {
    return val;
}

static inline UWORD read_be_uword(UWORD val) {
    return val;
}

static LONG decompress_rle(UBYTE *src, UBYTE *dest, LONG src_len, LONG dest_len) {
    LONG src_pos = 0;
    LONG dest_pos = 0;
    while (src_pos < src_len && dest_pos < dest_len) {
        BYTE n = (BYTE)src[src_pos++];
        if (n >= 0) {
            int count = n + 1;
            while (count-- && src_pos < src_len && dest_pos < dest_len) {
                dest[dest_pos++] = src[src_pos++];
            }
        } else if (n != -128) {
            int count = -n + 1;
            if (src_pos < src_len) {
                BYTE val = src[src_pos++];
                while (count-- && dest_pos < dest_len) {
                    dest[dest_pos++] = val;
                }
            }
        }
    }
    /* ByteRun1 callers need the number of COMPRESSED source bytes
     * consumed, not the number of decoded destination bytes produced.
     * Returning dest_pos made the next row/bitplane start at the wrong
     * BODY offset whenever compression actually saved any bytes. */
    return src_pos;
}

static UWORD get_pixel_from_planes(UBYTE **planes, int plane_count, int bytes_per_row, int x, int y) {
    UWORD result = 0;
    int byte_offset = y * bytes_per_row + (x >> 3);
    int bit = 0x80 >> (x & 7);
    for (int p = 0; p < plane_count; p++) {
        if (planes[p][byte_offset] & bit) result |= (1 << p);
    }
    return result;
}

static void decode_ham_pixels(UBYTE **planes, int plane_count, int bytes_per_row, int width, int height,
                              UBYTE *colormap, int colors, ULONG camg_mode, struct jpeg_data *data, int orig_width, int orig_height) {
    BOOL is_ham8 = (plane_count == 8);
    int data_bits = is_ham8 ? 6 : 4;
    int data_mask = (1 << data_bits) - 1;
    int control_shift = data_bits;
    UBYTE hold_r = 0, hold_g = 0, hold_b = 0;
    for (int y = 0; y < height; y++) {
        hold_r = hold_g = hold_b = 0;
        for (int x = 0; x < width; x++) {
            UWORD pix = get_pixel_from_planes(planes, plane_count, bytes_per_row, x, y);
            int ctrl = (pix >> control_shift) & 0x03;
            int val = pix & data_mask;
            switch (ctrl) {
                case 0:
                    if (val < colors) {
                        hold_r = colormap[val * 3 + 0];
                        hold_g = colormap[val * 3 + 1];
                        hold_b = colormap[val * 3 + 2];
                    } else {
                        hold_r = hold_g = hold_b = 0;
                    }
                    break;
                case 1: hold_b = is_ham8 ? (val << 2) | (val >> 4) : (val << 4) | val; break;
                case 2: hold_r = is_ham8 ? (val << 2) | (val >> 4) : (val << 4) | val; break;
                case 3: hold_g = is_ham8 ? (val << 2) | (val >> 4) : (val << 4) | val; break;
            }
            int idx = y * width + x;
            if (x < orig_width && y < orig_height) {
                data->red[idx] = hold_r;
                data->green[idx] = hold_g;
                data->blue[idx] = hold_b;
            }
        }
    }
}

int load_iff_direct(const char *filename, struct jpeg_data *data) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        return -1;
    }

    ULONG form_id = 0, file_size = 0, type_id = 0;
    fread(&form_id, 4, 1, file);
    fread(&file_size, 4, 1, file);
    fread(&type_id, 4, 1, file);
    form_id = read_be_ulong(form_id);
    type_id = read_be_ulong(type_id);

    if (form_id != ID_FORM || type_id != ID_ILBM) {
        fclose(file);
        return -1;
    }

    struct BMHDChunk bmhd = {0};
    UBYTE *colormap = NULL, *body_data = NULL;
    UBYTE **planes = NULL;
    ULONG camg_mode = 0;
    ULONG body_size = 0;
    int colors = 0;

    struct IFFChunk chunk;
    while (fread(&chunk, sizeof(chunk), 1, file) == 1) {
        ULONG id = read_be_ulong(chunk.id);
        ULONG size = read_be_ulong(chunk.size);
        long chunk_start = ftell(file);

        if (id == ID_BMHD) {
            fread(&bmhd, sizeof(bmhd), 1, file);
            bmhd.w = read_be_uword(bmhd.w);
            bmhd.h = read_be_uword(bmhd.h);
        } else if (id == ID_CMAP) {
            colormap = AllocVec(size, MEMF_ANY);
            fread(colormap, size, 1, file);
            colors = size / 3;
        } else if (id == ID_CAMG) {
            fread(&camg_mode, 4, 1, file);
            camg_mode = read_be_ulong(camg_mode);
        } else if (id == ID_BODY) {
            body_data = AllocVec(size, MEMF_ANY);
            fread(body_data, size, 1, file);
            body_size = size;
        } else {
            fseek(file, size + (size & 1), SEEK_CUR);
        }

        long chunk_end = chunk_start + size;
        fseek(file, chunk_end + (size & 1), SEEK_SET);
    }
    fclose(file);

    if (!body_data || (!colormap && bmhd.nPlanes <= 8)) {
        return -1;
    }

    int ow = bmhd.w, oh = bmhd.h;
    int w = (ow + 15) & ~15, h = (oh + 15) & ~15;
    int bpr = ((ow + 15) / 16) * 2;

    data->width = w;
    data->height = h;
    data->num_pixel = w * h;
    data->red = AllocVec(w * h, MEMF_CLEAR);
    data->green = AllocVec(w * h, MEMF_CLEAR);
    data->blue = AllocVec(w * h, MEMF_CLEAR);

    planes = AllocVec(bmhd.nPlanes * sizeof(UBYTE *), MEMF_CLEAR);
    for (int i = 0; i < bmhd.nPlanes; i++) planes[i] = AllocVec(bpr * oh, MEMF_ANY);

    int src = 0;
    if (bmhd.compression == 1) {
        for (int y = 0; y < oh; y++) {
            for (int p = 0; p < bmhd.nPlanes; p++) {
                int offset = y * bpr;
                int consumed = decompress_rle(body_data + src, planes[p] + offset, body_size - src, bpr);
                if (consumed <= 0 || src + consumed > (int)body_size) {
                    for (int q = 0; q < bmhd.nPlanes; q++)
                        if (planes[q]) FreeVec(planes[q]);
                    if (planes) FreeVec(planes);
                    if (colormap) FreeVec(colormap);
                    if (body_data) FreeVec(body_data);
                    if (data->red) FreeVec(data->red);
                    if (data->green) FreeVec(data->green);
                    if (data->blue) FreeVec(data->blue);
                    memset(data, 0, sizeof(*data));
                    return -1;
                }
                src += consumed;
            }
        }
    } else {
        for (int y = 0; y < oh; y++) {
            for (int p = 0; p < bmhd.nPlanes; p++) {
                memcpy(planes[p] + y * bpr, body_data + src, bpr);
                src += bpr;
            }
        }
    }

    BOOL is_ham = camg_mode & HAM_KEY;
    BOOL is_ehb = camg_mode & EXTRA_HALFBRITE;

    if (is_ham && (bmhd.nPlanes == 6 || bmhd.nPlanes == 8)) {
        decode_ham_pixels(planes, bmhd.nPlanes, bpr, w, h, colormap, colors, camg_mode, data, ow, oh);
    } else if (bmhd.nPlanes == 24) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int idx = y * w + x;
                UBYTE r = 0, g = 0, b = 0;
                for (int bit = 0; bit < 8; bit++) {
                    int mask = 1 << (7 - (x % 8));
                    /* Deep ILBM stores bitplanes least-significant first:
                     * R0..R7, G0..G7, B0..B7. Plane 0 therefore contributes
                     * bit 0, not bit 7. Reversing this produced recognisable
                     * images with wildly wrong colours (notably 24-bit ILBMs
                     * saved by Art Expression). */
                    if (planes[bit][y * bpr + (x / 8)] & mask) r |= 1 << bit;
                    if (planes[bit + 8][y * bpr + (x / 8)] & mask) g |= 1 << bit;
                    if (planes[bit + 16][y * bpr + (x / 8)] & mask) b |= 1 << bit;
                }
                data->red[idx] = r;
                data->green[idx] = g;
                data->blue[idx] = b;
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int idx = y * w + x;
                UWORD pix = get_pixel_from_planes(planes, bmhd.nPlanes, bpr, x, y);
                if (is_ehb && pix >= 32) pix -= 32, data->red[idx] = colormap[pix * 3] >> 1,
                data->green[idx] = colormap[pix * 3 + 1] >> 1,
                data->blue[idx] = colormap[pix * 3 + 2] >> 1;
                else if (pix < colors) {
                    data->red[idx] = colormap[pix * 3];
                    data->green[idx] = colormap[pix * 3 + 1];
                    data->blue[idx] = colormap[pix * 3 + 2];
                }
            }
        }
    }

    for (int i = 0; i < bmhd.nPlanes; i++) if (planes[i]) FreeVec(planes[i]);
    if (planes) FreeVec(planes);
    if (colormap) FreeVec(colormap);
    if (body_data) FreeVec(body_data);

    return 0;
}

void free_jpeg_data(struct jpeg_data *data) {
    if (data->red) FreeVec(data->red);
    if (data->green) FreeVec(data->green);
    if (data->blue) FreeVec(data->blue);
}
