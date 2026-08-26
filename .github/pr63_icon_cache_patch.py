from pathlib import Path

p = Path('src/MintPrintSettings.c')
s = p.read_text(encoding='utf-8')

def replace_once(old, new, label):
    global s
    n = s.count(old)
    if n != 1:
        raise SystemExit(f'{label}: expected 1 match, found {n}')
    s = s.replace(old, new, 1)

# 1) Per-Unit processed artwork path beside the existing config/capability cache paths.
old = '''static void unit_cache_path(int idx, BOOL envarc, char *out, int out_size) {
    snprintf(out, out_size, "%s:MintPRINT/Unit%d.cache", envarc ? "ENVARC" : "ENV", idx);
}
'''
new = old + '''
static void unit_icon_cache_path(int idx, BOOL envarc, char *out, int out_size) {
    snprintf(out, out_size, "%s:MintPRINT/Art/Unit%d.mpic",
             envarc ? "ENVARC" : "ENV", idx);
}
'''
replace_once(old, new, 'unit icon cache path')

# 2) Forward declaration so reload_current_unit() can display cached artwork immediately.
old = '''static void mp_draw_printer_icon(void);
static void mp_clear_printer_icon(void);

static void reload_current_unit(struct Window *win) {
    mp_cache_clear_capabilities();
    mp_clear_printer_icon();
'''
new = '''static void mp_draw_printer_icon(void);
static void mp_clear_printer_icon(void);
static BOOL mp_load_printer_icon_cache(BOOL require_uri_match);

static void reload_current_unit(struct Window *win) {
    mp_cache_clear_capabilities();
    mp_clear_printer_icon();
    /* Show the per-Unit processed artwork immediately. The normal startup
     * Query will validate its URI and fetch a replacement only if needed. */
    if (unit_file_exists(current_unit_index))
        mp_load_printer_icon_cache(FALSE);
'''
replace_once(old, new, 'reload current unit icon cache')

