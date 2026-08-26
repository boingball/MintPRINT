from pathlib import Path

p = Path('src/MintPrintSettings.c')
s = p.read_text(encoding='utf-8')


def once(old, new, label):
    global s
    if old not in s:
        raise SystemExit(label + ' anchor not found')
    s = s.replace(old, new, 1)


once('#include <graphics/displayinfo.h>\n',
     '#include <graphics/displayinfo.h>\n#include <graphics/scale.h>\n',
     'scale include')

once('struct Library *GadToolsBase = NULL;\nstruct IntuitionBase *IntuitionBase = NULL;',
     'struct Library *GadToolsBase = NULL;\nstruct Library *DataTypesBase = NULL;\nstruct IntuitionBase *IntuitionBase = NULL;',
     'DataTypesBase')

once('char printer_make_model[128] = "";\n',
     'char printer_make_model[128] = "";\n'
     'char printer_icon_uri[256] = "";\n\n'
     '#define MP_PRINTER_ICON_LEFT  420\n'
     '#define MP_PRINTER_ICON_TOP   117\n'
     '#define MP_PRINTER_ICON_SIZE   32\n'
     '#define MP_PRINTER_ICON_TEMP  "T:MintPRINT-printer-icon.img"\n'
     'static struct BitMap *mp_printer_icon_bitmap = NULL;\n',
     'printer icon globals')

once('static void mp_draw_marker_strips(void);\nstatic void mp_draw_sides_hint(void);',
     'static void mp_draw_marker_strips(void);\n'
     'static void mp_draw_sides_hint(void);\n'
     'static void mp_draw_printer_icon(void);\n'
     'static void mp_clear_printer_icon(void);',
     'draw declarations')

once('static void reload_current_unit(struct Window *win) {\n    mp_cache_clear_capabilities();',
     'static void reload_current_unit(struct Window *win) {\n'
     '    mp_cache_clear_capabilities();\n'
     '    mp_clear_printer_icon();',
     'unit icon clear')

once('    GT_RefreshWindow(win, NULL);\n    mp_draw_marker_strips();\n    mp_draw_sides_hint();\n}',
     '    GT_RefreshWindow(win, NULL);\n'
     '    mp_draw_marker_strips();\n'
     '    mp_draw_sides_hint();\n'
     '    mp_draw_printer_icon();\n}',
     'unit redraw')

once('    strcpy(pwg_sheet_back_value, "normal");\n    printer_make_model[0] = \'\\0\';\n    num_marker_names = 0;',
     '    strcpy(pwg_sheet_back_value, "normal");\n'
     '    printer_make_model[0] = \'\\0\';\n'
     '    printer_icon_uri[0] = \'\\0\';\n'
     '    num_marker_names = 0;',
     'query icon reset')

once('            "jpeg-y-dimension-supported",\n            "marker-names",',
     '            "jpeg-y-dimension-supported",\n'
     '            "printer-icons",\n'
     '            "marker-names",',
     'requested printer-icons')

once('                        printer_make_model[sizeof(printer_make_model) - 1] = \'\\0\';\n'
     '                    } else if (strcmp(name, "marker-names") == 0 &&',
     '                        printer_make_model[sizeof(printer_make_model) - 1] = \'\\0\';\n'
     '                    } else if (strcmp(name, "printer-icons") == 0 &&\n'
     '                               value_tag == 0x45 && printer_icon_uri[0] == \'\\0\') {\n'
     '                        strncpy(printer_icon_uri, value, sizeof(printer_icon_uri) - 1);\n'
     '                        printer_icon_uri[sizeof(printer_icon_uri) - 1] = \'\\0\';\n'
     '                    } else if (strcmp(name, "marker-names") == 0 &&',
     'printer-icons parser')

helper_anchor = '// Updated query_printer_attributes with fixed mapping logic and tray name parsing\n'
if helper_anchor not in s:
    raise SystemExit('query helper insertion anchor not found')

