from pathlib import Path

p = Path('src/MintPrintSettings.c')
s = p.read_text(encoding='utf-8')


def once(old, new, label):
    global s
    if old not in s:
        raise SystemExit(label + ' anchor not found')
    s = s.replace(old, new, 1)

once(
    'static struct BitMap *mp_printer_icon_bitmap = NULL;\n',
    'static struct BitMap *mp_printer_icon_bitmap = NULL;\n'
    'static struct BitMap *mp_printer_icon_mask_bitmap = NULL;\n',
    'mask bitmap global')

once(
    '''static void mp_clear_printer_icon(void) {\n    if (mp_printer_icon_bitmap) {\n        FreeBitMap(mp_printer_icon_bitmap);\n        mp_printer_icon_bitmap = NULL;\n    }\n    DeleteFile((CONST_STRPTR)MP_PRINTER_ICON_TEMP);\n}\n''',
    '''static void mp_clear_printer_icon(void) {\n    if (mp_printer_icon_mask_bitmap) {\n        FreeBitMap(mp_printer_icon_mask_bitmap);\n        mp_printer_icon_mask_bitmap = NULL;\n    }\n    if (mp_printer_icon_bitmap) {\n        FreeBitMap(mp_printer_icon_bitmap);\n        mp_printer_icon_bitmap = NULL;\n    }\n    DeleteFile((CONST_STRPTR)MP_PRINTER_ICON_TEMP);\n}\n''',
    'clear mask')

once(
    '''    struct BitMap *src_bitmap = NULL;\n    struct BitMapHeader *bmhd = NULL;\n    struct BitScaleArgs bsa;\n    ULONG depth;\n''',
    '''    struct BitMap *src_bitmap = NULL;\n    struct BitMapHeader *bmhd = NULL;\n    PLANEPTR src_mask = NULL;\n    struct BitMap native_mask_bitmap;\n    struct BitMap *mask_source_bitmap = NULL;\n    struct BitMap *fallback_mask_bitmap = NULL;\n    struct BitScaleArgs bsa;\n    ULONG depth;\n''',
    'load locals')

old_get = '''    if (!DoMethod(dto, DTM_PROCLAYOUT, NULL, TRUE) ||\n        !GetDTAttrs(dto,\n                    PDTA_DestBitMap, (ULONG)&src_bitmap,\n                    PDTA_BitMapHeader, (ULONG)&bmhd,\n                    TAG_DONE) ||\n        !src_bitmap || !bmhd || bmhd->bmh_Width == 0 || bmhd->bmh_Height == 0) {\n        DisposeDTObject(dto);\n        return FALSE;\n    }\n'''
new_get = '''    if (!DoMethod(dto, DTM_PROCLAYOUT, NULL, TRUE) ||\n        !GetDTAttrs(dto,\n                    PDTA_DestBitMap, (ULONG)&src_bitmap,\n                    PDTA_BitMapHeader, (ULONG)&bmhd,\n                    TAG_DONE) ||\n        !src_bitmap || !bmhd || bmhd->bmh_Width == 0 || bmhd->bmh_Height == 0) {\n        DisposeDTObject(dto);\n        return FALSE;\n    }\n\n#ifdef PDTA_MaskPlane\n    /* picture.datatype V43+ exposes a one-bit mask suitable for\n     * BltMaskBitMapRastPort().  Keep it while the datatype object lives,\n     * scale it alongside the colour bitmap, then use the scaled copy for\n     * the actual GUI blit. */\n    if (DataTypesBase->lib_Version >= 43) {\n        GetDTAttrs(dto, PDTA_MaskPlane, (ULONG)&src_mask, TAG_DONE);\n    }\n#endif\n'''
once(old_get, new_get, 'datatype attrs')