# 3) Add processed RGBA cache helpers after the normal in-memory clear routine.
old = '''static void mp_clear_printer_icon(void) {
    mp_printer_icon_valid = FALSE;
    mp_printer_icon_pens_valid = FALSE;
    memset(mp_printer_icon_rgba, 0, sizeof(mp_printer_icon_rgba));
    memset(mp_printer_icon_mask, 0, sizeof(mp_printer_icon_mask));
    DeleteFile((CONST_STRPTR)MP_PRINTER_ICON_TEMP);
}

'''
cache_helpers = r'''static void mp_clear_printer_icon(void) {
    mp_printer_icon_valid = FALSE;
    mp_printer_icon_pens_valid = FALSE;
    memset(mp_printer_icon_rgba, 0, sizeof(mp_printer_icon_rgba));
    memset(mp_printer_icon_mask, 0, sizeof(mp_printer_icon_mask));
    DeleteFile((CONST_STRPTR)MP_PRINTER_ICON_TEMP);
}

/* Processed printer-art cache format.  Keep this deliberately tiny and
 * private to MintPRINT: 8-byte version magic, a fixed 256-byte source URI,
 * then the already-scaled RGBA pixels.  Loading this avoids both the HTTP
 * transfer and a LodePNG decode on subsequent opens. */
static const UBYTE mp_printer_icon_cache_magic[8] = {
    'M', 'P', 'I', 'C', '0', '0', '0', '1'
};

static BOOL mp_write_printer_icon_cache_file(CONST_STRPTR path) {
    BPTR file;
    char cached_uri[sizeof(printer_icon_uri)];
    BOOL ok = TRUE;

    if (!mp_printer_icon_valid || !path)
        return FALSE;

    memset(cached_uri, 0, sizeof(cached_uri));
    strncpy(cached_uri, printer_icon_uri, sizeof(cached_uri) - 1);

    file = Open(path, MODE_NEWFILE);
    if (!file)
        return FALSE;

    if (Write(file, (APTR)mp_printer_icon_cache_magic,
              sizeof(mp_printer_icon_cache_magic)) !=
        (LONG)sizeof(mp_printer_icon_cache_magic))
        ok = FALSE;
    if (ok && Write(file, cached_uri, sizeof(cached_uri)) !=
              (LONG)sizeof(cached_uri))
        ok = FALSE;
    if (ok && Write(file, mp_printer_icon_rgba,
                    sizeof(mp_printer_icon_rgba)) !=
              (LONG)sizeof(mp_printer_icon_rgba))
        ok = FALSE;

    Close(file);
    if (!ok)
        DeleteFile(path);
    return ok;
}

static BOOL mp_load_printer_icon_cache_file(CONST_STRPTR path,
                                             BOOL require_uri_match) {
    BPTR file;
    UBYTE magic[sizeof(mp_printer_icon_cache_magic)];
    char cached_uri[sizeof(printer_icon_uri)];
    UBYTE *rgba;
    int i;

    file = Open(path, MODE_OLDFILE);
    if (!file)
        return FALSE;

    if (Read(file, magic, sizeof(magic)) != (LONG)sizeof(magic) ||
        memcmp(magic, mp_printer_icon_cache_magic, sizeof(magic)) != 0 ||
        Read(file, cached_uri, sizeof(cached_uri)) !=
            (LONG)sizeof(cached_uri)) {
        Close(file);
        return FALSE;
    }
    cached_uri[sizeof(cached_uri) - 1] = '\0';

    if (require_uri_match &&
        (!printer_icon_uri[0] || strcmp(cached_uri, printer_icon_uri) != 0)) {
        Close(file);
        return FALSE;
    }

    rgba = AllocVec(sizeof(mp_printer_icon_rgba), MEMF_ANY);
    if (!rgba) {
        Close(file);
        return FALSE;
    }
    if (Read(file, rgba, sizeof(mp_printer_icon_rgba)) !=
        (LONG)sizeof(mp_printer_icon_rgba)) {
        FreeVec(rgba);
        Close(file);
        return FALSE;
    }
    Close(file);

    memcpy(mp_printer_icon_rgba, rgba, sizeof(mp_printer_icon_rgba));
    FreeVec(rgba);
    memset(mp_printer_icon_mask, 0, sizeof(mp_printer_icon_mask));
    for (i = 0; i < MP_PRINTER_ICON_PIXELS; ++i)
        mp_printer_icon_mask[i] = mp_printer_icon_rgba[i * 4 + 3] ? 1 : 0;

    mp_printer_icon_valid = TRUE;
    mp_printer_icon_pens_valid = FALSE;
    return TRUE;
}

static void mp_save_printer_icon_cache(void) {
    char env_path[96];
    char envarc_path[96];

    if (!mp_printer_icon_valid)
        return;

    if (!ensure_config_dir((CONST_STRPTR)"ENV:MintPRINT") ||
        !ensure_config_dir((CONST_STRPTR)"ENVARC:MintPRINT") ||
        !ensure_config_dir((CONST_STRPTR)"ENV:MintPRINT/Art") ||
        !ensure_config_dir((CONST_STRPTR)"ENVARC:MintPRINT/Art"))
        return;

    unit_icon_cache_path(current_unit_index, FALSE, env_path, sizeof(env_path));
    unit_icon_cache_path(current_unit_index, TRUE, envarc_path, sizeof(envarc_path));
    mp_write_printer_icon_cache_file((CONST_STRPTR)env_path);
    mp_write_printer_icon_cache_file((CONST_STRPTR)envarc_path);
}

static BOOL mp_load_printer_icon_cache(BOOL require_uri_match) {
    char env_path[96];
    char envarc_path[96];

    unit_icon_cache_path(current_unit_index, FALSE, env_path, sizeof(env_path));
    unit_icon_cache_path(current_unit_index, TRUE, envarc_path, sizeof(envarc_path));

    if (mp_load_printer_icon_cache_file((CONST_STRPTR)env_path,
                                        require_uri_match))
        return TRUE;

    if (mp_load_printer_icon_cache_file((CONST_STRPTR)envarc_path,
                                        require_uri_match)) {
        /* Re-seed volatile ENV: after a reboot, best-effort. */
        if (ensure_config_dir((CONST_STRPTR)"ENV:MintPRINT") &&
            ensure_config_dir((CONST_STRPTR)"ENV:MintPRINT/Art"))
            mp_copy_file((CONST_STRPTR)envarc_path, (CONST_STRPTR)env_path);
        return TRUE;
    }
    return FALSE;
}

static void mp_delete_printer_icon_cache(void) {
    char env_path[96];
    char envarc_path[96];
    unit_icon_cache_path(current_unit_index, FALSE, env_path, sizeof(env_path));
    unit_icon_cache_path(current_unit_index, TRUE, envarc_path, sizeof(envarc_path));
    DeleteFile((CONST_STRPTR)env_path);
    DeleteFile((CONST_STRPTR)envarc_path);
}

'''
replace_once(old, cache_helpers, 'icon cache helpers')