helper = r'''/* ---------------------------------------------------------------------
 * Optional printer picture advertised by IPP's printer-icons attribute.
 *
 * Fetch the first HTTP URI only. picture.datatype performs format decode
 * and remapping (Brother's AirPrint icon is PNG), then graphics.library
 * scales the bitmap into a small 32x32 preview beside the duplex hint.
 * This is deliberately optional: unsupported URI/image format/download
 * failure simply leaves the preview blank.
 * ------------------------------------------------------------------ */
static void mp_clear_printer_icon(void) {
    if (mp_printer_icon_bitmap) {
        FreeBitMap(mp_printer_icon_bitmap);
        mp_printer_icon_bitmap = NULL;
    }
    DeleteFile((CONST_STRPTR)MP_PRINTER_ICON_TEMP);
}

static BOOL mp_fetch_printer_icon_file(const char *uri) {
    const char *authority;
    const char *slash;
    const char *colon;
    char host[96];
    char path[256];
    char request[512];
    char *response;
    int host_len;
    int port = 80;
    int sockfd;
    int total = 0;
    int http_status = 0;
    int body_off = 0;
    int body_len = 0;
    int complete = 0;
    int request_len;
    struct sockaddr_in serv_addr;
    struct timeval timeout;
    BPTR file;

    if (!uri || strncmp(uri, "http://", 7) != 0)
        return FALSE;

    authority = uri + 7;
    slash = strchr(authority, '/');
    if (!slash)
        return FALSE;

    colon = memchr(authority, ':', (size_t)(slash - authority));
    host_len = (int)((colon ? colon : slash) - authority);
    if (host_len <= 0 || host_len >= (int)sizeof(host))
        return FALSE;

    memcpy(host, authority, (size_t)host_len);
    host[host_len] = '\0';

    if (colon) {
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535)
            return FALSE;
    }

    strncpy(path, slash, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((UWORD)port);
    serv_addr.sin_addr.s_addr = inet_addr(host);
    if (serv_addr.sin_addr.s_addr == (ULONG)-1)
        return FALSE;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return FALSE;

    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));

    if (mp_connect_with_timeout(sockfd, &serv_addr, 5) < 0) {
        CloseSocket(sockfd);
        return FALSE;
    }

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: MintPrintSettings/%s\r\n"
             "Accept: image/png,image/jpeg,image/*\r\n"
             "Connection: close\r\n\r\n",
             path, host, MINTPRINT_SETTINGS_VERSION);
    request_len = (int)strlen(request);

    if (safe_send(sockfd, request, request_len) != request_len) {
        CloseSocket(sockfd);
        return FALSE;
    }

    response = AllocVec(MAX_BUFFER, MEMF_ANY);
    if (!response) {
        CloseSocket(sockfd);
        return FALSE;
    }

    while (total < MAX_BUFFER) {
        int got = recv(sockfd, response + total, MAX_BUFFER - total, 0);
        if (got <= 0)
            break;
        total += got;
        complete = mp_http_final_body(response, total, &http_status,
                                      &body_off, &body_len);
        if (complete != 0)
            break;
    }
    CloseSocket(sockfd);

    if (complete == 0)
        complete = mp_http_final_body(response, total, &http_status,
                                      &body_off, &body_len);

    if (complete != 1 || http_status != 200 || body_len <= 0 ||
        body_off < 0 || body_off + body_len > total) {
        FreeVec(response);
        return FALSE;
    }

    file = Open((CONST_STRPTR)MP_PRINTER_ICON_TEMP, MODE_NEWFILE);
    if (!file) {
        FreeVec(response);
        return FALSE;
    }

    if (Write(file, response + body_off, body_len) != body_len) {
        Close(file);
        DeleteFile((CONST_STRPTR)MP_PRINTER_ICON_TEMP);
        FreeVec(response);
        return FALSE;
    }

    Close(file);
    FreeVec(response);
    return TRUE;
}

static BOOL mp_load_printer_icon_bitmap(void) {
    Object *dto;
    struct BitMap *src_bitmap = NULL;
    struct BitMapHeader *bmhd = NULL;
    struct BitScaleArgs bsa;
    ULONG depth;
    int src_w;
    int src_h;
    int draw_w;
    int draw_h;

    if (!screen)
        return FALSE;

    if (!DataTypesBase) {
        DataTypesBase = OpenLibrary("datatypes.library", 39);
        if (!DataTypesBase)
            return FALSE;
    }

    dto = NewDTObject((APTR)MP_PRINTER_ICON_TEMP,
                      DTA_SourceType, DTST_FILE,
                      DTA_GroupID, GID_PICTURE,
                      PDTA_Screen, (ULONG)screen,
                      PDTA_Remap, TRUE,
                      PDTA_FreeSourceBitMap, TRUE,
                      TAG_DONE);
    if (!dto)
        return FALSE;

    if (!DoMethod(dto, DTM_PROCLAYOUT, NULL, TRUE) ||
        !GetDTAttrs(dto,
                    PDTA_DestBitMap, (ULONG)&src_bitmap,
                    PDTA_BitMapHeader, (ULONG)&bmhd,
                    TAG_DONE) ||
        !src_bitmap || !bmhd || bmhd->bmh_Width == 0 || bmhd->bmh_Height == 0) {
        DisposeDTObject(dto);
        return FALSE;
    }

    src_w = bmhd->bmh_Width;
    src_h = bmhd->bmh_Height;
    draw_w = MP_PRINTER_ICON_SIZE;
    draw_h = MP_PRINTER_ICON_SIZE;

    if (src_w > src_h)
        draw_h = (src_h * MP_PRINTER_ICON_SIZE) / src_w;
    else if (src_h > src_w)
        draw_w = (src_w * MP_PRINTER_ICON_SIZE) / src_h;

    if (draw_w < 1) draw_w = 1;
    if (draw_h < 1) draw_h = 1;

    depth = GetBitMapAttr(src_bitmap, BMA_DEPTH);
    mp_printer_icon_bitmap = AllocBitMap(MP_PRINTER_ICON_SIZE,
                                         MP_PRINTER_ICON_SIZE,
                                         depth, BMF_CLEAR, src_bitmap);
    if (!mp_printer_icon_bitmap) {
        DisposeDTObject(dto);
        return FALSE;
    }

    memset(&bsa, 0, sizeof(bsa));
    bsa.bsa_SrcX = 0;
    bsa.bsa_SrcY = 0;
    bsa.bsa_SrcWidth = src_w;
    bsa.bsa_SrcHeight = src_h;
    bsa.bsa_XSrcFactor = src_w;
    bsa.bsa_XDestFactor = draw_w;
    bsa.bsa_YSrcFactor = src_h;
    bsa.bsa_YDestFactor = draw_h;
    bsa.bsa_DestX = (MP_PRINTER_ICON_SIZE - draw_w) / 2;
    bsa.bsa_DestY = (MP_PRINTER_ICON_SIZE - draw_h) / 2;
    bsa.bsa_SrcBitMap = src_bitmap;
    bsa.bsa_DestBitMap = mp_printer_icon_bitmap;
    bsa.bsa_Flags = 0;
    BitMapScale(&bsa);
    WaitBlit();

    DisposeDTObject(dto);
    return TRUE;
}

static void mp_refresh_printer_icon(void) {
    mp_clear_printer_icon();

    if (!printer_icon_uri[0])
        return;

    if (mp_fetch_printer_icon_file(printer_icon_uri))
        mp_load_printer_icon_bitmap();

    DeleteFile((CONST_STRPTR)MP_PRINTER_ICON_TEMP);
}

static void mp_draw_printer_icon(void) {
    struct RastPort *rp;
    int left = MP_PRINTER_ICON_LEFT;
    int top = g_topborder + MP_PRINTER_ICON_TOP;

    if (!window)
        return;

    rp = window->RPort;
    SetDrMd(rp, JAM1);
    SetAPen(rp, 0);
    RectFill(rp, left - 1, top - 1,
             left + MP_PRINTER_ICON_SIZE, top + MP_PRINTER_ICON_SIZE);

    if (mp_printer_icon_bitmap) {
        BltBitMapRastPort(mp_printer_icon_bitmap, 0, 0,
                          rp, left, top,
                          MP_PRINTER_ICON_SIZE, MP_PRINTER_ICON_SIZE,
                          0xC0);
    }
}

'''
s = s.replace(helper_anchor, helper + helper_anchor, 1)