old_scale_tail = '''    bsa.bsa_Flags = 0;\n    BitMapScale(&bsa);\n    WaitBlit();\n\n    DisposeDTObject(dto);\n    return TRUE;\n}\n'''
new_scale_tail = '''    bsa.bsa_Flags = 0;\n    BitMapScale(&bsa);\n    WaitBlit();\n\n    /* Prefer the datatype's real transparency mask.  Some PNG datatypes\n     * report alpha/transparency in the BitMapHeader but do not expose\n     * PDTA_MaskPlane; for those, build a conservative fallback mask by\n     * treating the top-left decoded pixel as the transparent background.\n     * That fallback is only used when the source explicitly reports some\n     * form of masking, so normal opaque printer artwork is left alone. */\n    if (src_mask) {\n        InitBitMap(&native_mask_bitmap, 1, src_w, src_h);\n        native_mask_bitmap.Planes[0] = src_mask;\n        mask_source_bitmap = &native_mask_bitmap;\n    } else if (bmhd->bmh_Masking != 0) {\n        struct RastPort src_rp;\n        struct RastPort mask_rp;\n        LONG transparent_pixel;\n        int x;\n        int y;\n\n        fallback_mask_bitmap = AllocBitMap(src_w, src_h, 1, BMF_CLEAR, NULL);\n        if (fallback_mask_bitmap) {\n            InitRastPort(&src_rp);\n            src_rp.BitMap = src_bitmap;\n            InitRastPort(&mask_rp);\n            mask_rp.BitMap = fallback_mask_bitmap;\n            SetAPen(&mask_rp, 1);\n\n            transparent_pixel = ReadPixel(&src_rp, 0, 0);\n            for (y = 0; y < src_h; ++y) {\n                for (x = 0; x < src_w; ++x) {\n                    if (ReadPixel(&src_rp, x, y) != transparent_pixel)\n                        WritePixel(&mask_rp, x, y);\n                }\n            }\n            mask_source_bitmap = fallback_mask_bitmap;\n        }\n    }\n\n    if (mask_source_bitmap) {\n        mp_printer_icon_mask_bitmap = AllocBitMap(MP_PRINTER_ICON_SIZE,\n                                                  MP_PRINTER_ICON_SIZE,\n                                                  1, BMF_CLEAR, NULL);\n        if (mp_printer_icon_mask_bitmap) {\n            memset(&bsa, 0, sizeof(bsa));\n            bsa.bsa_SrcX = 0;\n            bsa.bsa_SrcY = 0;\n            bsa.bsa_SrcWidth = src_w;\n            bsa.bsa_SrcHeight = src_h;\n            bsa.bsa_XSrcFactor = src_w;\n            bsa.bsa_XDestFactor = draw_w;\n            bsa.bsa_YSrcFactor = src_h;\n            bsa.bsa_YDestFactor = draw_h;\n            bsa.bsa_DestX = (MP_PRINTER_ICON_SIZE - draw_w) / 2;\n            bsa.bsa_DestY = (MP_PRINTER_ICON_SIZE - draw_h) / 2;\n            bsa.bsa_SrcBitMap = mask_source_bitmap;\n            bsa.bsa_DestBitMap = mp_printer_icon_mask_bitmap;\n            bsa.bsa_Flags = 0;\n            BitMapScale(&bsa);\n            WaitBlit();\n        }\n    }\n\n    if (fallback_mask_bitmap)\n        FreeBitMap(fallback_mask_bitmap);\n\n    DisposeDTObject(dto);\n    return TRUE;\n}\n'''
once(old_scale_tail, new_scale_tail, 'scaled mask')

old_draw = '''    if (mp_printer_icon_bitmap) {\n        BltBitMapRastPort(mp_printer_icon_bitmap, 0, 0,\n                          rp, left, top,\n                          MP_PRINTER_ICON_SIZE, MP_PRINTER_ICON_SIZE,\n                          0xC0);\n    }\n}\n'''
new_draw = '''    if (mp_printer_icon_bitmap) {\n        if (mp_printer_icon_mask_bitmap &&\n            mp_printer_icon_mask_bitmap->Planes[0]) {\n            BltMaskBitMapRastPort(mp_printer_icon_bitmap, 0, 0,\n                                  rp, left, top,\n                                  MP_PRINTER_ICON_SIZE, MP_PRINTER_ICON_SIZE,\n                                  0xC0,\n                                  mp_printer_icon_mask_bitmap->Planes[0]);\n        } else {\n            BltBitMapRastPort(mp_printer_icon_bitmap, 0, 0,\n                              rp, left, top,\n                              MP_PRINTER_ICON_SIZE, MP_PRINTER_ICON_SIZE,\n                              0xC0);\n        }\n    }\n}\n'''
once(old_draw, new_draw, 'masked draw')

p.write_text(s, encoding='utf-8')