# 4) Refresh: validate existing processed cache by URI before downloading/decoding.
old = '''static void mp_refresh_printer_icon(void) {
    mp_clear_printer_icon();

    if (!printer_icon_uri[0])
        return;

    if (mp_fetch_printer_icon_file(printer_icon_uri))
        mp_load_printer_icon_rgba();

    DeleteFile((CONST_STRPTR)MP_PRINTER_ICON_TEMP);
}
'''
new = '''static void mp_refresh_printer_icon(void) {
    mp_clear_printer_icon();

    if (!printer_icon_uri[0]) {
        /* A successful Query saying there is no printer-icons attribute
         * makes any older artwork for this Unit stale. */
        mp_delete_printer_icon_cache();
        return;
    }

    /* Fast path: the processed 35x35 RGBA cache includes the source URI.
     * If it still matches, no HTTP transfer and no PNG decode are needed. */
    if (mp_load_printer_icon_cache(TRUE))
        return;

    if (mp_fetch_printer_icon_file(printer_icon_uri) &&
        mp_load_printer_icon_rgba())
        mp_save_printer_icon_cache();

    DeleteFile((CONST_STRPTR)MP_PRINTER_ICON_TEMP);
}
'''
replace_once(old, new, 'refresh icon cache fast path')

# 5) A transient failed Query should fall back to the last known artwork.
old = '''        custom_printf("Scan failed - please try Query again");
        mp_clear_printer_icon();
'''
new = '''        custom_printf("Scan failed - please try Query again");
        mp_clear_printer_icon();
        mp_load_printer_icon_cache(FALSE);
'''
replace_once(old, new, 'failed query cached icon fallback')

# 6) Activating another Unit copies its artwork cache too, just like capabilities.
old = '''                                    mp_copy_file((CONST_STRPTR)src_cache_env, (CONST_STRPTR)dst_cache_env);
                                    mp_copy_file((CONST_STRPTR)src_cache_envarc, (CONST_STRPTR)dst_cache_envarc);

                                    custom_printf("Unit%d copied to Unit0 - it is now the active printer.\n",
'''
new = '''                                    mp_copy_file((CONST_STRPTR)src_cache_env, (CONST_STRPTR)dst_cache_env);
                                    mp_copy_file((CONST_STRPTR)src_cache_envarc, (CONST_STRPTR)dst_cache_envarc);

                                    /* Carry the processed printer artwork too. */
                                    {
                                        char src_icon_env[96], src_icon_envarc[96];
                                        char dst_icon_env[96], dst_icon_envarc[96];
                                        ensure_config_dir((CONST_STRPTR)"ENV:MintPRINT/Art");
                                        ensure_config_dir((CONST_STRPTR)"ENVARC:MintPRINT/Art");
                                        unit_icon_cache_path(current_unit_index, FALSE, src_icon_env, sizeof(src_icon_env));
                                        unit_icon_cache_path(current_unit_index, TRUE, src_icon_envarc, sizeof(src_icon_envarc));
                                        unit_icon_cache_path(0, FALSE, dst_icon_env, sizeof(dst_icon_env));
                                        unit_icon_cache_path(0, TRUE, dst_icon_envarc, sizeof(dst_icon_envarc));
                                        mp_copy_file((CONST_STRPTR)src_icon_env, (CONST_STRPTR)dst_icon_env);
                                        mp_copy_file((CONST_STRPTR)src_icon_envarc, (CONST_STRPTR)dst_icon_envarc);
                                    }

                                    custom_printf("Unit%d copied to Unit0 - it is now the active printer.\n",
'''
replace_once(old, new, 'activate unit artwork cache copy')

# 7) Final tiny layout alignment requested from the latest screenshot.
old = '''    ng.ng_LeftEdge = 132;
    ng.ng_TopEdge = 78 + topborder;
'''
new = '''    ng.ng_LeftEdge = 130;
    ng.ng_TopEdge = 78 + topborder;
'''
replace_once(old, new, 'Printer Engine x alignment')
s = s.replace('// Printer Engine has the longest label in the left column; x=132 keeps\n'
              '// a small left margin while leaving the compact ink panel free at x=320.\n',
              '// Printer Engine aligns with the other left-column gadget bodies at x=130.\n', 1)

p.write_text(s, encoding='utf-8')
print('PR63 processed printer artwork cache + engine alignment staged')