once('    if (!ok) {\n        custom_printf("CLEAR");\n        custom_printf("Scan failed - please try Query again");\n    }\n    /* Redraw either way:',
     '    if (!ok) {\n'
     '        custom_printf("CLEAR");\n'
     '        custom_printf("Scan failed - please try Query again");\n'
     '        mp_clear_printer_icon();\n'
     '    } else {\n'
     '        mp_refresh_printer_icon();\n'
     '    }\n'
     '    /* Redraw either way:',
     'query icon refresh')

once('    mp_draw_marker_strips();\n    mp_draw_sides_hint();\n}\n\nint send_pwg_print_job',
     '    mp_draw_marker_strips();\n'
     '    mp_draw_sides_hint();\n'
     '    mp_draw_printer_icon();\n'
     '}\n\nint send_pwg_print_job',
     'query icon draw')

once('                    redraw_output_box();\n                    mp_draw_marker_strips();\n                    mp_draw_sides_hint();\n                    break;',
     '                    redraw_output_box();\n'
     '                    mp_draw_marker_strips();\n'
     '                    mp_draw_sides_hint();\n'
     '                    mp_draw_printer_icon();\n'
     '                    break;',
     'refresh icon draw')

once('    // Close libraries in reverse order of opening\n    if (SocketBase) {',
     '    // Close libraries in reverse order of opening\n'
     '    mp_clear_printer_icon();\n'
     '    if (DataTypesBase) {\n'
     '        CloseLibrary(DataTypesBase);\n'
     '        DataTypesBase = NULL;\n'
     '    }\n'
     '    if (SocketBase) {',
     'datatype cleanup')

p.write_text(s, encoding='utf-8')

probe = Path('windows_ipp_probe.py')
ps = probe.read_text(encoding='utf-8')
if '    "printer-icons",\n' not in ps:
    anchor = '    "printer-device-id",\n'
    if anchor not in ps:
        raise SystemExit('probe printer-device-id anchor not found')
    ps = ps.replace(anchor, anchor + '    "printer-icons",\n', 1)
probe.write_text(ps, encoding='utf-8')
