/* MintPrint Settings (formerly IPP-Test16.c / "MintPRINT Preferences").
   Setup/test GUI for the DEVS:Printers/MintPRINT driver: LAN printer
   discovery, IPP capability query, driver install/select helper, and
   per-job defaults editing. */
/* MintPRINT GUI stabilised: no live cycle-label frees; safe teardown. */
/* MintPRINT prefs #9: compact address row and status-box fit. */
/* MintPRINT prefs #8: capability cache and output-area layout polish. */
/* Amiga IPP Print-Job Prototype with GUI
   Configures and tests MintPRINT's IPP document engines
   Compile with: m68k-amigaos-gcc -g -o IPP-test11 ipp-test11.c -lamiga -lsocket -lm
 PATCH INCOMING: Adds IFF -> RGB -> PWG -> IPP printing support to IPP-test15 */


#include <proto/exec.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h> /* MEMF_ANY for OS-native response buffers */
#include <ctype.h> // for tolower()
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h> // for struct DosList/LDF_DEVICES (Spooler HDD detection)
#include <exec/lists.h> // for struct List/struct Node (Spooler job LISTVIEW_KIND)
#include <dos/dostags.h> // for SYS_Asynch (SystemTags)

/* The H (hidden) protection bit - Protect's HSPARWED flags, present since
 * AmigaOS 2.04 (this project's own minimum) though not always defined by
 * an older NDK's <dos/dos.h>. Guarded rather than assumed. */
#ifndef FIBF_HIDDEN
#define FIBF_HIDDEN 0x00000080L
#endif
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
typedef long ssize_t;
#include <clib/alib_protos.h>
#include <proto/bsdsocket.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/displayinfo.h>
#include <devices/printer.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h> // for O_NONBLOCK
#include <sys/ioctl.h> // for FIONBIO (mp_connect_with_timeout)
#include <errno.h> // for ETIMEDOUT (mp_connect_with_timeout) - EINPROGRESS
                   // already resolved via bsdsocket.h before this include
                   // existed, so ETIMEDOUT should too, but unconfirmed on
                   // this specific NDK without a real build
#include "iff-loader.h"
#include "http_response.h"
#include "dpi_options.h"
#include "media_size.h"
#include "config.h"
#include "ipp_client.h"
#include "ipp_enum.h"
#include "lodepng.h"

/* All status/progress output goes to the on-screen status box, never a
 * console - the end user may have launched this from Workbench, where
 * there is no console to see it in. custom_printf() itself is defined
 * further down (it draws into the on-screen output box); forward-declare
 * it and redirect printf() to it here, before any of this file's own
 * printf() calls, so every one of them lands in the box consistently. */
void custom_printf(const char *format, ...);
#define printf custom_printf

extern struct GfxBase *GfxBase;
extern struct ExecBase *SysBase;
#define MAX_VALUES 32
#define MAX_ATTR_LEN 64
#define MAX_BUFFER 256000
/* 8, not 10: the box's font is Topaz80 (see redraw_output_box()), giving
 * a line height of tf_YSize(8)+2=10px, i.e. 80px for the box's text area.
 * WA_InnerHeight (see main()) is derived from OUTPUT_TOP and this count
 * specifically so the box's bottom border always ends flush with the
 * window's own bottom edge - changing either one without the other
 * reintroduces either dead space below the box or a border pushed past
 * the window's edge. */
#define MAX_OUTPUT_LINES 8
/* The debug output box is OUTPUT_LEFT..OUTPUT_RIGHT wide - at the main
 * window's 520px width that's ~490px, or ~61 chars of Topaz80 (8px/char).
 * MAX_OUTPUT_LINE_LENGTH includes the terminating NUL, so 62 stores at
 * most 61 visible characters. Re-check this against OUTPUT_LEFT/
 * OUTPUT_RIGHT if the window width changes again. */
#define MAX_OUTPUT_LINE_LENGTH 62
#define MAX_PRINT_MODES 8
#define MAX_QUALITIES 5
#define MENU_ID_FILE       1
#define MENU_ID_FILE_SAVE  2
#define MENU_ID_FILE_QUIT  3

// Gadget IDs
#define GAD_IP_STRING 1
#define GAD_FILE_STRING 2
#define GAD_QUERY_BUTTON 3
#define GAD_PRINT_BUTTON 4
#define GAD_EXIT_BUTTON 5
#define GAD_MEDIA_DROPDOWN 6
#define GAD_PRINT_MODE 7
#define GAD_SCALING_MODE 8
#define GAD_QUALITY_MODE 9
#define GAD_IPP_PATH 10
#define GAD_DEBUG 11
#define GAD_ENGINE 12
#define GAD_SAVE_BUTTON 13
#define GAD_DISCOVER_BUTTON 14
#define GAD_UNIT_DROPDOWN 15
#define GAD_SET_ACTIVE_BUTTON 16
#define GAD_MODEL_DISPLAY 17
#define GAD_RESOLUTION 18
#define GAD_SIDES 19
#define GAD_SPOOLER 20
#define GAD_SPOOL_KEEP 21
#define GAD_VIEW_SPOOL 22

/* Spooler management window gadget IDs (separate window/gadget list, like
 * the discovery selection dialog's GAD_DISC_* above). */
#define GAD_SPOOL_JOB_LIST 1
#define GAD_SPOOL_REFRESH   2
#define GAD_SPOOL_DELETE    3
#define GAD_SPOOL_CLOSE     4
#define GAD_SPOOL_RETRY     5
#define GAD_SPOOL_COPIES    6
#define GAD_SPOOL_UNIT      7

/* Copies dialog gadget IDs (separate window/gadget list again). */
#define GAD_SPOOL_COPIES_FIELD  1
#define GAD_SPOOL_COPIES_OK     2
#define GAD_SPOOL_COPIES_CANCEL 3

// Discovery selection dialog gadget IDs (separate window/gadget list)
#define GAD_DISC_CYCLE  1
#define GAD_DISC_USE    2
#define GAD_DISC_CANCEL 3

#define MAX_DISCOVERY_RESULTS 16

struct DiscoveredPrinter {
    char ip[16];
    char label[80];
};

/* Test Print must outlive the gadget callback that starts it. Keeping the
 * RastPort, bitmap and IO request here lets printer.device run asynchronously
 * while the normal GadTools event loop continues servicing the window. */
struct MPTestPrintJob {
    struct MsgPort *port;
    struct IODRPReq *request;
    struct BitMap *bitmap;
    struct BitMap bitmap_storage;
    struct ColorMap *colormap;
    struct RastPort rastport;
    BOOL bitmap_manual;
    BOOL device_open;
    BOOL active;
};

static struct MPTestPrintJob test_print_job;

// Saved printer profiles: ENV:MintPRINT/Unit0 .. Unit(MAX_UNITS-1). Only
// Unit0 is what the driver actually reads at print time; the others are
// switchable GUI-side profiles (e.g. for a second/third network printer).
#define MAX_UNITS 8

/* A few pixels below the Test Print/Debug/Save/Exit row (216+12 tall,
 * itself 4px below Keep Spooled Jobs/View Spooler at 198) for even spacing - see
 * WA_InnerHeight in main() for why raising this also raises that: the
 * box's bottom border sits at OUTPUT_TOP + 81 (MAX_OUTPUT_LINES lines at
 * 10px, see below, plus the 2px border), and WA_InnerHeight is kept equal
 * to that so the box's border sits flush with the window's own bottom
 * edge instead of leaving dead space below it. */
#define OUTPUT_TOP     232 // Below Test Print / Debug / Save / Exit row
#define OUTPUT_LEFT    10
#define OUTPUT_RIGHT   (window->Width - 20)

// Define the USED macro for GCC
#define USED __attribute__((used))
#define MINTPRINT_SETTINGS_VERSION "1.3.0"
#define MINTPRINT_DRIVER_DEST ((CONST_STRPTR)"DEVS:Printers/MintPRINT")

/* MintPrint Settings now ships as a single drawer containing both bundled
 * driver builds under Drivers/, and picks the one matching this machine's
 * printer.device generation automatically - see mp_driver_src_path() near
 * the other driver-install helpers. The macro is function-like so every
 * use site (including ones earlier in this file than the function
 * definition itself) re-evaluates the detection rather than caching a
 * stale path. */
#define MINTPRINT_DRIVER_SRC  (mp_driver_src_path())
static CONST_STRPTR mp_driver_src_path(void);

/* The driver's own build version, read out of its "$VER: MintPRINT
 * <version>.<revision>" string - see mp_read_driver_version() near the
 * other driver-install helpers for how. A plain struct (not just a
 * forward-declared opaque type) because callers like the test page below
 * need it as a value, not just a pointer. */
struct MPDriverVersion {
    UWORD version;
    UWORD revision;
};
static BOOL mp_read_driver_version(CONST_STRPTR path, struct MPDriverVersion *out);

/* Visible both to AmigaOS's Version command and in the About requester. */
static const char USED mintprint_version[] =
    "$VER: MintPrintSettings " MINTPRINT_SETTINGS_VERSION " (29.08.2026)";

// Simple extension check
BOOL has_extension(const char *filename, const char *ext) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return FALSE;
    return strcasecmp(dot, ext) == 0;
}
/*
 * Classic AmigaOS / libnix stack request.
 *
 * The "$STACK:" cookie is useful on newer AmigaOS startup code, but classic
 * m68k AmigaOS programs built with the GCC/libnix runtime use the __stack
 * variable. Keep a 256 KiB margin: discovery/add chains through
 * parsing, profile migration, cache/icon handling, and GadTools requesters;
 * reducing this to 128 KiB caused a delayed 81000005 memory-list failure on
 * classic OS 2.x testing. Exercise Query, Discover, Add and Test Print on
 * real AmigaOS hardware before reducing this again.
 *
 * 256 KiB = 262144 bytes.
 */
unsigned long __stack = 262144UL;

/* Keep the cookie as harmless metadata for newer startup code too. */
static const char USED min_stack[] = "$STACK:262144";

// Structure to map media sizes to trays (Updated to include tray name and medianame)
struct MediaTrayMap {
    char media[MAX_ATTR_LEN];      // e.g., "iso_a4_210x297mm"
    char source[MAX_ATTR_LEN];     // e.g., "by-pass-tray"
    char trayName[MAX_ATTR_LEN];   // e.g., "MP TRAY"
    char medianame[MAX_ATTR_LEN];  // e.g., "INKJET"
};

// Globals for parsed capabilities
char supported_formats[MAX_VALUES][MAX_ATTR_LEN];
int num_supported_formats = 0;
BOOL jpeg_constraints_queried = FALSE;
BOOL jpeg_k_octets_reported = FALSE;
BOOL jpeg_x_dimension_reported = FALSE;
BOOL jpeg_y_dimension_reported = FALSE;

char supported_media[MAX_VALUES][MAX_ATTR_LEN];
int num_supported_media = 0;

char supported_output_modes[MAX_VALUES][MAX_ATTR_LEN];
int num_supported_output_modes = 0;

char supported_sides[MAX_VALUES][MAX_ATTR_LEN];
int num_supported_sides = 0;
BOOL supports_create_job = FALSE;
BOOL supports_send_document = FALSE;
BOOL supports_multiple_document_jobs = FALSE;
BOOL supports_single_document_handling = FALSE;
char pwg_sheet_back_value[MAX_ATTR_LEN] = "normal";

char supported_scaling[MAX_VALUES][MAX_ATTR_LEN];
int num_supported_scaling = 0;

int supported_orientations[MAX_VALUES];
int num_supported_orientations = 0;

char supported_print_modes[MAX_VALUES][MAX_ATTR_LEN];
int num_supported_print_modes = 0;

#define MP_MAX_DPI_OPTIONS MP_DPI_MAX_OPTIONS
int supported_dpi[MP_MAX_DPI_OPTIONS];
int num_supported_dpi = 0;
static struct MPDpiOptions mp_dpi_options = {
    { 300, 0 }, { 0, 0 }, 1, 0, 300
};

char *print_mode_options[MAX_PRINT_MODES];
int num_print_modes = 0;
static char initial_print_mode_value[MAX_ATTR_LEN] = "Not Detected";
static STRPTR initial_print_mode[] = { initial_print_mode_value, NULL };
char selected_print_mode[MAX_ATTR_LEN] = "monochrome"; // Default fallback
char selected_scaling[MAX_ATTR_LEN] = "auto"; // Default
static char initial_scaling_value[MAX_ATTR_LEN] = "Not Detected";
static STRPTR initial_scaling_mode[] = { initial_scaling_value, NULL };
/* MintPRINT stable Cycle gadget label storage (OS3.1-safe).
 *
 * Classic GadTools is much happier when GTCY_Labels keeps the same array
 * address for the complete lifetime of a live CYCLE_KIND gadget. Earlier
 * versions repeatedly AllocVec'd new arrays during Query/Save and retargeted
 * the live gadgets. That could leave V37 GadTools with stale bookkeeping,
 * producing delayed memory alerts or a hard lock during a later Query/Exit.
 *
 * Keep both the pointer arrays and their text backing in static storage.
 * Query rewrites the contents in-place and re-applies THE SAME pointer.
 */
#define MP_MEDIA_LABEL_LEN   (MAX_ATTR_LEN + 32)
#define MP_UNIT_LABEL_LEN    128

static char mp_media_label_storage[MAX_VALUES + 1][MP_MEDIA_LABEL_LEN];
static STRPTR mp_media_label_ptrs[MAX_VALUES + 2];

static char mp_scaling_label_storage[MAX_VALUES + 1][MAX_ATTR_LEN];
static STRPTR mp_scaling_label_ptrs[MAX_VALUES + 2];

static char mp_print_mode_label_storage[MAX_VALUES + 1][MAX_ATTR_LEN];
static STRPTR mp_print_mode_label_ptrs[MAX_VALUES + 2];

static char mp_quality_label_storage[MAX_VALUES + 1][32];
static STRPTR mp_quality_label_ptrs[MAX_VALUES + 2];

static char mp_dpi_label_storage[MP_MAX_DPI_OPTIONS + 1][16];
static STRPTR mp_dpi_label_ptrs[MP_MAX_DPI_OPTIONS + 2];

#define MP_MAX_SIDES_OPTIONS 3
static char mp_sides_label_storage[MP_MAX_SIDES_OPTIONS][24];
static char mp_sides_value_storage[MP_MAX_SIDES_OPTIONS][MAX_ATTR_LEN];
static STRPTR mp_sides_label_ptrs[MP_MAX_SIDES_OPTIONS + 1];
static int mp_sides_option_count = 1;

/* Spooler cycle gadget storage - same static-storage/fixed-array-address
 * discipline as mp_sides_* above (see driver_spool_buffer's own comment,
 * defined with the other persisted Unit0 buffers below, for why). Built
 * once at startup by mp_build_spool_options() from this machine's DOS
 * device list, never reallocated while the gadget is live. */
#define MP_MAX_SPOOL_OPTIONS 9 /* RAM + up to 8 detected hard drive devices */
static char mp_spool_label_storage[MP_MAX_SPOOL_OPTIONS][24];
static char mp_spool_value_storage[MP_MAX_SPOOL_OPTIONS][MAX_ATTR_LEN];
static STRPTR mp_spool_label_ptrs[MP_MAX_SPOOL_OPTIONS + 1];
static int mp_spool_option_count = 1;

static char mp_unit_label_storage[MAX_UNITS][MP_UNIT_LABEL_LEN];
static STRPTR mp_unit_label_ptrs[MAX_UNITS + 1];

STRPTR *scaling_mode_labels = mp_scaling_label_ptrs;
char selected_quality[16] = "auto"; // Default
char supported_quality[MAX_QUALITIES][16];
int num_supported_quality = 0;
STRPTR *quality_mode_labels = mp_quality_label_ptrs;
static char initial_quality_value[32] = "Not Detected";
static STRPTR initial_quality_mode[] = { initial_quality_value, NULL };
// Media dropdown state
char *selected_media = NULL;
struct Gadget *media_dropdown = NULL;
STRPTR *media_dropdown_items = mp_media_label_ptrs;
BOOL has_media_ready = FALSE;
struct Menu *menu = NULL;
struct MediaTrayMap media_tray_map[MAX_VALUES];
int num_media_tray_mappings = 0;

// Radio button labels for print mode
STRPTR *print_mode_labels = mp_print_mode_label_ptrs;

static char initial_media_value[160] = "Not Detected";
static STRPTR initial_media_labels[] = { initial_media_value, NULL };



static struct NewMenu menu_template[] = {
    { NM_TITLE, (STRPTR)"File", 0, 0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Save Driver Settings", 0, 0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Reload Driver Settings", 0, 0, 0, 0 },
    { NM_ITEM,  NM_BARLABEL, 0, 0, 0, 0 },
    { NM_ITEM,  (STRPTR)"About MintPRINT...", 0, 0, 0, 0 },
    { NM_ITEM,  NM_BARLABEL, 0, 0, 0, 0 },
    { NM_ITEM,  (STRPTR)"Quit", 0, 0, 0, 0 },
    { NM_TITLE, (STRPTR)"Help", 0, 0, 0, 0 },
    { NM_ITEM,  (STRPTR)"MintPrint Settings Help...", 0, 0, 0, 0 },
    { NM_END,   NULL, 0, 0, 0, 0 }
};
// Global variable to store the print mode (0 = Black and White, 1 = Color)
int print_mode = 0; // Default to Black and White

// Helper to store values into lists
void store_value(char dest[MAX_VALUES][MAX_ATTR_LEN], int *count, const char *value) {
    if (*count >= MAX_VALUES) return;
    strncpy(dest[*count], value, MAX_ATTR_LEN - 1);
    dest[*count][MAX_ATTR_LEN - 1] = '\0';
    (*count)++;
}

void store_int_value(int dest[MAX_VALUES], int *count, int val) {
    if (*count >= MAX_VALUES) return;
    dest[(*count)++] = val;
}

static void mp_add_supported_dpi(int dpi) {
    int i;

    if (dpi != 300 && dpi != 600) return;
    for (i = 0; i < num_supported_dpi; ++i) {
        if (supported_dpi[i] == dpi) return;
    }
    if (num_supported_dpi < MP_MAX_DPI_OPTIONS)
        supported_dpi[num_supported_dpi++] = dpi;
}

static int mp_normalise_ipp_dpi(ULONG resolution, UBYTE units) {
    ULONG dpi = resolution;

    if (units == 4) {
        /* IPP dpcm -> dpi. Tolerances below absorb the integer rounding
         * used by printers for 118dpcm/236dpcm (roughly 300/600dpi). */
        dpi = (resolution * 254UL + 50UL) / 100UL;
    } else if (units != 3) {
        return 0;
    }

    if (dpi >= 295UL && dpi <= 305UL) return 300;
    if (dpi >= 595UL && dpi <= 605UL) return 600;
    return 0;
}

static void mp_add_ipp_resolution(const UBYTE *raw, int value_len) {
    ULONG xres;
    ULONG yres;
    int xdpi;
    int ydpi;

    if (!raw || value_len != 9) return;

    xres = ((ULONG)raw[0] << 24) | ((ULONG)raw[1] << 16) |
           ((ULONG)raw[2] << 8) | (ULONG)raw[3];
    yres = ((ULONG)raw[4] << 24) | ((ULONG)raw[5] << 16) |
           ((ULONG)raw[6] << 8) | (ULONG)raw[7];
    xdpi = mp_normalise_ipp_dpi(xres, raw[8]);
    ydpi = mp_normalise_ipp_dpi(yres, raw[8]);

    /* The driver currently has one DPI setting for both axes, so do not
     * advertise asymmetric printer resolutions it cannot represent. */
    if (xdpi && xdpi == ydpi) {
        mp_add_supported_dpi(xdpi);
        printf("Printer supports %d DPI\n", xdpi);
    }
}

static int mp_dpi_active_index(int dpi) {
    int i;

    for (i = 0; i < mp_dpi_options.count; ++i) {
        if (mp_dpi_options.values[i] == dpi) return i;
    }
    return 0;
}

/* Defined with the other persisted Unit0 buffers below. */
extern char driver_sides_buffer[MAX_ATTR_LEN];
extern char driver_engine_buffer[32];
extern char driver_spool_buffer[MAX_ATTR_LEN];
extern BOOL driver_spool_keep;

static BOOL mp_supported_side(const char *value) {
    int i;

    if (!value) return FALSE;
    for (i = 0; i < num_supported_sides; ++i) {
        if (strcmp(supported_sides[i], value) == 0) return TRUE;
    }
    return FALSE;
}

static BOOL mp_duplex_transport_supported(void) {
    int i;
    const char *format_mime;

    /* MintPRINT duplex is one multi-page PWG Raster or Apple Raster (URF)
     * document in one ordinary Print-Job. This works on printers such as
     * the Brother MFC-J6930DW that advertise duplex but explicitly reject
     * multi-document IPP Jobs. */
    if (strcmp(driver_engine_buffer, "pwg-raster") == 0)
        format_mime = "image/pwg-raster";
    else if (strcmp(driver_engine_buffer, "urf") == 0)
        format_mime = "image/urf";
    else
        return FALSE;
    for (i = 0; i < num_supported_formats; ++i) {
        if (strcmp(supported_formats[i], format_mime) == 0)
            return TRUE;
    }
    return FALSE;
}

static ULONG mp_sides_active_index(void) {
    int i;

    for (i = 0; i < mp_sides_option_count; ++i) {
        if (strcmp(driver_sides_buffer, mp_sides_value_storage[i]) == 0)
            return (ULONG)i;
    }
    return 0;
}

static ULONG mp_spool_active_index(void) {
    int i;

    for (i = 0; i < mp_spool_option_count; ++i) {
        if (strcmp(driver_spool_buffer, mp_spool_value_storage[i]) == 0)
            return (ULONG)i;
    }
    return 0;
}

/* "Keep spooled jobs" only means anything once job files actually spool
 * to a real drive - RAM keeps MintPRINT's original flat, always-
 * overwritten behaviour regardless (see driver_core.c's mp_job_begin()),
 * so the tickbox is disabled and forced off whenever driver_spool_buffer
 * is empty or "RAM". */
static BOOL mp_spool_keep_available(void) {
    return driver_spool_buffer[0] && strcmp(driver_spool_buffer, "RAM") != 0;
}

/* Populates the Spooler cycle gadget's choices: "RAM" (index 0, always
 * present) plus one "HDD (DHn:)" entry per DHn hard drive device this
 * machine's DOS device list actually has mounted - the standard Amiga
 * IDE/SCSI partition naming convention (a name-based heuristic, not a
 * hardware check: a controller using some other naming scheme, e.g. a
 * vendor-specific CF/SD adapter, will not show up here). Called once at
 * startup, before the window/gadgets exist - see the static-storage
 * comment above driver_spool_buffer. LockDosList()/UnLockDosList() take
 * the DOS list's own internal semaphore, so no Forbid() is needed around
 * the walk. dol_Name is a BCPL BSTR (length-prefixed, not
 * NUL-terminated), hence BADDR() and an explicit length rather than a
 * plain string copy. */
static void mp_build_spool_options(void) {
    struct DosList *dl;
    int count = 0;

    mp_spool_label_ptrs[0] = mp_spool_label_storage[0];
    strcpy(mp_spool_label_storage[0], "RAM");
    strcpy(mp_spool_value_storage[0], "RAM");
    count = 1;

    dl = LockDosList(LDF_DEVICES | LDF_READ);
    while (count < MP_MAX_SPOOL_OPTIONS &&
           (dl = NextDosEntry(dl, LDF_DEVICES)) != NULL) {
        UBYTE *bname = (UBYTE *)BADDR(dl->dol_Name);
        UBYTE blen;
        char name[32];
        UBYTE i;

        if (!bname) continue;
        blen = bname[0];
        if (blen > sizeof(name) - 1) blen = (UBYTE)(sizeof(name) - 1);
        for (i = 0; i < blen; ++i) name[i] = (char)bname[i + 1];
        name[blen] = '\0';

        if (blen >= 2 && (name[0] == 'D' || name[0] == 'd') &&
            (name[1] == 'H' || name[1] == 'h')) {
            snprintf(mp_spool_label_storage[count],
                     sizeof(mp_spool_label_storage[0]),
                     "HDD (%s:)", name);
            mp_spool_label_ptrs[count] = mp_spool_label_storage[count];
            snprintf(mp_spool_value_storage[count],
                     sizeof(mp_spool_value_storage[0]), "%s:", name);
            ++count;
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);

    mp_spool_label_ptrs[count] = NULL;
    mp_spool_option_count = count;
}

// Media Size Helper
BOOL parse_media_dimensions(const char *media_str, int *x, int *y) {
    const char *dim_part = strchr(media_str, '_');
    if (!dim_part) return FALSE;

    int w, h;
    if (sscanf(dim_part + 1, "%dx%dmm", &w, &h) == 2) {
        *x = w * 100; // Convert mm to hundredths of mm
        *y = h * 100;
        return TRUE;
    }
    return FALSE;
}

void ensure_quality_defaults() {
    if (num_supported_quality == 0) {
        /* Do not invent draft/high support when a printer omits this
         * capability. The Samsung C480W only accepts normal, yet the old
         * three-value fallback selected draft and made the IPP server
         * return successful-ok-ignored-or-substituted-attributes. */
        printf("No print-quality-supported returned; using safe normal quality.\n");
        strcpy(supported_quality[0], "normal");
        num_supported_quality = 1;
    }
}

//Helper to parse IP and port from GUI
int parse_ip_and_port(const char *input, char *ip_out, int ip_len, int *port_out) {
    char *colon = strchr(input, ':');
    if (colon) {
        int len = colon - input;
        if (len >= ip_len) return 0;
        strncpy(ip_out, input, len);
        ip_out[len] = '\0';
        *port_out = atoi(colon + 1);
    } else {
        strncpy(ip_out, input, ip_len - 1);
        ip_out[ip_len - 1] = '\0';
        *port_out = -1;  // no port specified
    }
    return 1;
}


//Safe Send Data
int safe_send(int sockfd, const void *vbuf, int len) {
    const char *buf = (const char *)vbuf;
    int total_sent = 0;
    int attempt = 0;

    while (total_sent < len) {
        int chunk_size = (len - total_sent > 4096) ? 4096 : (len - total_sent);
        int sent = send(sockfd, (char *)buf + total_sent, chunk_size, 0);
        attempt++;

        if (sent <= 0) {
            printf("\n[!] send() failed at %d bytes (attempt %d)\n", total_sent, attempt);
            perror("send");
            return -1;
        }

        total_sent += sent;

        // Progress bar (every 64KB or on finish)
        if ((total_sent % 65536 == 0) || (total_sent == len)) {
            printf("[+] Sent %d / %d bytes (%d%%)\n", total_sent, len, (total_sent * 100) / len);
        }
    }

    printf("[OK] Finished sending %d bytes successfully\n", total_sent);
    return total_sent;
}

// Global variables for GUI
struct Window *window = NULL;
struct Gadget *glist = NULL;
struct Library *SocketBase = NULL;
struct Library *GadToolsBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
char ip_buffer[256] = "";
char driver_path_buffer[96] = "/ipp/print";
BOOL driver_debug = FALSE;
static STRPTR debug_labels[] = { "Debug Off", "Debug On", NULL };
/* Capture resolution driver.device reports to the app and renders at.
 * 600dpi quadruples raster size/RAM use for a real quality gain; 300dpi
 * (index 0) stays the default so existing users see no behaviour change. */
STRPTR *resolution_labels = mp_dpi_label_ptrs;
static char initial_dpi_value[16] = "300 dpi";
int driver_resolution = 300;
/* Distinguishes a saved/user-selected 300 DPI value from the unsaved default.
 * An explicit value may select the marked compatibility entry; a fresh Query
 * otherwise keeps an actually reported resolution as the default. */
static BOOL driver_resolution_explicit = FALSE;
char driver_engine_buffer[32] = "jpeg";
/* Same convention as driver_resolution_explicit above: distinguishes a
 * saved/user-selected engine from the unsaved "jpeg" compiled-in default.
 * Without this, a fresh printer that supports both JPEG and PWG Raster
 * silently stayed on JPEG forever - PWG Raster was only ever picked if
 * JPEG wasn't advertised at all (mp_rebuild_engine_options_from_query()
 * keeps whatever's already in driver_engine_buffer as long as it's still
 * supported). JPEG (and PDF, which reuses the same JPEG encoder - see
 * pdf_writer.c) costs real DCT/quantization work per pixel on top of
 * printer.device's DUMPRPORT scale-up; PWG Raster is a cheap PackBits-style
 * pack. Real 68k hardware (issue #30, HP OfficeJet 8014e on an '060) felt
 * that gap directly: it advertises both formats, defaulted to JPEG anyway,
 * and Test Print was slow enough to look hung. */
static BOOL driver_engine_explicit = FALSE;
/* An explicitly-pinned engine (see above) is deliberately never silently
 * overridden - but staying on JPEG once a printer is confirmed to support
 * PWG Raster is still very likely a mistake nobody meant to make (an old
 * saved config from before PWG Raster became the default, or a stray
 * click on the Engine cycle gadget), not a deliberate choice to keep
 * paying JPEG's per-pixel DCT cost. Ask once per printer per session,
 * right after a live Query confirms PWG Raster support, rather than
 * either nagging on every Query click or silently switching out from
 * under someone who really did mean to pick JPEG. Reset alongside
 * driver_engine_explicit whenever a Unit's config is (re)loaded. */
static BOOL driver_engine_pwg_offer_shown = FALSE;
#define MP_ENGINE_MAX 5

static const char *mp_engine_all_labels[MP_ENGINE_MAX] = {
    "JPEG", "PostScript", "PWG Raster", "PDF", "Apple Raster"
};
static const char *mp_engine_all_values[MP_ENGINE_MAX] = {
    "jpeg", "postscript", "pwg-raster", "pdf", "urf"
};
static const char *mp_engine_all_mimes[MP_ENGINE_MAX] = {
    "image/jpeg", "application/postscript", "image/pwg-raster", "application/pdf",
    "image/urf"
};

/* This array address stays fixed for the lifetime of the GadTools Cycle. */
static STRPTR engine_labels[MP_ENGINE_MAX + 1] = {
    "JPEG", "PostScript", "PWG Raster", "PDF", "Apple Raster", NULL
};

/* Maps the currently-visible Cycle index to MintPRINT's internal value. */
static const char *mp_engine_value_map[MP_ENGINE_MAX] = {
    "jpeg", "postscript", "pwg-raster", "pdf", "urf"
};
static int mp_engine_count = MP_ENGINE_MAX;
char driver_media_buffer[MAX_ATTR_LEN] = "";
char driver_source_buffer[MAX_ATTR_LEN] = "";
char driver_color_buffer[MAX_ATTR_LEN] = "";
char driver_quality_buffer[MAX_ATTR_LEN] = "";
char driver_scaling_buffer[MAX_ATTR_LEN] = "";
char driver_sides_buffer[MAX_ATTR_LEN] = "";
/* Where the driver spools job files: "RAM" (default - whatever T: is
 * assigned to, normally RAM: on a stock system) or a real hard drive
 * device such as "DH0:", for memory-tight systems where even T:'s usual
 * RAM: backing is scarce. See mp_build_spool_options()'s comment (above,
 * with the rest of the Spooler gadget's static storage) for how its
 * choices are populated. */
char driver_spool_buffer[MAX_ATTR_LEN] = "RAM";
/* Keeps every hard-drive-spooled job under a unique retained name (see
 * driver_core.c's mp_job_begin()) for the Spooler management window to
 * list. Only meaningful - and only enabled in the GUI - when
 * driver_spool_buffer names a real device; see mp_spool_keep_available()
 * and its GAD_SPOOL_KEEP gating below. */
BOOL driver_spool_keep = FALSE;
int current_unit_index = 0;
char printer_make_model[128] = "";
char printer_icon_uri[256] = "";

#define MP_PRINTER_ICON_LEFT  400
/* TOP/SIZE fill the full gap between the ink/toner panel above (its
 * bottom row is MP_MARKER_AREA_BOTTOM, 115) and the Sides/Quality row
 * below (TopEdge 162). The artwork is deliberately kept above that row
 * on the taller-font OS 2.x screens. */
#define MP_PRINTER_ICON_TOP   119
#define MP_PRINTER_ICON_SIZE   42
#define MP_PRINTER_ICON_TEMP  "T:MintPRINT-printer-icon.img"
#define MP_PRINTER_ICON_PIXELS (MP_PRINTER_ICON_SIZE * MP_PRINTER_ICON_SIZE)
#define MP_PRINTER_ICON_MAX_SOURCE_DIM 1024
static UBYTE mp_printer_icon_rgba[MP_PRINTER_ICON_PIXELS * 4];
static UBYTE mp_printer_icon_pens[MP_PRINTER_ICON_PIXELS];
static UBYTE mp_printer_icon_mask[MP_PRINTER_ICON_PIXELS];
static BOOL mp_printer_icon_valid = FALSE;
static BOOL mp_printer_icon_pens_valid = FALSE;

/* Ink/toner status (RFC 3805 Printer MIB / PWG5100.13 "marker-*"
 * attributes). Each of marker-names/marker-colors/marker-types/
 * marker-levels/marker-low-levels/marker-high-levels is its own separate
 * 1setOf IPP attribute, arriving as a consecutive run of values (same
 * shape as supported_media/supported_sides/etc. above) rather than
 * interleaved - so they're kept as parallel arrays here, each with its
 * own count, and combined by index afterwards (see mp_marker_count()
 * near the drawing code) rather than as one array of structs. */
#define MAX_MARKERS MAX_VALUES
char marker_names[MAX_MARKERS][MAX_ATTR_LEN];
int num_marker_names = 0;
char marker_colors[MAX_MARKERS][MAX_ATTR_LEN];
int num_marker_colors = 0;
char marker_types[MAX_MARKERS][MAX_ATTR_LEN];
int num_marker_types = 0;
int marker_levels[MAX_MARKERS];
int num_marker_levels = 0;
int marker_low_levels[MAX_MARKERS];
int num_marker_low_levels = 0;
int marker_high_levels[MAX_MARKERS];
int num_marker_high_levels = 0;

/* Printer status (RFC 8011 5.4.11 printer-state / 5.4.12
 * printer-state-reasons), reduced to one short word/phrase shown next to
 * the ink/toner strips - see mp_printer_status_label() near the drawing
 * code. Live-only, same as the marker-* fields above: reset alongside them,
 * never written to the capability cache file. printer_state_value is the
 * raw IPP enum (3 idle, 4 processing, 5 stopped); 0 means "not queried
 * yet". printer-state-reasons is itself a 1setOf keyword, so it needs the
 * same parallel-array treatment as marker_names/marker_colors/etc. */
int printer_state_value = 0;
char printer_state_reasons[MAX_MARKERS][MAX_ATTR_LEN];
int num_printer_state_reasons = 0;

STRPTR *unit_dropdown_labels = mp_unit_label_ptrs;
/* MintPRINT prefs #6: queried job defaults are saved into Unit0. */
/* MintPRINT prefs #7: saved-state placeholders, ghosting, layout and engine selector. */
char file_buffer[256] = "UHD:test.jpg";
char output_buffer[MAX_OUTPUT_LINES][MAX_OUTPUT_LINE_LENGTH];
char supported_media_sources[MAX_VALUES][MAX_ATTR_LEN];
int num_supported_media_sources = 0;
int output_line = 0;
struct Screen *screen = NULL;
void *vi = NULL;
struct TextFont *font = NULL;
/* Set once in main() from the same WBorTop/Font->ta_YSize calculation
 * createAllGadgets()'s "topborder" parameter uses, so the ink-strip panel
 * (drawn separately from GadTools, after the window already exists) lines
 * its rows up with the gadget rows instead of recomputing its own guess. */
static UWORD g_topborder = 0;
BOOL operation_in_progress = FALSE;

// Font definition
struct TextAttr Topaz80 = {
    "topaz.font",
    8,
    0,
    0
};

// Save print mode to ENV:
void save_print_mode(void) {
    BPTR file = Open("ENV:IPP_Printer_PrintMode", MODE_NEWFILE);
    if (file) {
        FPrintf(file, "%s\n", (ULONG)selected_print_mode);
        Close(file);
    }
}

// Load print mode from ENV:
void load_print_mode(void) {
    BPTR file = Open("ENV:IPP_Printer_PrintMode", MODE_OLDFILE);
    if (file) {
        char buffer[64];
        if (FGets(file, buffer, sizeof(buffer))) {
            buffer[strcspn(buffer, "\r\n")] = 0;  // Strip newline
            strncpy(selected_print_mode, buffer, MAX_ATTR_LEN - 1);
            selected_print_mode[MAX_ATTR_LEN - 1] = '\0';

            // Match it back to the index
            for (int i = 0; i < num_supported_print_modes; i++) {
                if (strcmp(supported_print_modes[i], selected_print_mode) == 0) {
                    print_mode = i;
                    break;
                }
            }
        }
        Close(file);
    }
}


static struct Gadget *find_gadget_by_id(UWORD id) {
    struct Gadget *g = glist;
    while (g && g->GadgetID != id) g = g->NextGadget;
    return g;
}

/* V37 TEXT_KIND does not reliably erase the previous text when GTTX_Text
 * changes. Clear only the gadget's value rectangle first; the descriptive
 * "Printer Model:" label sits outside this box and is left untouched.
 * This is harmless on V39+ and keeps one shared executable. */
static void mp_update_model_display(struct Window *win) {
    struct Gadget *g;
    struct RastPort *rp;
    UBYTE old_apen;

    if (!win || !win->RPort)
        return;
    g = find_gadget_by_id(GAD_MODEL_DISPLAY);
    if (!g)
        return;

    rp = win->RPort;
    old_apen = rp->FgPen;
    SetAPen(rp, 0);
    RectFill(rp, g->LeftEdge, g->TopEdge,
             g->LeftEdge + g->Width - 1,
             g->TopEdge + g->Height - 1);
    SetAPen(rp, old_apen);

    GT_SetGadgetAttrs(g, win, NULL,
                      GTTX_Text, (ULONG)printer_make_model,
                      TAG_DONE);
}

static void trim_config_line(char *s) {
    size_t n;
    if (!s) return;
    n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                 s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static BOOL ensure_config_dir(CONST_STRPTR name) {
    BPTR lock;

    lock = Lock(name, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return TRUE;
    }

    lock = CreateDir(name);
    if (!lock) return FALSE;
    UnLock(lock);
    return TRUE;
}

/* Every hard-drive Spooler location keeps its job files in a dedicated
 * MPSPOOL drawer on that device rather than loose at the volume's root -
 * see mp_build_spool_path()/mp_build_text_spool_path() (driver_core.c/
 * command_table.c), which spool there instead of T: once SPOOL= names a
 * real device. Marked hidden (Protect's H bit - best-effort: an older
 * filesystem that ignores it just leaves the drawer visible, nothing
 * breaks) and given no .info icon, the same way the release build's own
 * driver binaries deliberately go without one - this is a working
 * directory, not something meant to be opened or double-clicked. Called
 * once per Save, from save_driver_config() below; a failure here doesn't
 * block saving the rest of Unit0 - the driver will simply fail to open
 * its job file at print time and log why, same as any other missing
 * destination. */
static void mp_ensure_hidden_spool_dir(const char *device) {
    char path[MAX_ATTR_LEN + 16];
    BPTR lock;

    if (!device || !device[0] || strcmp(device, "RAM") == 0) return;

    snprintf(path, sizeof(path), "%sMPSPOOL", device);

    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (lock) {
        UnLock(lock);
    } else {
        lock = CreateDir((CONST_STRPTR)path);
        if (!lock) {
            printf("Could not create spool directory %s\n", path);
            return;
        }
        UnLock(lock);
    }

    SetProtection((CONST_STRPTR)path, FIBF_HIDDEN);
}

static void unit_config_path(int idx, BOOL envarc, char *out, int out_size) {
    snprintf(out, out_size, "%s:MintPRINT/Unit%d", envarc ? "ENVARC" : "ENV", idx);
}

static void unit_cache_path(int idx, BOOL envarc, char *out, int out_size) {
    snprintf(out, out_size, "%s:MintPRINT/Unit%d.cache", envarc ? "ENVARC" : "ENV", idx);
}

static void unit_icon_cache_path(int idx, BOOL envarc, char *out, int out_size) {
    snprintf(out, out_size, "%s:MintPRINT/Art/Unit%d.mpic",
             envarc ? "ENVARC" : "ENV", idx);
}

static BOOL unit_file_exists(int idx) {
    BPTR lock;
    char path[64];

    unit_config_path(idx, FALSE, path, sizeof(path));
    lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!lock) {
        unit_config_path(idx, TRUE, path, sizeof(path));
        lock = Lock((CONST_STRPTR)path, ACCESS_READ);
    }
    if (lock) {
        UnLock(lock);
        return TRUE;
    }
    return FALSE;
}

/* Peeks just the MODEL= line out of a saved unit file, without disturbing
 * any of the live GUI/driver-config state. Used to label the Unit dropdown. */
static void peek_unit_model(int idx, char *out, int out_size) {
    BPTR file;
    char path[64];
    char line[192];

    out[0] = '\0';

    unit_config_path(idx, FALSE, path, sizeof(path));
    file = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!file) {
        unit_config_path(idx, TRUE, path, sizeof(path));
        file = Open((CONST_STRPTR)path, MODE_OLDFILE);
    }
    if (!file) return;

    while (FGets(file, line, sizeof(line))) {
        trim_config_line(line);
        if (strncmp(line, "MODEL=", 6) == 0 && line[6]) {
            strncpy(out, line + 6, out_size - 1);
            out[out_size - 1] = '\0';
            break;
        }
    }
    Close(file);
}

/* Rebuilds the Unit dropdown's labels from whatever is currently saved on
 * disk for each slot ("Unit0 - Brother HL-L2350DW", "Unit1 (empty)", ...).
 * Callable before the window exists (win == NULL) to seed the gadget's
 * initial GTCY_Labels, or afterwards to refresh a live gadget - e.g. after
 * Save, in case a freshly-queried make/model just got written out. Matches
 * this file's existing "leak the old label block rather than free it while
 * GadTools might still reference it" convention (see update_media_dropdown
 * and friends). */
static void refresh_unit_dropdown(struct Window *win) {
    int i;

    for (i = 0; i < MAX_UNITS; i++) {
        char model[96];

        mp_unit_label_ptrs[i] = mp_unit_label_storage[i];
        mp_unit_label_storage[i][0] = '\0';

        if (i == current_unit_index && printer_make_model[0]) {
            strncpy(model, printer_make_model, sizeof(model) - 1);
            model[sizeof(model) - 1] = '\0';
        } else {
            peek_unit_model(i, model, sizeof(model));
        }

        /* The "Unit:" gadget label already says "Unit" - repeating it in
         * every cycle entry ("Unit0 - Brother MFC-J6930DW") just wastes
         * width that a real model name badly needs. */
        if (model[0]) {
            snprintf(mp_unit_label_storage[i], MP_UNIT_LABEL_LEN,
                     "%d: %s", i, model);
        } else if (unit_file_exists(i)) {
            snprintf(mp_unit_label_storage[i], MP_UNIT_LABEL_LEN,
                     "%d", i);
        } else {
            snprintf(mp_unit_label_storage[i], MP_UNIT_LABEL_LEN,
                     "%d (empty)", i);
        }
    }
    mp_unit_label_ptrs[MAX_UNITS] = NULL;
    unit_dropdown_labels = mp_unit_label_ptrs;

    if (win) {
        struct Gadget *g = find_gadget_by_id(GAD_UNIT_DROPDOWN);
        if (g) {
            GT_SetGadgetAttrs(g, win, NULL,
                              GTCY_Labels, (ULONG)unit_dropdown_labels,
                              GTCY_Active, (ULONG)current_unit_index,
                              TAG_DONE);
            RefreshGList(g, win, NULL, 1);
            GT_RefreshWindow(win, NULL);
        }
    }
}

static const char *engine_mime_type(const char *engine) {
    if (strcmp(engine, "postscript") == 0) return "application/postscript";
    if (strcmp(engine, "pwg-raster") == 0) return "image/pwg-raster";
    if (strcmp(engine, "pdf") == 0) return "application/pdf";
    if (strcmp(engine, "urf") == 0) return "image/urf";
    return "image/jpeg";
}

/* The visible order may be filtered by a printer capability query. */
static ULONG mp_engine_active_index(void) {
    int i;

    for (i = 0; i < mp_engine_count; ++i) {
        if (mp_engine_value_map[i] &&
            strcmp(driver_engine_buffer, mp_engine_value_map[i]) == 0)
            return (ULONG)i;
    }
    return 0;
}

/* Every document-format this driver's engines can actually produce. Kept
 * in sync with engine_mime_type()'s cases. */
static const char *mp_supported_engine_mimes[] = {
    "image/jpeg", "application/postscript", "image/pwg-raster", "application/pdf",
    "image/urf"
};
#define MP_SUPPORTED_ENGINE_MIME_COUNT \
    (sizeof(mp_supported_engine_mimes) / sizeof(mp_supported_engine_mimes[0]))

/* After a successful Query, checks whether the printer advertised ANY
 * document-format this driver can actually produce. Unlike
 * warn_if_engine_unsupported() (which only flags a mismatch with the
 * currently-selected engine and is purely informational), a printer that
 * supports none of them cannot be printed to at all - a hard "this
 * printer isn't supported" finding, worth a real requester rather than a
 * status-box line easily missed among the rest of the Query output. */
static void mp_check_any_engine_supported(struct Window *win) {
    int i, j;
    BOOL any_match = FALSE;
    struct EasyStruct es;

    if (num_supported_formats == 0) return; /* printer didn't report - can't judge */

    for (i = 0; i < num_supported_formats && !any_match; i++) {
        for (j = 0; j < (int)MP_SUPPORTED_ENGINE_MIME_COUNT; j++) {
            if (strcasecmp(supported_formats[i], mp_supported_engine_mimes[j]) == 0) {
                any_match = TRUE;
                break;
            }
        }
    }

    if (any_match) return;

    es.es_StructSize = sizeof(struct EasyStruct);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)"MintPrint Settings";
    es.es_TextFormat = (UBYTE *)
        "This printer did not advertise any document format\n"
        "MintPRINT can produce JPEG, PostScript, PWG Raster, PDF,\n"
        "or Apple Raster (URF).\n\n"
        "It is likely not supported yet. To help add support,\n"
        "please log an issue at github.com/boingball/MintPRINT -\n"
        "run windows_ipp_probe.py (from a Windows PC on the same\n"
        "network) against this printer and attach its output.";
    es.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequest(win, &es, NULL);
}

/* Cross-checks the chosen engine against the formats the printer actually
 * advertised in document-format-supported (populated by a prior Query).
 * Purely informational: it does not block Save, since a printer that has
 * never been queried yet has an empty list and should not be warned about. */
static void warn_if_engine_unsupported(const char *engine) {
    const char *mime;
    int i;
    BOOL found;

    if (num_supported_formats == 0) return;

    mime = engine_mime_type(engine);
    found = FALSE;
    for (i = 0; i < num_supported_formats; i++) {
        if (strcasecmp(supported_formats[i], mime) == 0) {
            found = TRUE;
            break;
        }
    }
    if (!found) {
        custom_printf("Warning: printer did not advertise %s support for the '%s' engine\n", mime, engine);
    }
}

/* GT_GetGadgetAttrsA() is V39. GadTools STRING_KIND wraps a standard
 * Intuition StringInfo whose Buffer field is available on v37, so use that
 * directly. Cycle values are retained from IDCMP_GADGETUP message codes in
 * process_window_events(), which is the documented pre-V39 mechanism. */
static char *mp_string_gadget_value(struct Gadget *g) {
    struct StringInfo *si;

    if (!g || !g->SpecialInfo)
        return NULL;
    si = (struct StringInfo *)g->SpecialInfo;
    return si->Buffer;
}

static void capture_driver_settings(struct Window *win) {
    struct Gadget *g;
    char *value;

    if (!win) return;

    g = find_gadget_by_id(GAD_IP_STRING);
    value = mp_string_gadget_value(g);
    if (value) {
        strncpy(ip_buffer, value, sizeof(ip_buffer) - 1);
        ip_buffer[sizeof(ip_buffer) - 1] = '\0';
    }

    g = find_gadget_by_id(GAD_IPP_PATH);
    value = mp_string_gadget_value(g);
    if (value) {
        strncpy(driver_path_buffer, value, sizeof(driver_path_buffer) - 1);
        driver_path_buffer[sizeof(driver_path_buffer) - 1] = '\0';
    }

    /* Every cycle gadget updates its persisted backing value from the
     * IDCMP message code as the user changes it. */
    warn_if_engine_unsupported(driver_engine_buffer);
}

static BOOL write_driver_config_file(CONST_STRPTR filename) {
    BPTR file;
    char host[64];
    int port = -1;
    char line[192];

    if (!parse_ip_and_port(ip_buffer, host, sizeof(host), &port) || !host[0]) {
        printf("Invalid printer IPv4 address: %s\n", ip_buffer);
        return FALSE;
    }
    if (port <= 0) port = 80;
    if (port > 65535) {
        printf("Invalid printer port: %d\n", port);
        return FALSE;
    }
    if (!driver_path_buffer[0] || driver_path_buffer[0] != '/') {
        printf("IPP path must start with '/': %s\n", driver_path_buffer);
        return FALSE;
    }

    file = Open(filename, MODE_NEWFILE);
    if (!file) return FALSE;

    snprintf(line, sizeof(line), "# MintPRINT Unit%d - written by MintPrint Settings\n", current_unit_index);
    FPuts(file, line);
    snprintf(line, sizeof(line), "HOST=%s\n", host);
    FPuts(file, line);
    snprintf(line, sizeof(line), "PORT=%d\n", port);
    FPuts(file, line);
    snprintf(line, sizeof(line), "PATH=%s\n", driver_path_buffer);
    FPuts(file, line);
    snprintf(line, sizeof(line), "ENGINE=%s\n", driver_engine_buffer);
    FPuts(file, line);
    snprintf(line, sizeof(line), "DEBUG=%d\n", driver_debug ? 1 : 0);
    FPuts(file, line);
    snprintf(line, sizeof(line), "RESOLUTION=%d\n", driver_resolution);
    FPuts(file, line);
    snprintf(line, sizeof(line), "MEDIA=%s\n", driver_media_buffer);
    FPuts(file, line);
    snprintf(line, sizeof(line), "SOURCE=%s\n", driver_source_buffer);
    FPuts(file, line);
    snprintf(line, sizeof(line), "COLOR=%s\n", driver_color_buffer);
    FPuts(file, line);
    snprintf(line, sizeof(line), "QUALITY=%s\n", driver_quality_buffer);
    FPuts(file, line);
    snprintf(line, sizeof(line), "SCALING=%s\n", driver_scaling_buffer);
    FPuts(file, line);
    snprintf(line, sizeof(line), "SIDES=%s\n", driver_sides_buffer);
    FPuts(file, line);
    snprintf(line, sizeof(line), "SPOOL=%s\n", driver_spool_buffer);
    FPuts(file, line);
    snprintf(line, sizeof(line), "SPOOL_KEEP=%d\n",
             mp_spool_keep_available() && driver_spool_keep ? 1 : 0);
    FPuts(file, line);
    snprintf(line, sizeof(line), "PWG_SHEET_BACK=%s\n", pwg_sheet_back_value);
    FPuts(file, line);
    snprintf(line, sizeof(line), "MODEL=%s\n", printer_make_model);
    FPuts(file, line);
    Close(file);
    return TRUE;
}

static BOOL save_driver_config(struct Window *win) {
    BOOL env_ok;
    BOOL envarc_ok;
    char env_path[64];
    char envarc_path[64];

    capture_driver_settings(win);
    mp_ensure_hidden_spool_dir(driver_spool_buffer);

    if (!ensure_config_dir((CONST_STRPTR)"ENV:MintPRINT")) {
        printf("Could not create/find ENV:MintPRINT\n");
        return FALSE;
    }
    if (!ensure_config_dir((CONST_STRPTR)"ENVARC:MintPRINT")) {
        printf("Could not create/find ENVARC:MintPRINT\n");
        return FALSE;
    }

    unit_config_path(current_unit_index, FALSE, env_path, sizeof(env_path));
    unit_config_path(current_unit_index, TRUE, envarc_path, sizeof(envarc_path));

    env_ok = write_driver_config_file((CONST_STRPTR)env_path);
    envarc_ok = write_driver_config_file((CONST_STRPTR)envarc_path);

    /*
     * Do NOT replace the live Unit cycle gadget's GTCY_Labels here.
     *
     * On classic GadTools (OS3.1/V37), repeatedly swapping a CYCLE_KIND
     * label array while the gadget is live can leave internal gadget state
     * pointing at the old label list.  The failure shows up later during
     * GUI teardown (Save -> Exit can hard-lock the machine), not necessarily
     * at the GT_SetGadgetAttrs() call itself.
     *
     * Saving does not actually require a Unit dropdown rebuild: the current
     * Unit number is unchanged, and a freshly queried model is already
     * previewed by the query path.  Leave the existing live labels alone.
     * They will be rebuilt normally the next time Settings is launched.
     */
    (void)win;

    return env_ok && envarc_ok;
}

static BOOL load_driver_config(void) {
    BPTR file;
    char line[192];
    /* Empty, not a real address: a fresh install has no saved endpoint, and
     * defaulting to some other user's LAN printer address would make the
     * startup Query below (main()'s "if (ip_buffer[0])" refresh) probe a
     * random device on this network instead of doing nothing. */
    char host[64] = "";
    char env_path[64];
    char envarc_path[64];
    int port = 80;
    BOOL found = FALSE;

    strcpy(driver_path_buffer, "/ipp/print");
    strcpy(driver_engine_buffer, "jpeg");
    driver_engine_explicit = FALSE;
    driver_engine_pwg_offer_shown = FALSE;
    driver_debug = FALSE;
    driver_resolution = 300;
    driver_resolution_explicit = FALSE;
    driver_media_buffer[0] = '\0';
    driver_source_buffer[0] = '\0';
    driver_color_buffer[0] = '\0';
    driver_quality_buffer[0] = '\0';
    driver_scaling_buffer[0] = '\0';
    driver_sides_buffer[0] = '\0';
    strcpy(driver_spool_buffer, "RAM");
    driver_spool_keep = FALSE;
    strcpy(pwg_sheet_back_value, "normal");
    printer_make_model[0] = '\0';

    unit_config_path(current_unit_index, FALSE, env_path, sizeof(env_path));
    unit_config_path(current_unit_index, TRUE, envarc_path, sizeof(envarc_path));

    file = Open((CONST_STRPTR)env_path, MODE_OLDFILE);
    if (!file)
        file = Open((CONST_STRPTR)envarc_path, MODE_OLDFILE);

    if (!file) {
        ip_buffer[0] = '\0';
        return FALSE;
    }

    found = TRUE;
    while (FGets(file, line, sizeof(line))) {
        char *value;
        trim_config_line(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;

        if (strncmp(line, "HOST=", 5) == 0) {
            value = line + 5;
            if (*value) {
                strncpy(host, value, sizeof(host) - 1);
                host[sizeof(host) - 1] = '\0';
            }
        } else if (strncmp(line, "PORT=", 5) == 0) {
            int parsed = atoi(line + 5);
            if (parsed >= 1 && parsed <= 65535) port = parsed;
        } else if (strncmp(line, "PATH=", 5) == 0) {
            value = line + 5;
            if (*value == '/') {
                strncpy(driver_path_buffer, value, sizeof(driver_path_buffer) - 1);
                driver_path_buffer[sizeof(driver_path_buffer) - 1] = '\0';
            }
        } else if (strncmp(line, "ENGINE=", 7) == 0) {
            if (strcmp(line + 7, "pwg-raster") == 0)
                strcpy(driver_engine_buffer, "pwg-raster");
            else if (strcmp(line + 7, "pdf") == 0)
                strcpy(driver_engine_buffer, "pdf");
            else if (strcmp(line + 7, "postscript") == 0)
                strcpy(driver_engine_buffer, "postscript");
            else if (strcmp(line + 7, "urf") == 0)
                strcpy(driver_engine_buffer, "urf");
            else
                strcpy(driver_engine_buffer, "jpeg");
            /* Matches driver_resolution_explicit's own precedent just above:
             * once a value has actually been saved, treat it as pinned
             * rather than re-deriving it from capabilities on every load -
             * consistent behaviour for both settings, and avoids silently
             * flipping a printer that was deliberately kept on JPEG (e.g.
             * a real PWG rendering bug on that model) back to PWG Raster
             * behind the user's back on a later Query. */
            driver_engine_explicit = TRUE;
        } else if (strncmp(line, "DEBUG=", 6) == 0) {
            driver_debug = (line[6] == '0') ? FALSE : TRUE;
        } else if (strncmp(line, "KEEPJOB=", 8) == 0) {
            /* Backward compatibility: the old diagnostic-artifact setting
             * maps directly to the new, broader Debug switch. */
            driver_debug = (line[8] == '0') ? FALSE : TRUE;
        } else if (strncmp(line, "RESOLUTION=", 11) == 0) {
            driver_resolution = (atoi(line + 11) == 600) ? 600 : 300;
            driver_resolution_explicit = TRUE;
        } else if (strncmp(line, "MEDIA=", 6) == 0) {
            strncpy(driver_media_buffer, line + 6, sizeof(driver_media_buffer) - 1);
            driver_media_buffer[sizeof(driver_media_buffer) - 1] = '\0';
        } else if (strncmp(line, "SOURCE=", 7) == 0) {
            strncpy(driver_source_buffer, line + 7, sizeof(driver_source_buffer) - 1);
            driver_source_buffer[sizeof(driver_source_buffer) - 1] = '\0';
        } else if (strncmp(line, "COLOR=", 6) == 0) {
            strncpy(driver_color_buffer, line + 6, sizeof(driver_color_buffer) - 1);
            driver_color_buffer[sizeof(driver_color_buffer) - 1] = '\0';
        } else if (strncmp(line, "QUALITY=", 8) == 0) {
            strncpy(driver_quality_buffer, line + 8, sizeof(driver_quality_buffer) - 1);
            driver_quality_buffer[sizeof(driver_quality_buffer) - 1] = '\0';
        } else if (strncmp(line, "SCALING=", 8) == 0) {
            strncpy(driver_scaling_buffer, line + 8, sizeof(driver_scaling_buffer) - 1);
            driver_scaling_buffer[sizeof(driver_scaling_buffer) - 1] = '\0';
        } else if (strncmp(line, "SIDES=", 6) == 0) {
            const char *sides = line + 6;
            if (strcmp(sides, "one-sided") == 0 ||
                strcmp(sides, "two-sided-long-edge") == 0 ||
                strcmp(sides, "two-sided-short-edge") == 0) {
                strncpy(driver_sides_buffer, sides,
                        sizeof(driver_sides_buffer) - 1);
                driver_sides_buffer[sizeof(driver_sides_buffer) - 1] = '\0';
            }
        } else if (strncmp(line, "SPOOL=", 6) == 0) {
            const char *spool = line + 6;
            if (spool[0]) {
                strncpy(driver_spool_buffer, spool,
                        sizeof(driver_spool_buffer) - 1);
                driver_spool_buffer[sizeof(driver_spool_buffer) - 1] = '\0';
            }
        } else if (strncmp(line, "SPOOL_KEEP=", 11) == 0) {
            driver_spool_keep = (line[11] == '0') ? FALSE : TRUE;
        } else if (strncmp(line, "PWG_SHEET_BACK=", 15) == 0) {
            const char *sheet_back = line + 15;
            if (strcmp(sheet_back, "normal") == 0 ||
                strcmp(sheet_back, "rotated") == 0 ||
                strcmp(sheet_back, "flipped") == 0 ||
                strcmp(sheet_back, "manual-tumble") == 0) {
                strncpy(pwg_sheet_back_value, sheet_back,
                        sizeof(pwg_sheet_back_value) - 1);
                pwg_sheet_back_value[sizeof(pwg_sheet_back_value) - 1] = '\0';
            }
        } else if (strncmp(line, "MODEL=", 6) == 0) {
            strncpy(printer_make_model, line + 6, sizeof(printer_make_model) - 1);
            printer_make_model[sizeof(printer_make_model) - 1] = '\0';
        }
    }

    Close(file);
    if (host[0])
        snprintf(ip_buffer, sizeof(ip_buffer), "%s:%d", host, port);
    else
        ip_buffer[0] = '\0';
    return found;
}

/* TRUE for a raw IPP/PWG5100.3 self-describing keyword: lowercase letters,
 * digits, '-', '.' and '_' only. Real keywords are always shaped like this
 * ("iso_a4_210x297mm", "by-pass-tray", "tray-1"); anything with a space
 * or an uppercase letter is already a human-readable name - either
 * printer-supplied (trayname=/medianame= from a vendor's media-col
 * response) or one of this program's own "AUTO"/"Unknown" fallbacks -
 * and mp_pretty_media_size()/mp_pretty_tray_name() below leave it alone
 * rather than risk mangling it. */
static BOOL mp_looks_like_raw_ipp_keyword(const char *s) {
    int i;
    if (!s || !s[0]) return FALSE;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.' || c == '_'))
            return FALSE;
    }
    return TRUE;
}

/* Capitalises each hyphen-separated word of a raw keyword's middle
 * segment, turning hyphens into spaces - shared by
 * mp_pretty_media_size() and mp_pretty_tray_name() below. Caller
 * guarantees len < out_size. */
static void mp_pretty_words(const char *start, size_t len, char *out) {
    size_t i, oi = 0;
    BOOL cap_next = TRUE;
    for (i = 0; i < len; i++) {
        char c = start[i];
        if (c == '-') {
            out[oi++] = ' ';
            cap_next = TRUE;
        } else if (cap_next) {
            out[oi++] = (char)toupper((unsigned char)c);
            cap_next = FALSE;
        } else {
            out[oi++] = c;
        }
    }
    out[oi] = '\0';
}

/* PWG5100.3 self-describing media names look like
 * "<region>_<name>_<dims><unit>", e.g. "iso_a4_210x297mm" or
 * "na_number-10-envelope_4.125x9.5in" - keep just the <name> segment,
 * the part a person actually recognises, and drop the region prefix and
 * exact millimetre/inch dimensions, which are just noise in a dropdown
 * ("A4" instead of "iso_a4_210x297mm"). Falls back to the raw string
 * unchanged if it doesn't look like a raw keyword, or doesn't have the
 * expected two-underscore shape, so nothing already friendly gets
 * mangled. */
static void mp_pretty_media_size(const char *raw, char *out, size_t out_size) {
    const char *first_us, *last_us, *name_start;
    size_t name_len;
    char buf[MAX_ATTR_LEN];

    if (!mp_looks_like_raw_ipp_keyword(raw)) {
        snprintf(out, out_size, "%s", raw ? raw : "");
        return;
    }

    first_us = strchr(raw, '_');
    last_us = strrchr(raw, '_');
    if (!first_us || !last_us || first_us == last_us) {
        snprintf(out, out_size, "%s", raw);
        return;
    }

    name_start = first_us + 1;
    name_len = (size_t)(last_us - name_start);
    if (name_len == 0 || name_len >= sizeof(buf)) {
        snprintf(out, out_size, "%s", raw);
        return;
    }

    mp_pretty_words(name_start, name_len, buf);
    snprintf(out, out_size, "%s", buf);
}

/* PWG5100.3 media-source keywords, prettified for the dropdown. Only two
 * need a special case - the rest ("tray-1", "roll-2", "envelope-manual",
 * "large-capacity", ...) already read fine from the same hyphen-to-space,
 * capitalise-each-word transform mp_pretty_media_size() uses above.
 * Falls back to the raw string unchanged if it doesn't look like a raw
 * keyword - see mp_looks_like_raw_ipp_keyword(). */
static void mp_pretty_tray_name(const char *raw, char *out, size_t out_size) {
    char buf[MAX_ATTR_LEN];
    size_t len;

    if (!mp_looks_like_raw_ipp_keyword(raw)) {
        snprintf(out, out_size, "%s", raw ? raw : "");
        return;
    }
    if (strcmp(raw, "auto") == 0) {
        snprintf(out, out_size, "AUTO");
        return;
    }
    if (strcmp(raw, "by-pass-tray") == 0 || strcmp(raw, "bypass-tray") == 0) {
        snprintf(out, out_size, "Bypass Tray");
        return;
    }

    len = strlen(raw);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    mp_pretty_words(raw, len, buf);
    snprintf(out, out_size, "%s", buf);
}

static void seed_saved_option_labels(void) {
    if (driver_media_buffer[0]) {
        char media_pretty[MAX_ATTR_LEN];

        mp_pretty_media_size(driver_media_buffer, media_pretty, sizeof(media_pretty));
        if (driver_source_buffer[0]) {
            char tray_pretty[MAX_ATTR_LEN];

            mp_pretty_tray_name(driver_source_buffer, tray_pretty, sizeof(tray_pretty));
            snprintf(initial_media_value, sizeof(initial_media_value), "%s (%s)",
                     media_pretty, tray_pretty);
        } else {
            snprintf(initial_media_value, sizeof(initial_media_value), "%s",
                     media_pretty);
        }
    } else {
        strcpy(initial_media_value, "Not Detected");
    }

    if (driver_color_buffer[0])
        snprintf(initial_print_mode_value, sizeof(initial_print_mode_value), "%s",
                 driver_color_buffer);
    else
        strcpy(initial_print_mode_value, "Not Detected");

    if (driver_scaling_buffer[0])
        snprintf(initial_scaling_value, sizeof(initial_scaling_value), "%s",
                 driver_scaling_buffer);
    else
        strcpy(initial_scaling_value, "Not Detected");

    if (driver_quality_buffer[0])
        snprintf(initial_quality_value, sizeof(initial_quality_value), "%s",
                 driver_quality_buffer);
    else
        strcpy(initial_quality_value, "Not Detected");

    snprintf(initial_dpi_value, sizeof(initial_dpi_value), "%d dpi",
             driver_resolution);
    mp_dpi_options.values[0] = driver_resolution;
    mp_dpi_options.compatibility[0] = 0;
    mp_dpi_options.count = 1;
    mp_dpi_options.active = 0;
    mp_dpi_options.selected = driver_resolution;

    mp_media_label_ptrs[0] = mp_media_label_storage[0];
    strncpy(mp_media_label_storage[0], initial_media_value,
            sizeof(mp_media_label_storage[0]) - 1);
    mp_media_label_storage[0][sizeof(mp_media_label_storage[0]) - 1] = '\0';
    mp_media_label_ptrs[1] = NULL;

    mp_print_mode_label_ptrs[0] = mp_print_mode_label_storage[0];
    strncpy(mp_print_mode_label_storage[0], initial_print_mode_value,
            sizeof(mp_print_mode_label_storage[0]) - 1);
    mp_print_mode_label_storage[0][sizeof(mp_print_mode_label_storage[0]) - 1] = '\0';
    mp_print_mode_label_ptrs[1] = NULL;

    mp_scaling_label_ptrs[0] = mp_scaling_label_storage[0];
    strncpy(mp_scaling_label_storage[0], initial_scaling_value,
            sizeof(mp_scaling_label_storage[0]) - 1);
    mp_scaling_label_storage[0][sizeof(mp_scaling_label_storage[0]) - 1] = '\0';
    mp_scaling_label_ptrs[1] = NULL;

    mp_quality_label_ptrs[0] = mp_quality_label_storage[0];
    strncpy(mp_quality_label_storage[0], initial_quality_value,
            sizeof(mp_quality_label_storage[0]) - 1);
    mp_quality_label_storage[0][sizeof(mp_quality_label_storage[0]) - 1] = '\0';
    mp_quality_label_ptrs[1] = NULL;

    mp_dpi_label_ptrs[0] = mp_dpi_label_storage[0];
    strncpy(mp_dpi_label_storage[0], initial_dpi_value,
            sizeof(mp_dpi_label_storage[0]) - 1);
    mp_dpi_label_storage[0][sizeof(mp_dpi_label_storage[0]) - 1] = '\0';
    mp_dpi_label_ptrs[1] = NULL;

    mp_sides_label_ptrs[0] = mp_sides_label_storage[0];
    strcpy(mp_sides_label_storage[0], "One-sided");
    strcpy(mp_sides_value_storage[0], "one-sided");
    mp_sides_label_ptrs[1] = NULL;
    mp_sides_option_count = 1;

    media_dropdown_items = mp_media_label_ptrs;
    print_mode_labels = mp_print_mode_label_ptrs;
    scaling_mode_labels = mp_scaling_label_ptrs;
    quality_mode_labels = mp_quality_label_ptrs;
    resolution_labels = mp_dpi_label_ptrs;
}

static void apply_saved_option_state(struct Window *win) {
    struct Gadget *g;

    if (!win) return;
    seed_saved_option_labels();

    if (num_media_tray_mappings == 0) {
        g = find_gadget_by_id(GAD_MEDIA_DROPDOWN);
        if (g)
            GT_SetGadgetAttrs(g, win, NULL,
                              GTCY_Labels, (ULONG)media_dropdown_items,
                              GTCY_Active, 0,
                              GA_Disabled, driver_media_buffer[0] ? FALSE : TRUE,
                              TAG_DONE);
    }

    if (num_supported_scaling == 0) {
        g = find_gadget_by_id(GAD_SCALING_MODE);
        if (g)
            GT_SetGadgetAttrs(g, win, NULL,
                              GTCY_Labels, (ULONG)scaling_mode_labels,
                              GTCY_Active, 0,
                              GA_Disabled, driver_scaling_buffer[0] ? FALSE : TRUE,
                              TAG_DONE);
    }

    if (num_supported_quality == 0) {
        g = find_gadget_by_id(GAD_QUALITY_MODE);
        if (g)
            GT_SetGadgetAttrs(g, win, NULL,
                              GTCY_Labels, (ULONG)quality_mode_labels,
                              GTCY_Active, 0,
                              GA_Disabled, driver_quality_buffer[0] ? FALSE : TRUE,
                              TAG_DONE);
    }

    if (num_supported_dpi == 0) {
        g = find_gadget_by_id(GAD_RESOLUTION);
        if (g)
            GT_SetGadgetAttrs(g, win, NULL,
                              GTCY_Labels, (ULONG)resolution_labels,
                              GTCY_Active, 0,
                              GA_Disabled, TRUE,
                              TAG_DONE);
    }

    if (num_supported_print_modes == 0) {
        g = find_gadget_by_id(GAD_PRINT_MODE);
        if (g)
            GT_SetGadgetAttrs(g, win, NULL,
                              GTCY_Labels, (ULONG)print_mode_labels,
                              GTCY_Active, 0,
                              GA_Disabled, driver_color_buffer[0] ? FALSE : TRUE,
                              TAG_DONE);
    }

    g = find_gadget_by_id(GAD_SIDES);
    if (g)
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Labels, (ULONG)mp_sides_label_ptrs,
                          GTCY_Active, 0,
                          GA_Disabled, TRUE,
                          TAG_DONE);

    GT_RefreshWindow(win, NULL);
}

static void apply_job_defaults_to_gadgets(struct Window *win) {
    struct Gadget *g;
    int i;

    if (!win) return;

    if (media_dropdown && num_media_tray_mappings > 0 && driver_media_buffer[0]) {
        for (i = 0; i < num_media_tray_mappings; ++i) {
            if (strcmp(media_tray_map[i].media, driver_media_buffer) == 0 &&
                (!driver_source_buffer[0] ||
                 strcmp(media_tray_map[i].source, driver_source_buffer) == 0)) {
                GT_SetGadgetAttrs(media_dropdown, win, NULL,
                                  GTCY_Active, (ULONG)i,
                                  TAG_DONE);
                break;
            }
        }
    }

    g = find_gadget_by_id(GAD_PRINT_MODE);
    if (g && driver_color_buffer[0]) {
        for (i = 0; i < num_supported_print_modes; ++i) {
            if (strcmp(supported_print_modes[i], driver_color_buffer) == 0) {
                GT_SetGadgetAttrs(g, win, NULL, GTCY_Active, (ULONG)i, TAG_DONE);
                strncpy(selected_print_mode, supported_print_modes[i],
                        sizeof(selected_print_mode) - 1);
                selected_print_mode[sizeof(selected_print_mode) - 1] = '\0';
                break;
            }
        }
    }

    g = find_gadget_by_id(GAD_SCALING_MODE);
    if (g && driver_scaling_buffer[0]) {
        for (i = 0; i < num_supported_scaling; ++i) {
            if (strcmp(supported_scaling[i], driver_scaling_buffer) == 0) {
                GT_SetGadgetAttrs(g, win, NULL, GTCY_Active, (ULONG)i, TAG_DONE);
                strncpy(selected_scaling, supported_scaling[i], sizeof(selected_scaling) - 1);
                selected_scaling[sizeof(selected_scaling) - 1] = '\0';
                break;
            }
        }
    }

    g = find_gadget_by_id(GAD_QUALITY_MODE);
    if (g && driver_quality_buffer[0]) {
        for (i = 0; i < num_supported_quality; ++i) {
            if (strcmp(supported_quality[i], driver_quality_buffer) == 0) {
                GT_SetGadgetAttrs(g, win, NULL, GTCY_Active, (ULONG)i, TAG_DONE);
                strncpy(selected_quality, supported_quality[i], sizeof(selected_quality) - 1);
                selected_quality[sizeof(selected_quality) - 1] = '\0';
                break;
            }
        }
    }

    g = find_gadget_by_id(GAD_SIDES);
    if (g)
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Active, mp_sides_active_index(),
                          TAG_DONE);

    GT_RefreshWindow(win, NULL);
}

static void mp_set_test_print_enabled(struct Window *win, BOOL enabled)
{
    struct Gadget *g = find_gadget_by_id(GAD_PRINT_BUTTON);
    if (g && win) {
        GT_SetGadgetAttrs(g, win, NULL,
                          GA_Disabled, enabled ? FALSE : TRUE,
                          TAG_DONE);
    }
}

/* Keeps the Keep Spooled Jobs checkbox's enabled/ticked state in step
 * with the Spooler cycle - called right after driver_spool_buffer
 * changes (GAD_SPOOLER's handler below) and once at startup
 * (apply_driver_config_to_gadgets()). Forces driver_spool_keep off the
 * moment Spooler no longer names a real device, rather than leaving a
 * stale tick nobody can see or clear on a disabled gadget - see
 * mp_spool_keep_available(). */
static void mp_update_spool_keep_gadget(struct Window *win)
{
    struct Gadget *g = find_gadget_by_id(GAD_SPOOL_KEEP);
    BOOL available = mp_spool_keep_available();

    if (!available) driver_spool_keep = FALSE;

    if (g && win) {
        GT_SetGadgetAttrs(g, win, NULL,
                          GA_Disabled, (ULONG)(available ? FALSE : TRUE),
                          GTCB_Checked, (ULONG)(available && driver_spool_keep),
                          TAG_DONE);
    }
}

static void mp_test_print_release(struct Window *win)
{
    if (test_print_job.request && test_print_job.device_open) {
        CloseDevice((struct IORequest *)test_print_job.request);
        test_print_job.device_open = FALSE;
    }
    if (test_print_job.request) {
        DeleteIORequest((struct IORequest *)test_print_job.request);
        test_print_job.request = NULL;
    }
    if (test_print_job.port) {
        DeleteMsgPort(test_print_job.port);
        test_print_job.port = NULL;
    }
    if (test_print_job.bitmap) {
        if (test_print_job.bitmap_manual) {
            int plane;
            for (plane = 0;
                 plane < (int)test_print_job.bitmap_storage.Depth;
                 ++plane) {
                if (test_print_job.bitmap_storage.Planes[plane]) {
                    FreeRaster(test_print_job.bitmap_storage.Planes[plane],
                               (ULONG)test_print_job.bitmap_storage.BytesPerRow * 8UL,
                               (ULONG)test_print_job.bitmap_storage.Rows);
                    test_print_job.bitmap_storage.Planes[plane] = NULL;
                }
            }
        } else {
            FreeBitMap(test_print_job.bitmap);
        }
        test_print_job.bitmap = NULL;
        test_print_job.bitmap_manual = FALSE;
    }
    if (test_print_job.colormap) {
        FreeColorMap(test_print_job.colormap);
        test_print_job.colormap = NULL;
    }
    test_print_job.active = FALSE;
    mp_set_test_print_enabled(win, TRUE);
}

static void mp_test_print_complete(struct Window *win)
{
    LONG ioerr;

    if (!test_print_job.active || !test_print_job.request) return;
    ioerr = WaitIO((struct IORequest *)test_print_job.request);
    if (ioerr != 0 || test_print_job.request->io_Error != 0) {
        printf("Test Print failed: WaitIO=%ld io_Error=%ld\n",
               ioerr, (LONG)test_print_job.request->io_Error);
    } else {
        printf("Test Print completed successfully\n");
    }
    mp_test_print_release(win);
}

static void mp_test_print_cancel(struct Window *win)
{
    if (!test_print_job.active || !test_print_job.request) return;
    printf("Cancelling Test Print...\n");
    if (!CheckIO((struct IORequest *)test_print_job.request))
        AbortIO((struct IORequest *)test_print_job.request);
    WaitIO((struct IORequest *)test_print_job.request);
    mp_test_print_release(win);
}

/* Portrait, A4-proportioned (210x297mm) test canvas, drawn into its own
 * private bitmap and 8-colour ColorMap rather than the live Workbench
 * screen's.
 *
 * This is the original pre-PR38 320x453 layout, restored: real testing of
 * PR38's smaller 240x320 live-palette canvas showed the same left-margin/
 * positioning regression on both PWG Raster and JPEG Test Print, which
 * means it's an upstream printer.device DUMPRPORT geometry quirk tied to
 * that canvas's own dimensions/aspect - not something specific to any one
 * MintPRINT encoder. Going back to the previously-proven 320x453 source
 * (kept identical for JPEG, PWG Raster and PDF - same bitmap, same source
 * dimensions, same configured-media DestCols/DestRows, same
 * SPECIAL_ASPECT|SPECIAL_CENTER) sidesteps it again.
 *
 * The private ColorMap exists because pen 0 on a live Workbench screen is
 * whatever grey the user's background happens to be, not white - dumping
 * the screen's own pens printed a full page of grey ink. Pen 0 here is
 * fixed to true white and pen 1 to true black, independent of the user's
 * screen/theme, and pens 2-7 are fixed primaries for the colour test.
 *
 * printer.device still runs the DUMPRPORT request asynchronously
 * (test_print_job, above) so the GUI stays responsive; mp_test_print_complete()
 * (called from the main event loop) and mp_test_print_cancel() (called on
 * window close) finish the job and release its resources, including the
 * ColorMap allocated here. */
#define MP_TESTPAGE_WIDTH  320
#define MP_TESTPAGE_HEIGHT 453
#define MP_TESTPAGE_DEPTH  3   /* 8 pens: 2^3 */
#define MP_TESTPAGE_COLORS 8

/* PostScript alone still gets an exact small physical target
 * (SPECIAL_MILCOLS/MILROWS, no SPECIAL_CENTER) since the PostScript writer
 * centres the image on /PageSize itself; asking printer.device to also
 * centre it left 983 blank raster columns on a real Samsung capture, and
 * the smaller target keeps the 300 DPI encoder input down. The target
 * keeps the restored 320:453 source's own aspect ratio (320/453 =
 * 0.7064) rather than the previous 240:320 canvas's 3:4 (0.75) - printing
 * to a mismatched aspect would itself reintroduce cropping/positioning
 * error independent of the printer.device quirk above. */
#define MP_TEST_PS_WIDTH_MILS  4200
#define MP_TEST_PS_HEIGHT_MILS 5940

static const char *mp_test_print_engine_name(void)
{
    int i;

    for (i = 0; i < MP_ENGINE_MAX; ++i) {
        if (strcmp(driver_engine_buffer, mp_engine_all_values[i]) == 0)
            return mp_engine_all_labels[i];
    }
    return driver_engine_buffer[0] ? driver_engine_buffer : "unknown";
}

static BOOL mintprint_test_page(struct Window *win) {
    ULONG mode_id = 0;
    LONG left = 16, right = MP_TESTPAGE_WIDTH - 17;
    LONG swatch_area, swatch_w, i;
    unsigned long media_w_100mm, media_h_100mm;
    struct MPDriverVersion installed_ver = {0, 0};
    BOOL have_installed_ver;
    UWORD tw;
    const char *title = "MintPRINT";
    const char *tagline = "Network Printer Test Page";
    const char *colour_label = "Colour Test";
    const char *settings_label = "Version & Settings";
    const char *footer1 = "printer.device -> MintPRINT -> IPP";
    const char *footer2 = "github.com/boingball/MintPRINT";
    static const char *swatch_names[MP_TESTPAGE_COLORS] =
        { "Wht", "Blk", "Red", "Grn", "Blu", "Cyn", "Mag", "Yel" };
    char info_lines[9][80];
    int num_info_lines = 0;
    BOOL is_postscript;

    if (test_print_job.active) {
        printf("Test Print is already running\n");
        return FALSE;
    }

    if (!screen) {
        printf("Test Print: public screen is not available\n");
        return FALSE;
    }

    /* Test the settings the user is looking at, and make them the live Unit0. */
    if (!save_driver_config(win)) {
        printf("Test Print: could not save Unit0 settings\n");
        return FALSE;
    }

    have_installed_ver = mp_read_driver_version(MINTPRINT_DRIVER_DEST, &installed_ver);

    snprintf(info_lines[num_info_lines++], sizeof(info_lines[0]),
             "Unit%d  |  Settings v%s", current_unit_index, MINTPRINT_SETTINGS_VERSION);
    if (have_installed_ver)
        snprintf(info_lines[num_info_lines++], sizeof(info_lines[0]),
                 "Driver: v%u.%u installed",
                 (unsigned)installed_ver.version, (unsigned)installed_ver.revision);
    else
        snprintf(info_lines[num_info_lines++], sizeof(info_lines[0]),
                 "Driver: not installed");
    snprintf(info_lines[num_info_lines++], sizeof(info_lines[0]),
             "Printer: %s", printer_make_model[0] ? printer_make_model : "(unknown model)");
    snprintf(info_lines[num_info_lines++], sizeof(info_lines[0]),
             "Host: %s%s", ip_buffer, driver_path_buffer);
    snprintf(info_lines[num_info_lines++], sizeof(info_lines[0]),
             "Format: %s   DPI: %d", driver_engine_buffer, driver_resolution);
    snprintf(info_lines[num_info_lines++], sizeof(info_lines[0]),
             "Media: %s   Source: %s",
             driver_media_buffer[0] ? driver_media_buffer : "auto",
             driver_source_buffer[0] ? driver_source_buffer : "auto");
    snprintf(info_lines[num_info_lines++], sizeof(info_lines[0]),
             "Colour: %s   Quality: %s",
             driver_color_buffer[0] ? driver_color_buffer : "auto",
             driver_quality_buffer[0] ? driver_quality_buffer : "auto");
    snprintf(info_lines[num_info_lines++], sizeof(info_lines[0]),
             "Sides: %s   Scaling: %s",
             driver_sides_buffer[0] ? driver_sides_buffer : "one-sided",
             driver_scaling_buffer[0] ? driver_scaling_buffer : "auto");
    snprintf(info_lines[num_info_lines++], sizeof(info_lines[0]),
             "Debug: %s", driver_debug ? "on" : "off");

    test_print_job.colormap = GetColorMap(MP_TESTPAGE_COLORS);
    if (!test_print_job.colormap) {
        printf("Test Print: could not allocate colour map\n");
        return FALSE;
    }
    SetRGB4CM(test_print_job.colormap, 0, 15, 15, 15); /* white - paper/background */
    SetRGB4CM(test_print_job.colormap, 1, 0, 0, 0);    /* black - ink/text/border */
    SetRGB4CM(test_print_job.colormap, 2, 15, 0, 0);   /* red */
    SetRGB4CM(test_print_job.colormap, 3, 0, 15, 0);   /* green */
    SetRGB4CM(test_print_job.colormap, 4, 0, 0, 15);   /* blue */
    SetRGB4CM(test_print_job.colormap, 5, 0, 15, 15);  /* cyan */
    SetRGB4CM(test_print_job.colormap, 6, 15, 0, 15);  /* magenta */
    SetRGB4CM(test_print_job.colormap, 7, 15, 15, 0);  /* yellow */

    /* AllocBitMap()/FreeBitMap() were added in graphics.library V39.
     * On V37 build the same ordinary planar bitmap from the original
     * InitBitMap()/AllocRaster() API. Keep AllocBitMap on V39+ so newer
     * systems retain their normal graphics.library allocation path. */
    test_print_job.bitmap_manual = FALSE;
    if (GfxBase->LibNode.lib_Version >= 39) {
        test_print_job.bitmap = AllocBitMap(MP_TESTPAGE_WIDTH,
                                            MP_TESTPAGE_HEIGHT,
                                            MP_TESTPAGE_DEPTH,
                                            BMF_CLEAR, NULL);
    } else {
        int plane;

        memset(&test_print_job.bitmap_storage, 0,
               sizeof(test_print_job.bitmap_storage));
        InitBitMap(&test_print_job.bitmap_storage, MP_TESTPAGE_DEPTH,
                   MP_TESTPAGE_WIDTH, MP_TESTPAGE_HEIGHT);
        test_print_job.bitmap = &test_print_job.bitmap_storage;
        test_print_job.bitmap_manual = TRUE;

        for (plane = 0; plane < MP_TESTPAGE_DEPTH; ++plane) {
            PLANEPTR raster = AllocRaster(MP_TESTPAGE_WIDTH,
                                          MP_TESTPAGE_HEIGHT);
            if (!raster)
                break;
            memset(raster, 0, RASSIZE(MP_TESTPAGE_WIDTH,
                                     MP_TESTPAGE_HEIGHT));
            test_print_job.bitmap_storage.Planes[plane] = raster;
        }
        if (plane != MP_TESTPAGE_DEPTH) {
            printf("Test Print: could not allocate V37 bitmap plane %d\n",
                   plane);
            mp_test_print_release(win);
            return FALSE;
        }
        printf("Test Print: using V37 planar bitmap allocation\n");
    }
    if (!test_print_job.bitmap) {
        printf("Test Print: could not allocate test bitmap\n");
        mp_test_print_release(win);
        return FALSE;
    }

    InitRastPort(&test_print_job.rastport);
    test_print_job.rastport.BitMap = test_print_job.bitmap;
    if (screen->RastPort.Font) SetFont(&test_print_job.rastport, screen->RastPort.Font);

    /* Page border. */
    SetAPen(&test_print_job.rastport, 1);
    RectFill(&test_print_job.rastport, 4, 4, MP_TESTPAGE_WIDTH - 5, 5);
    RectFill(&test_print_job.rastport, 4, MP_TESTPAGE_HEIGHT - 6, MP_TESTPAGE_WIDTH - 5, MP_TESTPAGE_HEIGHT - 5);
    RectFill(&test_print_job.rastport, 4, 4, 5, MP_TESTPAGE_HEIGHT - 5);
    RectFill(&test_print_job.rastport, MP_TESTPAGE_WIDTH - 6, 4, MP_TESTPAGE_WIDTH - 5, MP_TESTPAGE_HEIGHT - 5);

    /* Header - a faux-bold double-strike stands in for a real logo bitmap. */
    Move(&test_print_job.rastport, left, 24);
    Text(&test_print_job.rastport, (STRPTR)title, strlen(title));
    Move(&test_print_job.rastport, left + 1, 24);
    Text(&test_print_job.rastport, (STRPTR)title, strlen(title));
    Move(&test_print_job.rastport, left, 40);
    Text(&test_print_job.rastport, (STRPTR)tagline, strlen(tagline));
    RectFill(&test_print_job.rastport, left, 52, right, 53);

    Move(&test_print_job.rastport, left, 70);
    Text(&test_print_job.rastport, (STRPTR)colour_label, strlen(colour_label));

    swatch_area = right - left + 1;
    swatch_w = swatch_area / MP_TESTPAGE_COLORS;
    for (i = 0; i < MP_TESTPAGE_COLORS; i++) {
        LONG x0 = left + i * swatch_w;
        LONG x1 = (i == MP_TESTPAGE_COLORS - 1) ? right : (x0 + swatch_w - 3);
        if (x1 < x0) x1 = x0;

        /* A 1px black frame drawn slightly larger than the fill keeps the
         * white swatch visible against the equally white page background. */
        SetAPen(&test_print_job.rastport, 1);
        RectFill(&test_print_job.rastport, x0 - 1, 79, x1 + 1, 171);
        SetAPen(&test_print_job.rastport, (UBYTE)i);
        RectFill(&test_print_job.rastport, x0, 80, x1, 170);

        SetAPen(&test_print_job.rastport, 1);
        tw = TextLength(&test_print_job.rastport, (STRPTR)swatch_names[i], strlen(swatch_names[i]));
        Move(&test_print_job.rastport, x0 + ((x1 - x0 + 1 - tw) / 2), 182);
        Text(&test_print_job.rastport, (STRPTR)swatch_names[i], strlen(swatch_names[i]));
    }

    SetAPen(&test_print_job.rastport, 1);
    RectFill(&test_print_job.rastport, left, 196, right, 197);

    Move(&test_print_job.rastport, left, 214);
    Text(&test_print_job.rastport, (STRPTR)settings_label, strlen(settings_label));
    for (i = 0; i < num_info_lines; i++) {
        Move(&test_print_job.rastport, left, 232 + i * 20);
        Text(&test_print_job.rastport, (STRPTR)info_lines[i], strlen(info_lines[i]));
    }

    RectFill(&test_print_job.rastport, left, 404, right, 405);
    Move(&test_print_job.rastport, left, 420);
    Text(&test_print_job.rastport, (STRPTR)footer1, strlen(footer1));
    Move(&test_print_job.rastport, left, 434);
    Text(&test_print_job.rastport, (STRPTR)footer2, strlen(footer2));

    test_print_job.port = CreateMsgPort();
    if (!test_print_job.port) {
        printf("Test Print: CreateMsgPort failed\n");
        mp_test_print_release(win);
        return FALSE;
    }

    test_print_job.request = (struct IODRPReq *)CreateIORequest(
        test_print_job.port, sizeof(struct IODRPReq));
    if (!test_print_job.request) {
        printf("Test Print: CreateIORequest failed\n");
        mp_test_print_release(win);
        return FALSE;
    }

    if (OpenDevice((CONST_STRPTR)"printer.device", 0,
                   (struct IORequest *)test_print_job.request, 0) != 0) {
        printf("Test Print: could not open printer.device\n");
        mp_test_print_release(win);
        return FALSE;
    }
    test_print_job.device_open = TRUE;

    mode_id = GetVPModeID(&screen->ViewPort);
    if (mode_id == INVALID_ID) mode_id = 0;

    test_print_job.request->io_Command = PRD_DUMPRPORT;
    test_print_job.request->io_RastPort = &test_print_job.rastport;
    test_print_job.request->io_ColorMap = test_print_job.colormap;
    test_print_job.request->io_Modes = mode_id;
    test_print_job.request->io_SrcX = 0;
    test_print_job.request->io_SrcY = 0;
    test_print_job.request->io_SrcWidth = MP_TESTPAGE_WIDTH;
    test_print_job.request->io_SrcHeight = MP_TESTPAGE_HEIGHT;

    is_postscript = strcmp(driver_engine_buffer, "postscript") == 0;
    if (is_postscript) {
        /* Give printer.device an exact portrait size and do not ask it to
         * centre the dump - see the MP_TEST_PS_WIDTH_MILS comment above. */
        test_print_job.request->io_DestCols = MP_TEST_PS_WIDTH_MILS;
        test_print_job.request->io_DestRows = MP_TEST_PS_HEIGHT_MILS;
        test_print_job.request->io_Special =
            SPECIAL_MILCOLS | SPECIAL_MILROWS;
    } else {
        /* JPEG, PWG Raster, PDF and Apple Raster (URF) all reach this
         * branch identically: same source bitmap, same source dimensions,
         * same configured-media-derived destination, same
         * SPECIAL_ASPECT|SPECIAL_CENTER.
         * printer.device does not derive the destination from the
         * configured media when DestCols/DestRows are left at 0 - a real
         * test print showed ~3113x3015px, unrelated to iso_a4_210x297mm -
         * so compute it explicitly instead. */
        ULONG dpi = (driver_resolution > 0) ? (ULONG)driver_resolution : 300UL;
        if (!driver_media_buffer[0] ||
            !mp_media_dimensions_100mm(driver_media_buffer,
                                       &media_w_100mm, &media_h_100mm)) {
            media_w_100mm = 21000UL; /* A4 210mm fallback */
            media_h_100mm = 29700UL; /* A4 297mm fallback */
        }
        test_print_job.request->io_DestCols =
            (LONG)((media_w_100mm * dpi + 1270UL) / 2540UL);
        test_print_job.request->io_DestRows =
            (LONG)((media_h_100mm * dpi + 1270UL) / 2540UL);
        test_print_job.request->io_Special = SPECIAL_ASPECT | SPECIAL_CENTER;
    }

    printf("Test Print: sending page through printer.device (dest %ld x %ld)...\n",
           (long)test_print_job.request->io_DestCols,
           (long)test_print_job.request->io_DestRows);

    test_print_job.active = TRUE;
    mp_set_test_print_enabled(win, FALSE);
    printf("Test Print started; MintPRINT remains responsive\n");
    SendIO((struct IORequest *)test_print_job.request);
    return TRUE;
}

static void apply_driver_config_to_gadgets(struct Window *win) {
    struct Gadget *g;

    if (!win) return;

    g = find_gadget_by_id(GAD_IP_STRING);
    if (g)
        GT_SetGadgetAttrs(g, win, NULL,
                          GTST_String, (ULONG)ip_buffer,
                          TAG_DONE);

    g = find_gadget_by_id(GAD_IPP_PATH);
    if (g)
        GT_SetGadgetAttrs(g, win, NULL,
                          GTST_String, (ULONG)driver_path_buffer,
                          TAG_DONE);

    g = find_gadget_by_id(GAD_DEBUG);
    if (g)
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Active, driver_debug ? 1 : 0,
                          TAG_DONE);

    g = find_gadget_by_id(GAD_ENGINE);
    if (g)
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Active, mp_engine_active_index(),
                          TAG_DONE);

    g = find_gadget_by_id(GAD_RESOLUTION);
    if (g)
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Active, (ULONG)mp_dpi_active_index(driver_resolution),
                          TAG_DONE);

    g = find_gadget_by_id(GAD_SIDES);
    if (g)
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Active, mp_sides_active_index(),
                          TAG_DONE);

    g = find_gadget_by_id(GAD_SPOOLER);
    if (g)
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Active, mp_spool_active_index(),
                          TAG_DONE);

    mp_update_spool_keep_gadget(win);

    mp_update_model_display(win);

    GT_RefreshWindow(win, NULL);
}

// Add after successful query to rebuild media dropdown (Updated to show media (tray))
void update_media_dropdown(struct Window *win) {
    int i;
    int count = num_media_tray_mappings;

    printf("Updating media dropdown, num_mappings=%d\n", num_media_tray_mappings);

    if (count <= 0) {
        count = 1;
        mp_media_label_ptrs[0] = mp_media_label_storage[0];
        strcpy(mp_media_label_storage[0], "No Media Available");
    } else {
        if (count > MAX_VALUES) count = MAX_VALUES;
        for (i = 0; i < count; i++) {
            const char *media = media_tray_map[i].media[0]
                              ? media_tray_map[i].media : "Unknown";
            const char *tray = media_tray_map[i].trayName[0]
                             ? media_tray_map[i].trayName : "Unknown";
            char media_pretty[MAX_ATTR_LEN];
            char tray_pretty[MAX_ATTR_LEN];

            mp_pretty_media_size(media, media_pretty, sizeof(media_pretty));
            mp_pretty_tray_name(tray, tray_pretty, sizeof(tray_pretty));

            mp_media_label_ptrs[i] = mp_media_label_storage[i];
            snprintf(mp_media_label_storage[i],
                     sizeof(mp_media_label_storage[i]),
                     "%s (%s)", media_pretty, tray_pretty);
            printf("Dropdown item %d: %s\n",
                   i, mp_media_label_storage[i]);
        }
    }

    mp_media_label_ptrs[count] = NULL;
    media_dropdown_items = mp_media_label_ptrs;

    if (media_dropdown && win) {
        GT_SetGadgetAttrs(media_dropdown, win, NULL,
                          GTCY_Labels, (ULONG)media_dropdown_items,
                          GTCY_Active, 0,
                          GA_Disabled, num_media_tray_mappings > 0 ? FALSE : TRUE,
                          TAG_DONE);
        RefreshGList(media_dropdown, win, NULL, 1);
        GT_RefreshWindow(win, NULL);
    }
}

void update_scaling_dropdown(struct Window *win) {
    struct Gadget *g;
    int i;
    int count = num_supported_scaling;

    if (count <= 0) {
        count = 1;
        mp_scaling_label_ptrs[0] = mp_scaling_label_storage[0];
        strncpy(mp_scaling_label_storage[0], initial_scaling_value,
                sizeof(mp_scaling_label_storage[0]) - 1);
        mp_scaling_label_storage[0][sizeof(mp_scaling_label_storage[0]) - 1] = '\0';
    } else {
        if (count > MAX_VALUES) count = MAX_VALUES;
        for (i = 0; i < count; i++) {
            mp_scaling_label_ptrs[i] = mp_scaling_label_storage[i];
            strncpy(mp_scaling_label_storage[i], supported_scaling[i],
                    sizeof(mp_scaling_label_storage[i]) - 1);
            mp_scaling_label_storage[i][sizeof(mp_scaling_label_storage[i]) - 1] = '\0';
        }
    }
    mp_scaling_label_ptrs[count] = NULL;
    scaling_mode_labels = mp_scaling_label_ptrs;

    /* Prefer "auto" when the printer offers it, rather than whichever
     * value the printer happened to list first. Confirmed on real
     * hardware: "auto-fit" got a job rejected outright as malformed on
     * one printer and mis-paginated a single page across multiple
     * physical sheets on another, while "auto" printed correctly both
     * times - "auto" is the safer default across printers generally,
     * not just a preference for this one. */
    {
        int preferred = 0;
        for (i = 0; i < count; i++) {
            if (strcmp(mp_scaling_label_storage[i], "auto") == 0) {
                preferred = i;
                break;
            }
        }

        g = find_gadget_by_id(GAD_SCALING_MODE);
        if (g && win) {
            GT_SetGadgetAttrs(g, win, NULL,
                              GTCY_Labels, (ULONG)scaling_mode_labels,
                              GTCY_Active, (ULONG)preferred,
                              GA_Disabled, num_supported_scaling > 0 ? FALSE : TRUE,
                              TAG_DONE);
            RefreshGList(g, win, NULL, 1);
            GT_RefreshWindow(win, NULL);
        }
    }
}

void update_print_mode_dropdown(struct Window *win) {
    struct Gadget *g;
    int i;
    int count = num_supported_print_modes;

    if (count <= 0) {
        count = 1;
        mp_print_mode_label_ptrs[0] = mp_print_mode_label_storage[0];
        strncpy(mp_print_mode_label_storage[0], initial_print_mode_value,
                sizeof(mp_print_mode_label_storage[0]) - 1);
        mp_print_mode_label_storage[0][sizeof(mp_print_mode_label_storage[0]) - 1] = '\0';
    } else {
        if (count > MAX_VALUES) count = MAX_VALUES;
        for (i = 0; i < count; i++) {
            mp_print_mode_label_ptrs[i] = mp_print_mode_label_storage[i];
            strncpy(mp_print_mode_label_storage[i], supported_print_modes[i],
                    sizeof(mp_print_mode_label_storage[i]) - 1);
            mp_print_mode_label_storage[i][sizeof(mp_print_mode_label_storage[i]) - 1] = '\0';
        }
    }
    mp_print_mode_label_ptrs[count] = NULL;
    print_mode_labels = mp_print_mode_label_ptrs;

    g = find_gadget_by_id(GAD_PRINT_MODE);
    if (g && win) {
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Labels, (ULONG)print_mode_labels,
                          GTCY_Active, 0,
                          GA_Disabled, num_supported_print_modes > 0 ? FALSE : TRUE,
                          TAG_DONE);
        RefreshGList(g, win, NULL, 1);
        GT_RefreshWindow(win, NULL);
    }
}

void update_dpi_dropdown(struct Window *win) {
    struct Gadget *g;
    int i;
    int count;
    BOOL has_compat = FALSE;

    mp_dpi_build_options(supported_dpi, num_supported_dpi,
                         strcmp(driver_engine_buffer, "pwg-raster") == 0,
                         driver_resolution,
                         driver_resolution_explicit ? 1 : 0,
                         &mp_dpi_options);
    count = mp_dpi_options.count;
    driver_resolution = mp_dpi_options.selected;

    for (i = 0; i < count; ++i) {
        mp_dpi_label_ptrs[i] = mp_dpi_label_storage[i];
        if (mp_dpi_options.compatibility[i]) {
            snprintf(mp_dpi_label_storage[i],
                     sizeof(mp_dpi_label_storage[i]),
                     "%d* dpi", mp_dpi_options.values[i]);
            has_compat = TRUE;
        } else {
            snprintf(mp_dpi_label_storage[i],
                     sizeof(mp_dpi_label_storage[i]),
                     "%d dpi", mp_dpi_options.values[i]);
        }
    }

    mp_dpi_label_ptrs[count] = NULL;
    resolution_labels = mp_dpi_label_ptrs;

    if (has_compat)
        printf("DPI: 300* dpi = compatibility (not printer-reported)\n");

    g = find_gadget_by_id(GAD_RESOLUTION);
    if (g && win) {
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Labels, (ULONG)resolution_labels,
                          GTCY_Active, (ULONG)mp_dpi_options.active,
                          GA_Disabled, num_supported_dpi > 0 ? FALSE : TRUE,
                          TAG_DONE);
        RefreshGList(g, win, NULL, 1);
        GT_RefreshWindow(win, NULL);
    }
}

void update_quality_dropdown(struct Window *win) {
    struct Gadget *g;
    int i;
    int count = num_supported_quality;

    if (count <= 0) {
        count = 1;
        mp_quality_label_ptrs[0] = mp_quality_label_storage[0];
        strncpy(mp_quality_label_storage[0], initial_quality_value,
                sizeof(mp_quality_label_storage[0]) - 1);
        mp_quality_label_storage[0][sizeof(mp_quality_label_storage[0]) - 1] = '\0';
    } else {
        if (count > MAX_VALUES) count = MAX_VALUES;
        for (i = 0; i < count; i++) {
            mp_quality_label_ptrs[i] = mp_quality_label_storage[i];
            strncpy(mp_quality_label_storage[i], supported_quality[i],
                    sizeof(mp_quality_label_storage[i]) - 1);
            mp_quality_label_storage[i][sizeof(mp_quality_label_storage[i]) - 1] = '\0';
        }
    }
    mp_quality_label_ptrs[count] = NULL;
    quality_mode_labels = mp_quality_label_ptrs;

    g = find_gadget_by_id(GAD_QUALITY_MODE);
    if (g && win) {
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Labels, (ULONG)quality_mode_labels,
                          GTCY_Active, 0,
                          GA_Disabled, num_supported_quality > 0 ? FALSE : TRUE,
                          TAG_DONE);
        RefreshGList(g, win, NULL, 1);
        GT_RefreshWindow(win, NULL);
    }
}

static void update_sides_dropdown(struct Window *win) {
    struct Gadget *g;
    BOOL transport_ok = mp_duplex_transport_supported();
    int count = 0;

    mp_sides_label_ptrs[count] = mp_sides_label_storage[count];
    strcpy(mp_sides_label_storage[count], "One-sided");
    strcpy(mp_sides_value_storage[count], "one-sided");
    ++count;

    if (transport_ok && mp_supported_side("two-sided-long-edge")) {
        mp_sides_label_ptrs[count] = mp_sides_label_storage[count];
        strcpy(mp_sides_label_storage[count], "Long edge");
        strcpy(mp_sides_value_storage[count], "two-sided-long-edge");
        ++count;
    }
    if (transport_ok && mp_supported_side("two-sided-short-edge")) {
        mp_sides_label_ptrs[count] = mp_sides_label_storage[count];
        strcpy(mp_sides_label_storage[count], "Short edge");
        strcpy(mp_sides_value_storage[count], "two-sided-short-edge");
        ++count;
    }

    mp_sides_label_ptrs[count] = NULL;
    mp_sides_option_count = count;

    if (count <= 1) {
        if (driver_sides_buffer[0] == 't')
            printf("Duplex is unavailable for this printer; using one-sided.\n");
        driver_sides_buffer[0] = '\0';
    } else if (mp_sides_active_index() == 0 &&
               strcmp(driver_sides_buffer, "one-sided") != 0) {
        strcpy(driver_sides_buffer, "one-sided");
    }

    g = find_gadget_by_id(GAD_SIDES);
    if (g && win) {
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Labels, (ULONG)mp_sides_label_ptrs,
                          GTCY_Active, mp_sides_active_index(),
                          GA_Disabled, count > 1 ? FALSE : TRUE,
                          TAG_DONE);
        RefreshGList(g, win, NULL, 1);
        GT_RefreshWindow(win, NULL);
    }

    if (!transport_ok &&
        (mp_supported_side("two-sided-long-edge") ||
         mp_supported_side("two-sided-short-edge"))) {
        printf("Duplex requires the PWG Raster or Apple Raster engine.\n");
    }
}

static BOOL mp_printer_advertises_format(const char *mime) {
    int i;

    if (!mime) return FALSE;
    for (i = 0; i < num_supported_formats; ++i) {
        if (strcasecmp(supported_formats[i], mime) == 0)
            return TRUE;
    }
    return FALSE;
}

/* Some printers, including a Samsung C480W seen in the field, advertise
 * image/jpeg and accept the IPP job while silently discarding its contents.
 * PWG 5100.13's JPEG size/dimension attributes are not mandatory proof of a
 * working decoder, so their absence is only a warning and JPEG stays
 * selectable.  jpeg_constraints_queried distinguishes a fresh negative
 * answer from an old cache created before MintPRINT requested these fields. */
static void mp_warn_if_jpeg_nominal(void) {
    if (!jpeg_constraints_queried ||
        !mp_printer_advertises_format("image/jpeg") ||
        jpeg_k_octets_reported || jpeg_x_dimension_reported ||
        jpeg_y_dimension_reported)
        return;

    printf("Warning: JPEG is advertised without JPEG limits.\n");
    if (mp_printer_advertises_format("application/postscript"))
        printf("JPEG may be unreliable; prefer PostScript if it fails.\n");
    else
        printf("JPEG may be unreliable and can silently discard jobs.\n");
}

/*
 * Rebuild the Engine Cycle from document-format-supported.
 *
 * With no Query/cache yet, all MintPRINT engines remain visible.
 * After a Query, only engines the printer actually advertised are shown.
 * If it advertised none of MintPRINT's formats, leave all four visible;
 * the existing unsupported-printer requester handles that exceptional case
 * and an empty GadTools Cycle would be undesirable.
 */
static void mp_rebuild_engine_options_from_query(void) {
    int i;
    int out = 0;
    BOOL use_query = num_supported_formats > 0;
    BOOL current_found = FALSE;

    for (i = 0; i < MP_ENGINE_MAX; ++i) {
        if (!use_query ||
            mp_printer_advertises_format(mp_engine_all_mimes[i])) {
            engine_labels[out] = (STRPTR)mp_engine_all_labels[i];
            mp_engine_value_map[out] = mp_engine_all_values[i];
            if (strcmp(driver_engine_buffer, mp_engine_all_values[i]) == 0)
                current_found = TRUE;
            ++out;
        }
    }

    if (out == 0) {
        for (i = 0; i < MP_ENGINE_MAX; ++i) {
            engine_labels[i] = (STRPTR)mp_engine_all_labels[i];
            mp_engine_value_map[i] = mp_engine_all_values[i];
        }
        out = MP_ENGINE_MAX;
        current_found = TRUE;
    }

    /* Prefer PWG Raster over JPEG/PDF whenever the printer actually
     * advertised it and the engine hasn't been explicitly pinned (see
     * driver_engine_explicit above): PWG Raster is a cheap PackBits-style
     * pack, while JPEG and PDF (which reuses the JPEG encoder - see
     * pdf_writer.c) both cost real per-pixel DCT/quantization work on top
     * of printer.device's DUMPRPORT scale-up. Without this, JPEG being
     * first in mp_engine_all_values kept it as the default for any printer
     * that also advertises JPEG (nearly all of them), even when PWG Raster
     * was available and objectively cheaper (issue #30). Only meaningful
     * with real capability data (use_query) - nothing to prefer yet from
     * an unqueried printer's "all three visible" list. */
    if (use_query && !driver_engine_explicit) {
        BOOL preferred = FALSE;
        for (i = 0; i < out; ++i) {
            if (strcmp(mp_engine_value_map[i], "pwg-raster") == 0) {
                if (strcmp(driver_engine_buffer, "pwg-raster") != 0) {
                    strcpy(driver_engine_buffer, "pwg-raster");
                }
                current_found = TRUE;
                preferred = TRUE;
                break;
            }
        }
        /* No PWG Raster on this printer (the common case for a
         * URF-only device like the OKI B412 - issue #60): Apple Raster
         * uses the same cheap PackBits-style row compression PWG Raster
         * does, so prefer it over JPEG/PDF's per-pixel DCT cost for the
         * same reason, whenever it's actually advertised. */
        if (!preferred) {
            for (i = 0; i < out; ++i) {
                if (strcmp(mp_engine_value_map[i], "urf") == 0) {
                    if (strcmp(driver_engine_buffer, "urf") != 0) {
                        strcpy(driver_engine_buffer, "urf");
                    }
                    current_found = TRUE;
                    break;
                }
            }
        }
    }

    engine_labels[out] = NULL;
    mp_engine_count = out;

    if (!current_found && mp_engine_count > 0) {
        strncpy(driver_engine_buffer, mp_engine_value_map[0],
                sizeof(driver_engine_buffer) - 1);
        driver_engine_buffer[sizeof(driver_engine_buffer) - 1] = '\0';
    }
}

/* Forward declaration: mp_offer_pwg_raster_switch() calls back into
 * update_engine_dropdown() (defined right after it) to refresh the Engine
 * gadget and dependent dropdowns once it applies a switch. */
static void update_engine_dropdown(struct Window *win, BOOL live_query);

/* Offers to switch away from an explicitly-pinned (see
 * driver_engine_explicit) non-PWG-Raster engine when PWG Raster is
 * available - the auto-preference in mp_rebuild_engine_options_from_query()
 * deliberately leaves a pinned choice alone, so this is the active nudge
 * for the case that logic can't fix silently: an old saved config, or a
 * stray gadget click, that predates PWG Raster being worth preferring.
 * Only called for a live Query (see call sites) and only once per printer
 * per session (driver_engine_pwg_offer_shown). */
static void mp_offer_pwg_raster_switch(struct Window *win) {
    struct EasyStruct es;
    int i;
    BOOL pwg_available = FALSE;

    if (!win || driver_engine_pwg_offer_shown) return;
    if (!driver_engine_explicit) return; /* already auto-preferred if unpinned */
    /* Only JPEG/PDF - not PostScript, which may have been deliberately
     * selected as a compatibility workaround (e.g. a printer that accepts
     * IPP JPEG but silently discards the job - see PostScript issue #15)
     * rather than left on by accident the way JPEG/PDF usually are. */
    if (strcmp(driver_engine_buffer, "jpeg") != 0 &&
        strcmp(driver_engine_buffer, "pdf") != 0) return;

    for (i = 0; i < mp_engine_count; ++i) {
        if (mp_engine_value_map[i] &&
            strcmp(mp_engine_value_map[i], "pwg-raster") == 0) {
            pwg_available = TRUE;
            break;
        }
    }
    if (!pwg_available) return;

    driver_engine_pwg_offer_shown = TRUE;

    es.es_StructSize = sizeof(struct EasyStruct);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)"MintPrint Settings";
    es.es_TextFormat = (UBYTE *)
        "This printer supports PWG Raster, which usually uses much less\n"
        "CPU than JPEG or PDF and is recommended for faster printing on\n"
        "classic Amigas. Switch this printer to PWG Raster now?";
    es.es_GadgetFormat = (UBYTE *)"Use PWG Raster|Keep current engine";
    if (!EasyRequest(win, &es, NULL)) return;

    printf("Switched to PWG Raster at the user's request (was %s).\n",
           mp_test_print_engine_name());
    strcpy(driver_engine_buffer, "pwg-raster");
    update_engine_dropdown(win, FALSE);
    update_sides_dropdown(win);
    update_dpi_dropdown(win);
}

static void update_engine_dropdown(struct Window *win, BOOL live_query) {
    struct Gadget *g;
    char previous[sizeof(driver_engine_buffer)];

    strncpy(previous, driver_engine_buffer, sizeof(previous) - 1);
    previous[sizeof(previous) - 1] = '\0';

    mp_rebuild_engine_options_from_query();

    if (!win) return;

    g = find_gadget_by_id(GAD_ENGINE);
    if (g) {
        GT_SetGadgetAttrs(g, win, NULL,
                          GTCY_Labels, (ULONG)engine_labels,
                          GTCY_Active, mp_engine_active_index(),
                          TAG_DONE);
        RefreshGList(g, win, NULL, 1);
        GT_RefreshWindow(win, NULL);
    }

    if (strcmp(previous, driver_engine_buffer) != 0) {
        if (strcmp(driver_engine_buffer, "pwg-raster") == 0)
            printf("Selected %s: cheaper than JPEG/PDF and this printer advertises it.\n",
                   engine_labels[mp_engine_active_index()]);
        else
            printf("Selected %s because the previous engine was not advertised by this printer.\n",
                   engine_labels[mp_engine_active_index()]);
    }

    if (live_query) mp_offer_pwg_raster_switch(win);
}

void cleanup_dropdown_labels() {
    /*
     * All CYCLE_KIND label arrays and strings are static process-lifetime
     * storage. FreeGadgets() has already detached GadTools from them, and
     * there is intentionally nothing to FreeVec here.
     */
}

/* MintPRINT prefs #8: capability cache.
 *
 * Unit0 contains selected defaults.
 * Unit0.cache contains printer-discovered capabilities.
 * ENV: is preferred for the current session; ENVARC: makes the cache survive
 * reboot. A cache is only used when HOST/PORT/PATH match the current Unit0.
 */
#define MP_CAP_CACHE_LINE_MAX 384
static char mp_cap_cache_line[MP_CAP_CACHE_LINE_MAX];

static void mp_cache_copy(char *dst, int dst_size, const char *src) {
    if (!dst || dst_size <= 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void mp_cache_clear_capabilities(void) {
    num_supported_formats = 0;
    num_supported_media = 0;
    num_supported_output_modes = 0;
    num_supported_sides = 0;
    num_supported_scaling = 0;
    num_supported_orientations = 0;
    num_supported_media_sources = 0;
    num_supported_print_modes = 0;
    num_supported_quality = 0;
    num_supported_dpi = 0;
    num_media_tray_mappings = 0;
    has_media_ready = FALSE;
    jpeg_constraints_queried = FALSE;
    jpeg_k_octets_reported = FALSE;
    jpeg_x_dimension_reported = FALSE;
    jpeg_y_dimension_reported = FALSE;
    supports_create_job = FALSE;
    supports_send_document = FALSE;
    supports_multiple_document_jobs = FALSE;
    supports_single_document_handling = FALSE;
    strcpy(pwg_sheet_back_value, "normal");
    /* Ink/toner status is live-only (not persisted to the capability cache
     * file - see mp_cache_write_file() - since levels are a snapshot, not
     * a capability), so switching units must clear it here rather than
     * leaving the previous printer's ink levels on screen until the next
     * Query. */
    num_marker_names = 0;
    num_marker_colors = 0;
    num_marker_types = 0;
    num_marker_levels = 0;
    num_marker_low_levels = 0;
    num_marker_high_levels = 0;
    printer_state_value = 0;
    num_printer_state_reasons = 0;
}

static BOOL mp_cache_write_file(CONST_STRPTR filename,
                                CONST_STRPTR host,
                                int port,
                                CONST_STRPTR path) {
    BPTR fh;
    char line[384];
    int i;

    fh = Open(filename, MODE_NEWFILE);
    if (!fh) return FALSE;

    FPuts(fh, "# MintPRINT printer capability cache\n");
    FPuts(fh, "CACHE_VERSION=1\n");

    snprintf(line, sizeof(line), "HOST=%s\n", host);
    FPuts(fh, line);
    snprintf(line, sizeof(line), "PORT=%d\n", port);
    FPuts(fh, line);
    snprintf(line, sizeof(line), "PATH=%s\n", path);
    FPuts(fh, line);

    for (i = 0; i < num_supported_formats; ++i) {
        snprintf(line, sizeof(line), "FORMAT=%s\n", supported_formats[i]);
        FPuts(fh, line);
    }

    if (jpeg_constraints_queried)
        FPuts(fh, "JPEG_CONSTRAINTS_QUERIED=1\n");
    if (jpeg_k_octets_reported)
        FPuts(fh, "JPEG_K_OCTETS_REPORTED=1\n");
    if (jpeg_x_dimension_reported)
        FPuts(fh, "JPEG_X_DIMENSION_REPORTED=1\n");
    if (jpeg_y_dimension_reported)
        FPuts(fh, "JPEG_Y_DIMENSION_REPORTED=1\n");

    for (i = 0; i < num_supported_media; ++i) {
        snprintf(line, sizeof(line), "MEDIA_SUPPORTED=%s\n", supported_media[i]);
        FPuts(fh, line);
    }

    for (i = 0; i < num_supported_media_sources; ++i) {
        snprintf(line, sizeof(line), "SOURCE=%s\n", supported_media_sources[i]);
        FPuts(fh, line);
    }

    for (i = 0; i < num_media_tray_mappings; ++i) {
        snprintf(line, sizeof(line), "MEDIA=%s|%s|%s|%s\n",
                 media_tray_map[i].media,
                 media_tray_map[i].source,
                 media_tray_map[i].trayName,
                 media_tray_map[i].medianame);
        FPuts(fh, line);
    }

    for (i = 0; i < num_supported_output_modes; ++i) {
        snprintf(line, sizeof(line), "OUTPUTMODE=%s\n", supported_output_modes[i]);
        FPuts(fh, line);
    }

    for (i = 0; i < num_supported_sides; ++i) {
        snprintf(line, sizeof(line), "SIDE=%s\n", supported_sides[i]);
        FPuts(fh, line);
    }

    if (supports_create_job) FPuts(fh, "CREATE_JOB=1\n");
    if (supports_send_document) FPuts(fh, "SEND_DOCUMENT=1\n");
    if (supports_multiple_document_jobs)
        FPuts(fh, "MULTIPLE_DOCUMENT_JOBS=1\n");
    if (supports_single_document_handling)
        FPuts(fh, "SINGLE_DOCUMENT_HANDLING=1\n");
    snprintf(line, sizeof(line), "PWG_SHEET_BACK=%s\n", pwg_sheet_back_value);
    FPuts(fh, line);

    for (i = 0; i < num_supported_scaling; ++i) {
        snprintf(line, sizeof(line), "SCALING=%s\n", supported_scaling[i]);
        FPuts(fh, line);
    }

    for (i = 0; i < num_supported_orientations; ++i) {
        snprintf(line, sizeof(line), "ORIENTATION=%d\n", supported_orientations[i]);
        FPuts(fh, line);
    }

    for (i = 0; i < num_supported_print_modes; ++i) {
        snprintf(line, sizeof(line), "PRINTMODE=%s\n", supported_print_modes[i]);
        FPuts(fh, line);
    }

    for (i = 0; i < num_supported_quality; ++i) {
        snprintf(line, sizeof(line), "QUALITY=%s\n", supported_quality[i]);
        FPuts(fh, line);
    }

    for (i = 0; i < num_supported_dpi; ++i) {
        snprintf(line, sizeof(line), "DPI=%d\n", supported_dpi[i]);
        FPuts(fh, line);
    }

    Close(fh);
    return TRUE;
}

static BOOL save_capability_cache(CONST_STRPTR host, int port, CONST_STRPTR path) {
    BOOL env_ok;
    BOOL envarc_ok;
    char env_cache[64];
    char envarc_cache[64];

    if (!host || !host[0] || port <= 0 || port > 65535 ||
        !path || path[0] != '/') {
        return FALSE;
    }

    if (!ensure_config_dir((CONST_STRPTR)"ENV:MintPRINT"))
        return FALSE;
    if (!ensure_config_dir((CONST_STRPTR)"ENVARC:MintPRINT"))
        return FALSE;

    unit_cache_path(current_unit_index, FALSE, env_cache, sizeof(env_cache));
    unit_cache_path(current_unit_index, TRUE, envarc_cache, sizeof(envarc_cache));

    env_ok = mp_cache_write_file((CONST_STRPTR)env_cache, host, port, path);
    envarc_ok = mp_cache_write_file((CONST_STRPTR)envarc_cache, host, port, path);

    return env_ok && envarc_ok;
}

static BOOL mp_cache_endpoint_matches(CONST_STRPTR filename,
                                      CONST_STRPTR expected_host,
                                      int expected_port,
                                      CONST_STRPTR expected_path) {
    BPTR fh;
    char host[64] = "";
    char path[96] = "";
    int port = -1;

    fh = Open(filename, MODE_OLDFILE);
    if (!fh) return FALSE;

    while (FGets(fh, (STRPTR)mp_cap_cache_line, sizeof(mp_cap_cache_line))) {
        trim_config_line(mp_cap_cache_line);

        if (strncmp(mp_cap_cache_line, "HOST=", 5) == 0) {
            mp_cache_copy(host, sizeof(host), mp_cap_cache_line + 5);
        } else if (strncmp(mp_cap_cache_line, "PORT=", 5) == 0) {
            port = atoi(mp_cap_cache_line + 5);
        } else if (strncmp(mp_cap_cache_line, "PATH=", 5) == 0) {
            mp_cache_copy(path, sizeof(path), mp_cap_cache_line + 5);
        }
    }

    Close(fh);

    return strcmp(host, expected_host) == 0 &&
           port == expected_port &&
           strcmp(path, expected_path) == 0;
}

static void mp_cache_parse_media(char *value) {
    char *p1;
    char *p2;
    char *p3;
    int i;

    if (num_media_tray_mappings >= MAX_VALUES) return;

    p1 = strchr(value, '|');
    if (!p1) return;
    *p1++ = '\0';

    p2 = strchr(p1, '|');
    if (!p2) return;
    *p2++ = '\0';

    p3 = strchr(p2, '|');
    if (!p3) return;
    *p3++ = '\0';

    i = num_media_tray_mappings;
    mp_cache_copy(media_tray_map[i].media,
                  sizeof(media_tray_map[i].media), value);
    mp_cache_copy(media_tray_map[i].source,
                  sizeof(media_tray_map[i].source), p1);
    mp_cache_copy(media_tray_map[i].trayName,
                  sizeof(media_tray_map[i].trayName), p2);
    mp_cache_copy(media_tray_map[i].medianame,
                  sizeof(media_tray_map[i].medianame), p3);
    num_media_tray_mappings++;
}

static BOOL mp_cache_load_file(CONST_STRPTR filename) {
    BPTR fh;

    fh = Open(filename, MODE_OLDFILE);
    if (!fh) return FALSE;

    mp_cache_clear_capabilities();

    while (FGets(fh, (STRPTR)mp_cap_cache_line, sizeof(mp_cap_cache_line))) {
        char *value;

        trim_config_line(mp_cap_cache_line);
        if (!mp_cap_cache_line[0] ||
            mp_cap_cache_line[0] == '#' ||
            mp_cap_cache_line[0] == ';') {
            continue;
        }

        if (strncmp(mp_cap_cache_line, "FORMAT=", 7) == 0) {
            store_value(supported_formats, &num_supported_formats,
                        mp_cap_cache_line + 7);
        } else if (strcmp(mp_cap_cache_line, "JPEG_CONSTRAINTS_QUERIED=1") == 0) {
            jpeg_constraints_queried = TRUE;
        } else if (strcmp(mp_cap_cache_line, "JPEG_K_OCTETS_REPORTED=1") == 0) {
            jpeg_k_octets_reported = TRUE;
        } else if (strcmp(mp_cap_cache_line, "JPEG_X_DIMENSION_REPORTED=1") == 0) {
            jpeg_x_dimension_reported = TRUE;
        } else if (strcmp(mp_cap_cache_line, "JPEG_Y_DIMENSION_REPORTED=1") == 0) {
            jpeg_y_dimension_reported = TRUE;
        } else if (strncmp(mp_cap_cache_line, "MEDIA_SUPPORTED=", 16) == 0) {
            store_value(supported_media, &num_supported_media,
                        mp_cap_cache_line + 16);
        } else if (strncmp(mp_cap_cache_line, "SOURCE=", 7) == 0) {
            store_value(supported_media_sources, &num_supported_media_sources,
                        mp_cap_cache_line + 7);
        } else if (strncmp(mp_cap_cache_line, "MEDIA=", 6) == 0) {
            value = mp_cap_cache_line + 6;
            mp_cache_parse_media(value);
        } else if (strncmp(mp_cap_cache_line, "OUTPUTMODE=", 11) == 0) {
            store_value(supported_output_modes, &num_supported_output_modes,
                        mp_cap_cache_line + 11);
        } else if (strncmp(mp_cap_cache_line, "SIDE=", 5) == 0) {
            store_value(supported_sides, &num_supported_sides,
                        mp_cap_cache_line + 5);
        } else if (strcmp(mp_cap_cache_line, "CREATE_JOB=1") == 0) {
            supports_create_job = TRUE;
        } else if (strcmp(mp_cap_cache_line, "SEND_DOCUMENT=1") == 0) {
            supports_send_document = TRUE;
        } else if (strcmp(mp_cap_cache_line,
                          "MULTIPLE_DOCUMENT_JOBS=1") == 0) {
            supports_multiple_document_jobs = TRUE;
        } else if (strcmp(mp_cap_cache_line,
                          "SINGLE_DOCUMENT_HANDLING=1") == 0) {
            supports_single_document_handling = TRUE;
        } else if (strncmp(mp_cap_cache_line, "PWG_SHEET_BACK=", 15) == 0) {
            const char *sheet_back = mp_cap_cache_line + 15;
            if (strcmp(sheet_back, "normal") == 0 ||
                strcmp(sheet_back, "rotated") == 0 ||
                strcmp(sheet_back, "flipped") == 0 ||
                strcmp(sheet_back, "manual-tumble") == 0) {
                mp_cache_copy(pwg_sheet_back_value,
                              sizeof(pwg_sheet_back_value), sheet_back);
            }
        } else if (strncmp(mp_cap_cache_line, "SCALING=", 8) == 0) {
            store_value(supported_scaling, &num_supported_scaling,
                        mp_cap_cache_line + 8);
        } else if (strncmp(mp_cap_cache_line, "ORIENTATION=", 12) == 0) {
            if (num_supported_orientations < MAX_VALUES)
                supported_orientations[num_supported_orientations++] =
                    atoi(mp_cap_cache_line + 12);
        } else if (strncmp(mp_cap_cache_line, "PRINTMODE=", 10) == 0) {
            store_value(supported_print_modes, &num_supported_print_modes,
                        mp_cap_cache_line + 10);
        } else if (strncmp(mp_cap_cache_line, "QUALITY=", 8) == 0) {
            if (num_supported_quality < MAX_QUALITIES) {
                mp_cache_copy(supported_quality[num_supported_quality],
                              sizeof(supported_quality[num_supported_quality]),
                              mp_cap_cache_line + 8);
                num_supported_quality++;
            }
        } else if (strncmp(mp_cap_cache_line, "DPI=", 4) == 0) {
            mp_add_supported_dpi(atoi(mp_cap_cache_line + 4));
        }
    }

    Close(fh);
    has_media_ready = num_media_tray_mappings > 0 ? TRUE : FALSE;
    return TRUE;
}

static BOOL load_capability_cache_for_current_endpoint(void) {
    char host[64];
    int port = -1;
    char env_cache[64];
    char envarc_cache[64];

    if (!parse_ip_and_port(ip_buffer, host, sizeof(host), &port))
        return FALSE;
    if (port <= 0) port = 80;

    unit_cache_path(current_unit_index, FALSE, env_cache, sizeof(env_cache));
    unit_cache_path(current_unit_index, TRUE, envarc_cache, sizeof(envarc_cache));

    if (mp_cache_endpoint_matches((CONST_STRPTR)env_cache, host, port, driver_path_buffer))
        return mp_cache_load_file((CONST_STRPTR)env_cache);

    if (mp_cache_endpoint_matches((CONST_STRPTR)envarc_cache, host, port, driver_path_buffer))
        return mp_cache_load_file((CONST_STRPTR)envarc_cache);

    return FALSE;
}

static void apply_cached_capabilities(struct Window *win) {
    if (!win) return;

    update_engine_dropdown(win, FALSE);
    update_media_dropdown(win);
    update_print_mode_dropdown(win);
    update_scaling_dropdown(win);
    update_quality_dropdown(win);
    update_dpi_dropdown(win);
    update_sides_dropdown(win);

    /* Put the user's saved Unit0 choices back on top of the available lists. */
    apply_job_defaults_to_gadgets(win);
    mp_warn_if_jpeg_nominal();
}

/* Reloads everything for current_unit_index: saved Unit%d config, its
 * cached capabilities (or "Not Detected" ghosting if there is none yet),
 * and the print-mode radio state. Used both when switching the Unit
 * dropdown and by File > Reload Driver Settings. */
/* Defined further down, alongside the rest of the ink-status panel's
 * drawing code - forward-declared here since reload_current_unit() (a
 * unit switch clears any previously-queried printer's ink levels via
 * mp_cache_clear_capabilities() above) needs to repaint the now-empty
 * panel before that code appears in the file. */
static void mp_draw_marker_strips(void);
static void mp_draw_sides_hint(void);
static void mp_draw_printer_icon(void);
static void mp_clear_printer_icon(void);
static BOOL mp_load_printer_icon_cache(BOOL require_uri_match);

static void reload_current_unit(struct Window *win) {
    mp_cache_clear_capabilities();
    mp_clear_printer_icon();
    /* Show the per-Unit processed artwork immediately. The normal startup
     * Query will validate its URI and fetch a replacement only if needed. */
    if (unit_file_exists(current_unit_index))
        mp_load_printer_icon_cache(FALSE);

    if (load_driver_config())
        custom_printf("MintPRINT Unit%d loaded\n", current_unit_index);
    else
        custom_printf("No Unit%d found; using MintPRINT defaults\n", current_unit_index);

    seed_saved_option_labels();
    load_print_mode();

    if (!win) return;

    apply_driver_config_to_gadgets(win);

    if (load_capability_cache_for_current_endpoint()) {
        apply_cached_capabilities(win);
        custom_printf("Loaded cached printer capabilities\n");
    } else {
        apply_saved_option_state(win);
    }

    apply_job_defaults_to_gadgets(win);

    {
        struct Gadget *print_mode_gadget = find_gadget_by_id(GAD_PRINT_MODE);
        if (print_mode_gadget) {
            GT_SetGadgetAttrs(print_mode_gadget, win, NULL,
                              GTCY_Active, print_mode,
                              TAG_DONE);
            RefreshGList(print_mode_gadget, win, NULL, 1);
        }
    }

    GT_RefreshWindow(win, NULL);
    mp_draw_marker_strips();
    mp_draw_sides_hint();
    mp_draw_printer_icon();
}



// Redirect printf to buffer
/* Draws the status box border and whatever lines output_buffer/output_line
 * currently hold. This is the box's ENTIRE on-screen paint, and it is only
 * ever a side effect of custom_printf() being called - the window is
 * WA_SimpleRefresh, so nothing repaints this non-gadget area automatically.
 * That includes IDCMP_REFRESHWINDOW: GT_BeginRefresh/GT_EndRefresh there
 * only repaints GadTools gadgets, never this hand-drawn area, so without
 * this being called from that handler too, anything that forces a refresh
 * (another window opening on top and closing again, dragging this window
 * partly offscreen, etc.) leaves the box LOOKING empty - output_buffer's
 * data is untouched throughout, only the paint was lost. */
static void redraw_output_box(void) {
    struct RastPort *rp;
    int line_height, output_area_top, output_area_bottom, start_line, i;

    if (!window) return;

    rp = window->RPort;
    if (font) SetFont(rp, font);
    SetAPen(rp, 1); // Text color
    SetBPen(rp, 0); // Background color
    SetDrMd(rp, JAM2);

    // Calculate the output area dimensions
    line_height = font->tf_YSize + 2;
    output_area_top = OUTPUT_TOP;
    output_area_bottom = output_area_top + (MAX_OUTPUT_LINES * line_height) - 1;

    // Draw the border
    SetAPen(rp, 1); // Border color
    RectFill(rp, OUTPUT_LEFT - 2, output_area_top - 2, OUTPUT_RIGHT + 2, output_area_top - 1); // Top
    RectFill(rp, OUTPUT_LEFT - 2, output_area_bottom + 1, OUTPUT_RIGHT + 2, output_area_bottom + 2); // Bottom
    RectFill(rp, OUTPUT_LEFT - 2, output_area_top - 2, OUTPUT_LEFT - 1, output_area_bottom + 2); // Left
    RectFill(rp, OUTPUT_RIGHT + 1, output_area_top - 2, OUTPUT_RIGHT + 2, output_area_bottom + 2); // Right

    // Clear the output area
    SetAPen(rp, 0); // Background color
    RectFill(rp, OUTPUT_LEFT, output_area_top, OUTPUT_RIGHT, output_area_bottom);

    // Draw the most recent lines (scrolling effect)
    start_line = (output_line > MAX_OUTPUT_LINES) ? (output_line - MAX_OUTPUT_LINES) : 0;
    for (i = 0; i < MAX_OUTPUT_LINES && (start_line + i) < output_line; i++) {
        int y = output_area_top + (i * line_height) + font->tf_Baseline;
        Move(rp, OUTPUT_LEFT, y);
        SetAPen(rp, 1); // Text color
        Text(rp, output_buffer[start_line + i], strlen(output_buffer[start_line + i]));
    }
}

void custom_printf(const char *format, ...) {
    // Special case: clear the output area if the format string is "CLEAR"
    if (strcmp(format, "CLEAR") == 0) {
        output_line = 0;
        redraw_output_box();
        return;
    }

    va_list args;
    va_start(args, format);

    // Dynamically allocate temp buffer
    char *temp = malloc(256);
    if (!temp) {
        va_end(args);
        return; // Fail silently if allocation fails
    }

    vsnprintf(temp, 256, format, args);
    va_end(args);

    /* In Debug mode also append to T:MintPRINT-gui.log, best-effort.
     * custom_printf() is
     * this program's ONLY status output (see the file comment above its
     * forward declaration) - when the on-screen box itself is the thing
     * that's broken, there is otherwise no way to see what actually
     * happened, unlike the driver's own T:MintPRINT-driver.log. */
    if (driver_debug) {
        BPTR log_fh = Open((CONST_STRPTR)"T:MintPRINT-gui.log", MODE_READWRITE);
        if (!log_fh) log_fh = Open((CONST_STRPTR)"T:MintPRINT-gui.log", MODE_NEWFILE);
        if (log_fh) {
            LONG len = (LONG)strlen(temp);
            Seek(log_fh, 0, OFFSET_END);
            if (len) Write(log_fh, (APTR)temp, len);
            Write(log_fh, (APTR)"\n", 1);
            Close(log_fh);
        }
    }

    // Strip trailing newline
    size_t len = strlen(temp);
    if (len > 0 && temp[len - 1] == '\n') {
        temp[len - 1] = '\0';
        len--;
    }

    // Shift buffer if full
    if (output_line >= MAX_OUTPUT_LINES) {
        for (int i = 0; i < MAX_OUTPUT_LINES - 1; i++) {
            strncpy(output_buffer[i], output_buffer[i + 1], MAX_OUTPUT_LINE_LENGTH);
        }
        output_line = MAX_OUTPUT_LINES - 1;
    }

    // Store new line
    strncpy(output_buffer[output_line], temp, MAX_OUTPUT_LINE_LENGTH - 1);
    output_buffer[output_line][MAX_OUTPUT_LINE_LENGTH - 1] = '\0';
    output_line++;

    // Free the temp buffer
    free(temp);

    redraw_output_box();
}

/* Ink/toner status strip panel - a plain RastPort drawing in the compact
 * spare area to the right of IPP Path/Engine/Debug and above Media, not
 * GadTools gadgets: GadTools has no gauge/progress
 * widget, and the fill colour needs to come from whatever RGB the printer
 * itself reports (marker-colors), which a fixed screen pen can't do -
 * hence ObtainBestPenA() per strip below rather than one of the four
 * pens (SetAPen 0-3) everything else in this window already uses. */
#define MP_MARKER_AREA_LEFT     320
#define MP_MARKER_AREA_TOP       57
#define MP_MARKER_AREA_BOTTOM   115
#define MP_MARKER_COLS            2
#define MP_MARKER_COL_GAP          6
#define MP_MARKER_ROW_H           14
#define MP_MARKER_BAR_H            7
#define MP_MARKER_MAX_STRIPS       6

/* marker-colors (RFC 3805 / PWG5100.13) is either "#RRGGBB" or one of a
 * small set of keyword names - resolved to RGB here, not at parse time in
 * query_printer_attributes(), since what to do with an unrecognised name
 * is a drawing concern (fall back to no fill), not a parsing one. */
/* One hex digit -> 0-15, or -1 if not a hex digit - used instead of
 * sscanf("%x") below, since the lightweight clib some Amiga toolchains
 * link against doesn't reliably support %x, and there's no cross-compiler
 * in this environment to find that out the hard way. */
static int mp_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static BOOL mp_marker_rgb(const char *color, UBYTE *r, UBYTE *g, UBYTE *b) {
    if (!color || !color[0]) return FALSE;

    if (color[0] == '#' && strlen(color) >= 7) {
        int hi, lo, i;
        UBYTE comp[3];
        for (i = 0; i < 3; i++) {
            hi = mp_hex_nibble(color[1 + i * 2]);
            lo = mp_hex_nibble(color[2 + i * 2]);
            if (hi < 0 || lo < 0) return FALSE;
            comp[i] = (UBYTE)((hi << 4) | lo);
        }
        *r = comp[0]; *g = comp[1]; *b = comp[2];
        return TRUE;
    }

    if (strcasecmp(color, "cyan") == 0)            { *r = 0;   *g = 255; *b = 255; return TRUE; }
    if (strcasecmp(color, "light-cyan") == 0)      { *r = 170; *g = 255; *b = 255; return TRUE; }
    if (strcasecmp(color, "magenta") == 0)         { *r = 255; *g = 0;   *b = 255; return TRUE; }
    if (strcasecmp(color, "light-magenta") == 0)   { *r = 255; *g = 170; *b = 255; return TRUE; }
    if (strcasecmp(color, "yellow") == 0)          { *r = 255; *g = 255; *b = 0;   return TRUE; }
    if (strcasecmp(color, "black") == 0)           { *r = 0;   *g = 0;   *b = 0;   return TRUE; }
    if (strcasecmp(color, "photo-black") == 0 ||
        strcasecmp(color, "matte-black") == 0)     { *r = 40;  *g = 40;  *b = 40;  return TRUE; }
    if (strcasecmp(color, "gray") == 0 ||
        strcasecmp(color, "grey") == 0)            { *r = 160; *g = 160; *b = 160; return TRUE; }
    if (strcasecmp(color, "red") == 0)             { *r = 255; *g = 0;   *b = 0;   return TRUE; }
    if (strcasecmp(color, "green") == 0)           { *r = 0;   *g = 200; *b = 0;   return TRUE; }
    if (strcasecmp(color, "blue") == 0)            { *r = 0;   *g = 0;   *b = 255; return TRUE; }
    if (strcasecmp(color, "white") == 0)           { *r = 255; *g = 255; *b = 255; return TRUE; }
    return FALSE; /* "multi-color", "unknown", "none", or anything else */
}

/* Bounded by marker-names/marker-colors, not marker-levels: a strip with
 * no level yet (index >= num_marker_levels) still draws as "unknown"
 * rather than being dropped, so a printer that's slow to report levels
 * doesn't make the whole panel disappear. */
static int mp_marker_count(void) {
    int n = num_marker_names;
    if (num_marker_colors < n) n = num_marker_colors;
    if (n > MP_MARKER_MAX_STRIPS) n = MP_MARKER_MAX_STRIPS;
    return n;
}

static void mp_marker_short_name(int index, char out[3]) {
    const char *name;
    const char *color;

    out[0] = '\0';

    if (index < 0 || index >= MAX_MARKERS)
        return;

    name = marker_names[index];
    color = marker_colors[index];

    /* Keep already-compact printer names such as C/M/Y/K. */
    if (name && name[0] && strlen(name) <= 2) {
        out[0] = name[0];
        out[1] = name[1] ? name[1] : '\0';
        out[2] = '\0';
        return;
    }

    /* Otherwise derive a predictable short label from marker colour. */
    if (color && color[0]) {
        if (strcasecmp(color, "cyan") == 0)          { strcpy(out, "C");  return; }
        if (strcasecmp(color, "magenta") == 0)       { strcpy(out, "M");  return; }
        if (strcasecmp(color, "yellow") == 0)        { strcpy(out, "Y");  return; }
        if (strcasecmp(color, "black") == 0)         { strcpy(out, "BK"); return; }
        if (strcasecmp(color, "light-cyan") == 0)    { strcpy(out, "LC"); return; }
        if (strcasecmp(color, "light-magenta") == 0) { strcpy(out, "LM"); return; }
        if (strcasecmp(color, "photo-black") == 0)   { strcpy(out, "PK"); return; }
        if (strcasecmp(color, "matte-black") == 0)   { strcpy(out, "MK"); return; }
    }

    /* Last resort: first two characters of the reported marker name. */
    if (name && name[0]) {
        out[0] = (char)toupper((unsigned char)name[0]);
        out[1] = name[1] ? (char)toupper((unsigned char)name[1]) : '\0';
        out[2] = '\0';
    }
}

/* Pens obtained for the ink/toner bars must stay allocated for as long as
 * their colour needs to remain visible, not just for the RectFill() call
 * that used them: a pen number is only ever an index into the screen's
 * shared colour-map, and RectFill() writes that index into the bitplanes
 * rather than an RGB value. Releasing a pen right after drawing lets
 * ANYTHING else - including the very next marker in the same redraw loop
 * - reclaim that register and repaint it, which instantly recolours every
 * pixel already drawn with that same index, not just newly drawn ones.
 * That was the "flashes the right colours, then they all end up on
 * whichever colour was requested last" bug from obtaining and releasing a
 * pen per marker. Pens are now held here across redraws and only released
 * right before the next redraw, or at shutdown via
 * mp_release_marker_pens(). */
static LONG mp_marker_pens[MP_MARKER_MAX_STRIPS] = {
    -1, -1, -1, -1, -1, -1
};

static void mp_release_marker_pens(void) {
    int i;
    if (!screen) return;
    for (i = 0; i < MP_MARKER_MAX_STRIPS; i++) {
        if (mp_marker_pens[i] >= 0) {
            ReleasePen(screen->ViewPort.ColorMap, (ULONG)mp_marker_pens[i]);
            mp_marker_pens[i] = -1;
        }
    }
}

/* Reduces printer-state/printer-state-reasons to one short word/phrase
 * shown to the right of the "Ink/Toner:" header, on the same line - see
 * mp_draw_marker_strips(). That row is only ~12 characters wide after
 * "Ink/Toner:" itself at the window's minimum size, so every label here
 * is deliberately kept at or under that (checked against the longest,
 * "Out of Paper", 12 chars) rather than spelled out in full - there is no
 * spare row to give this its own line without either overlapping the
 * printer icon just below (MP_PRINTER_ICON_TOP) or the Sides/DPI cycle
 * gadgets below that.
 *
 * Matching is substring-based against the handful of reasons someone
 * actually needs to act on, most severe first, rather than enumerating
 * the full PWG reason-keyword registry (RFC 8011 5.4.12) - real printers
 * commonly suffix these with "-error"/"-warning"/"-report" (e.g.
 * "media-jam-error"), which a substring match still catches. An
 * unrecognised reason falls back to the bare printer-state enum (RFC
 * 8011 5.4.11) rather than being shown raw and risking a much longer,
 * unbounded string on this same tight row. Blank before the first
 * Query. */
static void mp_printer_status_label(char *buf, size_t bufsize) {
    static const struct { const char *needle; const char *label; } known_reasons[] = {
        { "jam",                 "Jam"          },
        { "door-open",           "Door Open"    },
        { "cover-open",          "Door Open"    },
        { "interlock-open",      "Door Open"    },
        { "marker-supply-empty", "Toner Empty"  },
        { "toner-empty",         "Toner Empty"  },
        { "media-empty",         "Out of Paper" },
        { "marker-supply-low",   "Supply Low"   },
        { "toner-low",           "Supply Low"   },
    };
    size_t k;
    int i;

    if (!buf || !bufsize) return;
    buf[0] = '\0';

    for (k = 0; k < sizeof(known_reasons) / sizeof(known_reasons[0]); k++) {
        for (i = 0; i < num_printer_state_reasons; i++) {
            if (strstr(printer_state_reasons[i], known_reasons[k].needle)) {
                snprintf(buf, bufsize, "%s", known_reasons[k].label);
                return;
            }
        }
    }

    switch (printer_state_value) {
        case 3: snprintf(buf, bufsize, "Ready");    break;
        case 4: snprintf(buf, bufsize, "Busy");     break;
        case 5: snprintf(buf, bufsize, "Stopped");  break;
        default: break; /* not yet queried - leave blank */
    }
}

static void mp_draw_marker_strips(void) {
    struct RastPort *rp;
    int area_left, area_right, area_top, area_bottom;
    int grid_top, cell_width, count, i;
    char status_label[24];

    if (!window || !screen || !font) return;

    /* Release whatever the previous redraw held before drawing new
     * content - see the comment above mp_marker_pens[] for why these
     * can't just be released at the end of the loop below. */
    mp_release_marker_pens();

    rp = window->RPort;
    SetFont(rp, font);
    SetDrMd(rp, JAM2);

    area_left = MP_MARKER_AREA_LEFT;
    area_right = window->Width - 20;
    area_top = g_topborder + MP_MARKER_AREA_TOP;
    area_bottom = g_topborder + MP_MARKER_AREA_BOTTOM;
    if (area_right <= area_left || area_bottom <= area_top) return;

    /* Clear only the compact panel; this rectangle contains no gadgets. */
    SetAPen(rp, 0);
    RectFill(rp, area_left, area_top, area_right, area_bottom);

    SetAPen(rp, 1);
    Move(rp, area_left, area_top + font->tf_Baseline);
    Text(rp, "Ink/Toner:", 10);

    /* Printer status shares this header row, right-aligned - there is no
     * spare row below the grid to give it its own line without reaching
     * into the printer icon just below (MP_PRINTER_ICON_TOP) or the
     * Sides/DPI cycle gadgets below that. See mp_printer_status_label()
     * for why every possible label is kept short enough to fit here. */
    mp_printer_status_label(status_label, sizeof(status_label));
    if (status_label[0]) {
        int len = (int)strlen(status_label);
        SetAPen(rp, 1);
        Move(rp, area_right - len * 8, area_top + font->tf_Baseline);
        Text(rp, status_label, len);
    }

    count = mp_marker_count();
    if (count <= 0) {
        Move(rp,
             area_left,
             area_top + font->tf_YSize + 3 + font->tf_Baseline);
        Text(rp, "(Query for levels)", 18);
        return;
    }

    grid_top = area_top + font->tf_YSize + 3;
    cell_width =
        (area_right - area_left - MP_MARKER_COL_GAP) / MP_MARKER_COLS;

    for (i = 0; i < count; i++) {
        int col = i % MP_MARKER_COLS;
        int row = i / MP_MARKER_COLS;
        int cell_left = area_left +
            col * (cell_width + MP_MARKER_COL_GAP);
        int cell_right = cell_left + cell_width - 1;
        int row_top = grid_top + row * MP_MARKER_ROW_H;
        int text_y = row_top + font->tf_Baseline;
        int level = (i < num_marker_levels) ? marker_levels[i] : -1;
        int display_level;
        char namebuf[3];
        char pct[8];
        int pct_len, pct_x;
        int bar_left, bar_right, bar_top, bar_bottom;
        UBYTE r, g, b;
        BOOL have_rgb;
        LONG pen;

        if (row_top + MP_MARKER_ROW_H > area_bottom)
            break;

        mp_marker_short_name(i, namebuf);

        SetAPen(rp, 1);
        Move(rp, cell_left, text_y);
        Text(rp, namebuf, strlen(namebuf));

        if (level >= 0) {
            display_level = level;
            if (display_level > 100) display_level = 100;
            snprintf(pct, sizeof(pct), "%d%%", display_level);
        } else {
            strcpy(pct, "--");
        }

        pct_len = strlen(pct);
        pct_x = cell_right - (pct_len * 8);
        Move(rp, pct_x, text_y);
        Text(rp, pct, pct_len);

        /* Thin level bar between the short marker name and percentage. */
        bar_left = cell_left + 20;
        bar_right = pct_x - 4;
        bar_top = row_top + 1;
        bar_bottom = bar_top + MP_MARKER_BAR_H;
        if (bar_right <= bar_left)
            continue;

        SetAPen(rp, 1);
        RectFill(rp, bar_left, bar_top, bar_right, bar_top);
        RectFill(rp, bar_left, bar_bottom, bar_right, bar_bottom);
        RectFill(rp, bar_left, bar_top, bar_left, bar_bottom);
        RectFill(rp, bar_right, bar_top, bar_right, bar_bottom);

        have_rgb = mp_marker_rgb(marker_colors[i], &r, &g, &b);
        pen = -1;
        /* ObtainPen()/ObtainBestPenA()/ReleasePen() are graphics.library v39
         * (AmigaOS 3.0) additions - this file now opens graphics.library at
         * v37 (see main()) to try running on AmigaOS 2.04, which has no such
         * shared-pen-allocation API at all. Skip straight to "no marker
         * fill" there rather than calling an entry point that doesn't exist
         * in a v37 graphics.library - mp_marker_pens[i] staying -1 already
         * means mp_release_marker_pens() never calls ReleasePen() either. */
        if (have_rgb && GfxBase->LibNode.lib_Version >= 39) {
            /* This is a shared PUBLIC screen (LockPubScreen(NULL) below),
             * so on a constrained low-colour Workbench (e.g. 32 colours)
             * most pens are usually already in use by Workbench or other
             * apps. ObtainBestPenA() only ever reuses an EXISTING pen's
             * colour, and with no true cyan/magenta already on screen it
             * silently snapped several different requested marker colours
             * onto whichever single existing pen happened to be closest
             * (observed: cyan and magenta both landing on the same
             * yellow-ish pen). Try ObtainPen() first - it only succeeds
             * when it can set a genuinely free pen to this exact RGB - and
             * fall back to the old best-match behaviour only if the
             * screen really has no free pens left. Both allocators are
             * released the same way, via mp_release_marker_pens(). */
            pen = ObtainPen(screen->ViewPort.ColorMap, -1,
                             (ULONG)r << 24, (ULONG)g << 24,
                             (ULONG)b << 24, 0);
            if (pen < 0) {
                pen = (LONG)ObtainBestPenA(screen->ViewPort.ColorMap,
                                          (ULONG)r << 24, (ULONG)g << 24,
                                          (ULONG)b << 24, NULL);
            }
        }
        /* Stored, not released here - see the comment above
         * mp_marker_pens[] for why releasing per-marker recolours
         * already-drawn bars sharing a reclaimed pen index. */
        mp_marker_pens[i] = pen;

        if (level >= 0) {
            int inside_width;
            int fill_width;
            int fill_right;

            display_level = level;
            if (display_level > 100) display_level = 100;
            if (display_level < 0) display_level = 0;

            inside_width = bar_right - bar_left - 1;
            fill_width = (inside_width * display_level) / 100;
            fill_right = bar_left + fill_width;

            if (fill_width > 0) {
                SetAPen(rp, pen >= 0 ? (UBYTE)pen : 1);
                RectFill(rp,
                         bar_left + 1, bar_top + 1,
                         fill_right, bar_bottom - 1);
            }

            if (fill_right < bar_right - 1) {
                SetAPen(rp, 0);
                RectFill(rp,
                         fill_right + 1, bar_top + 1,
                         bar_right - 1, bar_bottom - 1);
            }
        }
    }
}

int load_ilbm_to_rgb(const char *filename, unsigned char **rgb_out, int *width_out, int *height_out) {
    struct jpeg_data data;
    memset(&data, 0, sizeof(data));
    printf("Attempting to load IFF: %s\n", filename);

    if (load_iff_direct(filename, &data) != 0) {
        printf("load_iff_direct failed\n");
        return -1;
    }

    int num_pixels = data.width * data.height;
    printf("Loaded: %d x %d padded = %d px\n", data.width, data.height, num_pixels);

    *rgb_out = AllocVec(num_pixels * 3, MEMF_ANY);
    if (!*rgb_out) {
        printf("AllocVec failed\n");
        free_jpeg_data(&data);
        return -1;
    }

    for (int i = 0; i < num_pixels; i++) {
        (*rgb_out)[i * 3 + 0] = data.red[i];
        (*rgb_out)[i * 3 + 1] = data.green[i];
        (*rgb_out)[i * 3 + 2] = data.blue[i];
    }

    *width_out = data.width;
    *height_out = data.height;
    free_jpeg_data(&data);
    return 0;
}

/* ---------------------------------------------------------------------
 * Duplex visual hint.
 *
 * The 32x32 ILBM files live next to the application in PROGDIR:Art/ and
 * are staged there by the release targets in the Makefile. Keeping them
 * external makes them easy to replace without rebuilding MintPrintSettings
 * and reuses the ILBM decoder already linked into this program.
 *
 * This is intentionally hand-drawn into spare space rather than another
 * GadTools gadget: it is purely an explanatory picture for the Sides cycle.
 * ------------------------------------------------------------------ */
#define MP_SIDES_HINT_LEFT       460
#define MP_SIDES_HINT_TOP        126
#define MP_SIDES_HINT_SIZE        32
#define MP_SIDES_HINT_MAX_PENS    32
#define MP_SIDES_HINT_COUNT        3

struct MPSidesHintImage {
    unsigned char *rgb;
    int width;
    int height;
    BOOL attempted;
    /* Cached nearest-screen-pen index per pixel (width*height entries) -
     * see mp_sides_hint_ensure_pens(). Rebuilt only when the screen
     * palette actually changes, not on every redraw. */
    UBYTE *pens;
    BOOL pens_valid;
    ULONG cached_palette[3 * 256];
    int cached_pen_count;
};

struct MPSidesHintPen {
    UBYTE r, g, b;
    LONG pen;
};

static struct MPSidesHintImage mp_sides_hint_images[MP_SIDES_HINT_COUNT];
static const char *mp_sides_hint_paths[MP_SIDES_HINT_COUNT] = {
    "PROGDIR:Art/single.iff",
    "PROGDIR:Art/longside.iff",
    "PROGDIR:Art/shortside.iff"
};

static int mp_sides_hint_index(void) {
    if (strcmp(driver_sides_buffer, "two-sided-long-edge") == 0)
        return 1;
    if (strcmp(driver_sides_buffer, "two-sided-short-edge") == 0)
        return 2;
    return 0;
}

static BOOL mp_load_sides_hint_image(int index) {
    struct MPSidesHintImage *image;
    struct jpeg_data data;
    int pixels;
    int i;

    if (index < 0 || index >= MP_SIDES_HINT_COUNT)
        return FALSE;

    image = &mp_sides_hint_images[index];
    if (image->attempted)
        return image->rgb != NULL;

    image->attempted = TRUE;
    memset(&data, 0, sizeof(data));

    /* Avoid load_ilbm_to_rgb() here because it logs to the GUI status box. */
    if (load_iff_direct(mp_sides_hint_paths[index], &data) != 0)
        return FALSE;

    if (data.width <= 0 || data.height <= 0 ||
        data.width > 64 || data.height > 64) {
        free_jpeg_data(&data);
        return FALSE;
    }

    pixels = data.width * data.height;
    image->rgb = AllocVec((ULONG)pixels * 3UL, MEMF_ANY);
    if (!image->rgb) {
        free_jpeg_data(&data);
        return FALSE;
    }

    for (i = 0; i < pixels; ++i) {
        image->rgb[i * 3 + 0] = data.red[i];
        image->rgb[i * 3 + 1] = data.green[i];
        image->rgb[i * 3 + 2] = data.blue[i];
    }
    image->width = data.width;
    image->height = data.height;
    free_jpeg_data(&data);
    return TRUE;
}

static UBYTE mp_sides_hint_nearest_pen(const ULONG *palette,
                                         int pen_count,
                                         UBYTE r, UBYTE g, UBYTE b) {
    int i;
    int best = 0;
    ULONG best_distance = 0xffffffffUL;

    for (i = 0; i < pen_count; ++i) {
        LONG pr = (LONG)(palette[i * 3 + 0] >> 24);
        LONG pg = (LONG)(palette[i * 3 + 1] >> 24);
        LONG pb = (LONG)(palette[i * 3 + 2] >> 24);
        LONG dr = (LONG)r - pr;
        LONG dg = (LONG)g - pg;
        LONG db = (LONG)b - pb;
        ULONG distance = (ULONG)(dr * dr + dg * dg + db * db);

        if (distance < best_distance) {
            best = i;
            best_distance = distance;
            if (distance == 0)
                break;
        }
    }

    return (UBYTE)best;
}

/* mp_sides_hint_nearest_pen() is a linear scan over the whole screen
 * palette (up to 256 entries) - fine once, but mp_draw_sides_hint() used
 * to call it for every pixel of the 32x32 hint image on every single
 * redraw (Query, every gadget click, every window refresh, ...), up to
 * ~1024*256 distance computations each time for no reason: the palette
 * essentially never changes between redraws. Cache the per-pixel result
 * here and only redo the scan when the palette actually differs from the
 * one the cache was built against (a real screen depth/mode change,
 * which is rare), rather than unconditionally every redraw. */
static BOOL mp_sides_hint_ensure_pens(struct MPSidesHintImage *image,
                                      const ULONG *palette, int pen_count) {
    int pixels = image->width * image->height;
    int i;

    if (image->pens_valid && image->cached_pen_count == pen_count &&
        memcmp(image->cached_palette, palette,
               (size_t)pen_count * 3 * sizeof(ULONG)) == 0) {
        return TRUE;
    }

    if (!image->pens) {
        image->pens = AllocVec((ULONG)pixels, MEMF_ANY);
        if (!image->pens) return FALSE;
    }

    for (i = 0; i < pixels; ++i) {
        image->pens[i] = mp_sides_hint_nearest_pen(
            palette, pen_count,
            image->rgb[i * 3 + 0], image->rgb[i * 3 + 1],
            image->rgb[i * 3 + 2]);
    }

    memcpy(image->cached_palette, palette,
           (size_t)pen_count * 3 * sizeof(ULONG));
    image->cached_pen_count = pen_count;
    image->pens_valid = TRUE;
    return TRUE;
}

/* GetRGB32() is a graphics.library v39 (AmigaOS 3.0/AGA) addition and does
 * not exist in a v37 (AmigaOS 2.04) graphics.library - calling it there
 * jumps through a library vector that was never filled in, which is
 * exactly the illegal-instruction crash a real AmigaOS 2.0/2.04 test of
 * this build hit, right after mp_draw_marker_strips() (no v39-only calls
 * on its own code path) and before mp_draw_sides_hint() had a chance to
 * log a trace line. GetRGB4() - 4 bits per component, 0x0RGB packed into
 * one ULONG - has existed since Kickstart 1.x, so build the same
 * top-byte-significant 32-bit-per-component table GetRGB32() would have,
 * one pen at a time, expanding each nibble to a full byte
 * (0x0..0xF -> 0x00..0xFF) exactly the way driver/classic_render_shim.c
 * already does for the OS3.1 driver's own 4-bit-gun input. Shared by
 * mp_draw_sides_hint() and mp_draw_printer_icon() below, which both used
 * to call GetRGB32() directly. */
static void mp_fill_screen_palette32(struct ColorMap *cm, int pen_count,
                                     ULONG *out_palette) {
    if (GfxBase->LibNode.lib_Version >= 39) {
        GetRGB32(cm, 0, (ULONG)pen_count, out_palette);
        return;
    }

    int pen;
    for (pen = 0; pen < pen_count; ++pen) {
        ULONG rgb4 = GetRGB4(cm, pen);
        UBYTE r4 = (UBYTE)((rgb4 >> 8) & 0xF);
        UBYTE g4 = (UBYTE)((rgb4 >> 4) & 0xF);
        UBYTE b4 = (UBYTE)(rgb4 & 0xF);
        out_palette[pen * 3 + 0] = (ULONG)((r4 << 4) | r4) << 24;
        out_palette[pen * 3 + 1] = (ULONG)((g4 << 4) | g4) << 24;
        out_palette[pen * 3 + 2] = (ULONG)((b4 << 4) | b4) << 24;
    }
}

/* cm->Count only names a real palette size on a "V38 compatible" ColorMap
 * (cm->Type != 0 - graphics/view.h's own comment on that field: Type == 0
 * means the older, simpler V1.2/V1.3-compatible ColorMap layout). A Type
 * == 0 ColorMap - still seen from some OS2.0/2.04-era graphics.library
 * builds - predates the Count field's meaning, so trusting it there feeds
 * mp_fill_screen_palette32() and every nearest-pen search a pen_count
 * that does not match the screen's real, much smaller pen table -
 * anything at or past the true end of that table is whatever memory
 * happens to sit there, and a search that picks one of those phantom
 * entries returns a pen index the hardware cannot actually display (a
 * 4-bitplane screen only ever has 16 real pens; SetAPen() with a larger
 * index does not fail, it just displays *some* real pen picked by
 * whichever low bits survive). Falling back to the bitmap's own plane
 * count sidesteps this entirely - BitMap->Depth has been a stable,
 * always-valid field since Kickstart 1.0, unlike Count's meaning here. */
static int mp_screen_pen_count(struct Screen *scr, struct ColorMap *cm) {
    int count = (cm->Type != 0) ? (int)cm->Count : 0;

    if (count <= 0) {
        count = (scr && scr->RastPort.BitMap)
                    ? (1 << scr->RastPort.BitMap->Depth) : 16;
    }
    if (count > 256) count = 256;
    if (count < 1) count = 1;
    return count;
}

/* A pixel counts as "near-neutral" (grey-ish rather than a real hue) when
 * its RGB channels are all within this many levels (0-255 scale) of each
 * other - loose enough for a slightly warm/cool grey, tight enough to
 * exclude a genuinely saturated colour. Used below to decide, per pixel,
 * whether to prefer mp_low_colour_gray_pen()'s neutral-biased pen search
 * over the plain nearest-colour one. */
#define MP_NEUTRAL_SAT_THRESHOLD 32

/* A nearest-colour lookup is useful on a 32+ colour screen, but it produces
 * harsh, misleading colour blocks on a 16-colour Workbench palette. Use the
 * closest-luminance, least-saturated existing pen there instead. This keeps
 * printer artwork effectively grayscale without changing Workbench's palette
 * or relying on colour pens such as blue and yellow. */
static UBYTE mp_low_colour_gray_pen(const ULONG *palette, int pen_count,
                                    UBYTE r, UBYTE g, UBYTE b) {
    int i;
    ULONG luminance = (299UL * r + 587UL * g + 114UL * b) / 1000UL;
    ULONG best_score = 0xffffffffUL;
    UBYTE best = 0;

    for (i = 0; i < pen_count; ++i) {
        LONG pr = (LONG)((palette[i * 3 + 0] >> 24) & 0xffUL);
        LONG pg = (LONG)((palette[i * 3 + 1] >> 24) & 0xffUL);
        LONG pb = (LONG)((palette[i * 3 + 2] >> 24) & 0xffUL);
        LONG minimum = pr < pg ? pr : pg;
        LONG maximum = pr > pg ? pr : pg;
        ULONG palette_luminance;
        LONG level_delta;
        ULONG score;

        if (pb < minimum)
            minimum = pb;
        if (pb > maximum)
            maximum = pb;
        palette_luminance = (299UL * (ULONG)pr +
                             587UL * (ULONG)pg +
                             114UL * (ULONG)pb) / 1000UL;
        level_delta = (LONG)palette_luminance - (LONG)luminance;
        if (level_delta < 0)
            level_delta = -level_delta;
        /* Chroma is weighted heavily so neutral pens win over similarly
         * bright saturated pens in the old 16-colour Workbench palette. */
        score = (ULONG)level_delta + (ULONG)(maximum - minimum) * 8UL;
        if (score < best_score) {
            best_score = score;
            best = (UBYTE)i;
        }
    }
    return best;
}

static void mp_draw_sides_hint(void) {
    struct MPSidesHintImage *image;
    ULONG screen_palette[3 * 256];
    struct ColorMap *cm;
    struct RastPort *rp;
    int index;
    int screen_pen_count;
    int draw_w, draw_h;
    int x, y;
    int left = MP_SIDES_HINT_LEFT;
    int top = g_topborder + MP_SIDES_HINT_TOP;

    if (!window || !screen)
        return;

    rp = window->RPort;
    cm = screen->ViewPort.ColorMap;
    if (!cm)
        return;

    screen_pen_count = mp_screen_pen_count(screen, cm);
    mp_fill_screen_palette32(cm, screen_pen_count, screen_palette);
    SetDrMd(rp, JAM1);
    SetAPen(rp, 0);
    RectFill(rp, left - 1, top - 1,
             left + MP_SIDES_HINT_SIZE, top + MP_SIDES_HINT_SIZE);
    SetAPen(rp, 1);
    RectFill(rp, left - 1, top - 1,
             left + MP_SIDES_HINT_SIZE, top - 1);
    RectFill(rp, left - 1, top - 1,
             left - 1, top + MP_SIDES_HINT_SIZE);
    RectFill(rp, left - 1, top + MP_SIDES_HINT_SIZE,
             left + MP_SIDES_HINT_SIZE, top + MP_SIDES_HINT_SIZE);
    RectFill(rp, left + MP_SIDES_HINT_SIZE, top - 1,
             left + MP_SIDES_HINT_SIZE, top + MP_SIDES_HINT_SIZE);

    index = mp_sides_hint_index();
    if (!mp_load_sides_hint_image(index))
        return;

    image = &mp_sides_hint_images[index];
    if (!mp_sides_hint_ensure_pens(image, screen_palette, screen_pen_count))
        return;

    draw_w = image->width < MP_SIDES_HINT_SIZE
           ? image->width : MP_SIDES_HINT_SIZE;
    draw_h = image->height < MP_SIDES_HINT_SIZE
           ? image->height : MP_SIDES_HINT_SIZE;

    for (y = 0; y < draw_h; ++y) {
        LONG last_pen = -1;
        for (x = 0; x < draw_w; ++x) {
            int pixel = y * image->width + x;
            UBYTE sr = image->rgb[pixel * 3 + 0];
            UBYTE sg = image->rgb[pixel * 3 + 1];
            UBYTE sb = image->rgb[pixel * 3 + 2];
            UBYTE smax = (sr > sg) ? sr : sg;
            UBYTE smin = (sr < sg) ? sr : sg;
            UBYTE pen = image->pens[pixel];

            if (sb > smax) smax = sb;
            if (sb < smin) smin = sb;

            /* Per source pixel, not per screen: a screen's own advertised
             * colour count/depth turned out not to be a reliable signal
             * for whether it actually offers a smooth grey ramp (see
             * mp_screen_pen_count()'s comment on why Count itself can be
             * unreliable) - so this only asks whether THIS pixel looks
             * like it was meant to be grey, and if so, prefers a screen
             * pen that is too, on any screen. A source pixel that is
             * already a real colour still gets the plain nearest-colour
             * match above unchanged. */
            if ((int)smax - (int)smin <= MP_NEUTRAL_SAT_THRESHOLD)
                pen = mp_low_colour_gray_pen(screen_palette, screen_pen_count,
                                             sr, sg, sb);

            if ((LONG)pen != last_pen) {
                SetAPen(rp, pen);
                last_pen = (LONG)pen;
            }
            WritePixel(rp, left + x, top + y);
        }
    }

}

static void mp_free_sides_hint_images(void) {
    int i;
    for (i = 0; i < MP_SIDES_HINT_COUNT; ++i) {
        if (mp_sides_hint_images[i].rgb) {
            FreeVec(mp_sides_hint_images[i].rgb);
            mp_sides_hint_images[i].rgb = NULL;
        }
        if (mp_sides_hint_images[i].pens) {
            FreeVec(mp_sides_hint_images[i].pens);
            mp_sides_hint_images[i].pens = NULL;
        }
        mp_sides_hint_images[i].width = 0;
        mp_sides_hint_images[i].height = 0;
        mp_sides_hint_images[i].attempted = FALSE;
        mp_sides_hint_images[i].pens_valid = FALSE;
        mp_sides_hint_images[i].cached_pen_count = 0;
    }
}

// Creates a valid PWG header and uncompressed RGB data
int rgb_to_pwg_memory(unsigned char *rgb_data, int width, int height, unsigned char **pwg_out, int *pwg_size_out) {
    int row_bytes = width * 3;
    int header_size = 1796; // Standard PWG header size
    int data_size = row_bytes * height;
    int total_size = header_size + data_size;

    unsigned char *buffer = malloc(total_size);
    if (!buffer) return -1;
    memset(buffer, 0, total_size);

    // PWG header - 1796 bytes total
    // See Apple Raster Format spec for details
    buffer[0] = 'R'; buffer[1] = 'a'; buffer[2] = 'S'; buffer[3] = '2'; // PWG magic
    buffer[4] = 0x00; buffer[5] = 0x00; buffer[6] = 0x00; buffer[7] = 0x02; // Version 2
    buffer[8] = 0x00; buffer[9] = 0x00; buffer[10] = 0x00; buffer[11] = 0x01; // Number of pages = 1

    // Page 1 - raster attributes
    *(int *)&buffer[20] = width;       // pixelsPerLine
    *(int *)&buffer[24] = height;      // linesPerPage
    *(int *)&buffer[28] = 8;           // bitsPerColor
    *(int *)&buffer[32] = 24;          // bitsPerPixel
    *(int *)&buffer[36] = 1;           // color order (chunky)
    *(int *)&buffer[40] = 1;           // color space = sRGB
    *(int *)&buffer[44] = 0;           // compression = none

    // Set resolution (300 dpi)
    *(int *)&buffer[60] = 300;         // crossFeedTransform (dpi)
    *(int *)&buffer[64] = 300;         // feedTransform (dpi)

    // Copy raw RGB data after header
    memcpy(buffer + header_size, rgb_data, data_size);

    *pwg_out = buffer;
    *pwg_size_out = total_size;
    return 0;
}

// Existing functions (unchanged)
int rgb_to_pwg(const char *filename, unsigned char *rgb_data, int width, int height) {
    FILE *file = fopen(filename, "wb");
    if (!file) {
        printf("Failed to open PWG file: %s\n", filename);
        return -1;
    }

    char header[128] = {0};
    memcpy(header, "RaS2", 4);
    header[4] = 0x00;
    header[8] = 0x00;
    header[12] = 0x00;
    header[16] = 0x00;
    header[20] = 0x00;
    header[24] = (width >> 24) & 0xFF;
    header[25] = (width >> 16) & 0xFF;
    header[26] = (width >> 8) & 0xFF;
    header[27] = width & 0xFF;
    header[28] = (height >> 24) & 0xFF;
    header[29] = (height >> 16) & 0xFF;
    header[30] = (height >> 8) & 0xFF;
    header[31] = height & 0xFF;
    header[32] = 8;
    header[36] = 3;
    header[40] = 3;
    header[44] = (width * 3 >> 24) & 0xFF;
    header[45] = (width * 3 >> 16) & 0xFF;
    header[46] = (width * 3 >> 8) & 0xFF;
    header[47] = (width * 3) & 0xFF;
    fwrite(header, 1, 128, file);

    fwrite(rgb_data, 1, width * height * 3, file);

    fclose(file);
    return 0;
}
// Wrapper to convert RGB to PWG
int convert_to_pwg(unsigned char *rgb, int w, int h, unsigned char **pwg_out, int *pwg_size_out) {
    char pwgfile[256];
    snprintf(pwgfile, sizeof(pwgfile), "UHD:temp.pwg");

    if (rgb_to_pwg(pwgfile, rgb, w, h) != 0) {
        return -1;
    }

    FILE *fp = fopen(pwgfile, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);
    rewind(fp);

    *pwg_out = malloc(size);
    if (!*pwg_out) {
        fclose(fp);
        return -1;
    }

    fread(*pwg_out, 1, size, fp);
    fclose(fp);
    *pwg_size_out = size;
    return 0;
}
/* ---------------------------------------------------------------------
 * DEVS:Printers/MintPRINT install helper
 *
 * MintPrint Settings ships as a single drawer holding both bundled driver
 * builds under PROGDIR:Drivers/ (MintPRINT-V44 and MintPRINT-OS31 - see
 * docs/OS31_SUPPORT.md for why two builds exist at all) and automatically
 * picks the one matching this machine's printer.device generation. If the
 * chosen driver is not yet installed in DEVS:Printers/, offer to copy it
 * in and point the user at Printer Prefs.
 * ------------------------------------------------------------------- */

/* The extended V44 printer-driver interface (PPCF_EXTENDED / PRTA_NoIO /
 * PRTA_8BitGuns) shipped with AmigaOS 3.5, whose components are v44.
 * Every component released together with a given AmigaOS version shares
 * that version's major library number - except exec.library itself on a
 * software-only OS update layered on an existing Kickstart ROM. AmigaOS
 * 3.9 is exactly this: real-world 3.9 systems keep whatever exec.library
 * version their Kickstart ROM shipped with (3.1's exec v40 is common)
 * while workbench.library and the rest of LIBS: - including the parts
 * printer.device actually depends on here - get bumped to V44+ (v45,
 * higher again after Boing Bags). Checking exec.library's version alone
 * misdetected such a 3.9 system as needing the classic pre-V44 driver.
 * workbench.library, being a plain LIBS: file every such update actually
 * replaces, tracks "what AmigaOS is genuinely running" far more reliably
 * than the Kickstart-resident exec.library does. */
#define MP_EXEC_VERSION_V44 44

/* Falls back to exec.library's version only if workbench.library can't be
 * opened at all - should never happen on a real system, but that is a
 * safer failure mode than mis-defaulting to the classic driver on an
 * unknown one. */
static UWORD mp_os_version(void) {
    struct Library *wb_base = OpenLibrary((CONST_STRPTR)"workbench.library", 0);
    UWORD ver;

    if (!wb_base) return SysBase->LibNode.lib_Version;

    ver = wb_base->lib_Version;
    CloseLibrary(wb_base);
    return ver;
}

static BOOL mp_needs_os31_driver(void) {
    return mp_os_version() < MP_EXEC_VERSION_V44;
}

/* Short friendly label for EasyRequest prompts and the About box - kept
 * deliberately brief (no version number) because EasyRequest sizes its
 * window to the single longest \n-delimited line with no wrapping, and a
 * long line here easily pushes the whole requester off a low-resolution
 * AmigaOS screen. workbench.library's version, not exec.library's, is
 * what's checked (see mp_os_version() above) - the numbers below are the
 * ones real AmigaOS 3.x releases shipped workbench.library with on an
 * unpatched system; a 3.9 install with Boing Bags applied commonly reads
 * higher than the plain "45" below and falls through to the generic
 * fallback label instead, which is only cosmetic - mp_needs_os31_driver()'s
 * own >=44 comparison does not depend on hitting one of these exact
 * numbers. Any version outside this list (older, a future release, or a
 * patched 3.9) still gets a short label rather than being left blank -
 * see printf() callers for the version number itself, logged separately
 * to the on-screen output box where a longer line just wraps instead of
 * resizing a window. */
static void mp_describe_amiga_os(char *buf, size_t bufsize) {
    UWORD ver = mp_os_version();
    const char *label;

    switch (ver) {
        case 36: label = "2.0/2.03"; break;
        case 37: label = "2.04/2.1"; break;
        case 39: label = "3.0";  break;
        case 40: label = "3.1";  break;
        case 44: label = "3.5";  break;
        case 45: label = "3.9";  break;
        case 47: label = "3.2+"; break;
        default: label = NULL;   break;
    }

    if (label) {
        snprintf(buf, bufsize, "AmigaOS %s", label);
    } else {
        snprintf(buf, bufsize, "AmigaOS (v%u)", (unsigned)ver);
    }
}

/* Bundled driver source path, chosen from the two drawers under
 * PROGDIR:Drivers/ - see mp_needs_os31_driver() above. */
static CONST_STRPTR mp_driver_src_path(void) {
    return mp_needs_os31_driver()
        ? (CONST_STRPTR)"PROGDIR:Drivers/MintPRINT-OS31/MintPRINT"
        : (CONST_STRPTR)"PROGDIR:Drivers/MintPRINT-V44/MintPRINT";
}

static BOOL mp_file_exists(CONST_STRPTR name) {
    BPTR lock = Lock(name, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return TRUE;
    }
    return FALSE;
}

/* Copies to a "<dst>.new" temp file first, validates the full source size
 * landed, and only then swaps it in over the real destination. dst - e.g.
 * DEVS:Printers/MintPRINT, the live driver a printer.device caller may be
 * using right now - used to be Open(dst, MODE_NEWFILE) directly, which
 * truncates it immediately: an AllocVec failure, a short write, or the disk
 * filling up partway through left a previously-working file destroyed with
 * no way back. Now any such failure only ever touches the temp file.
 *
 * The swap itself is a rename-old-aside/rename-new-in/delete-old dance
 * rather than delete-then-rename: if Rename(tmp, dst) is the thing that
 * fails (disk full on the directory entry, whatever), a plain
 * DeleteFile(dst) beforehand would have already destroyed the working
 * file with nothing to put back. Keeping the old file as "<dst>.bak"
 * until the new one is confirmed in place means that failure instead
 * restores dst from the backup. */
static BOOL mp_copy_file(CONST_STRPTR src, CONST_STRPTR dst) {
    BPTR in, out;
    UBYTE *buf;
    LONG nread;
    BOOL ok = TRUE;
    LONG src_size;
    LONG written = 0;
    char tmp_path[256];
    char bak_path[256];
    BOOL had_existing;
    BOOL bak_created = FALSE;

    in = Open(src, MODE_OLDFILE);
    if (!in) return FALSE;

    if (Seek(in, 0, OFFSET_END) == -1) { Close(in); return FALSE; }
    src_size = Seek(in, 0, OFFSET_CURRENT);
    if (src_size < 0 || Seek(in, 0, OFFSET_BEGINNING) == -1) {
        Close(in);
        return FALSE;
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.new", (const char *)dst);

    out = Open((CONST_STRPTR)tmp_path, MODE_NEWFILE);
    if (!out) {
        Close(in);
        return FALSE;
    }

    buf = AllocVec(32768, MEMF_ANY);
    if (!buf) {
        Close(out);
        Close(in);
        DeleteFile((CONST_STRPTR)tmp_path);
        return FALSE;
    }

    while ((nread = Read(in, buf, 32768)) > 0) {
        if (Write(out, buf, nread) != nread) {
            ok = FALSE;
            break;
        }
        written += nread;
    }
    if (nread < 0) ok = FALSE;

    FreeVec(buf);
    Close(out);
    Close(in);

    if (ok && written == src_size) {
        snprintf(bak_path, sizeof(bak_path), "%s.bak", (const char *)dst);
        had_existing = mp_file_exists(dst);

        if (had_existing) {
            /* Clear any stale .bak left over from a prior failed attempt
             * first - Rename() fails if the destination name exists. */
            DeleteFile((CONST_STRPTR)bak_path);
            if (Rename(dst, (CONST_STRPTR)bak_path)) {
                bak_created = TRUE;
            } else {
                /* Couldn't even move the current file aside - stop here
                 * rather than risk leaving dst in an unknown state. */
                DeleteFile((CONST_STRPTR)tmp_path);
                return FALSE;
            }
        }

        if (Rename((CONST_STRPTR)tmp_path, dst)) {
            if (bak_created) DeleteFile((CONST_STRPTR)bak_path);
            return TRUE;
        }

        /* The new file didn't make it into place - put the previously
         * working one back rather than leaving dst missing/truncated. */
        if (bak_created) Rename((CONST_STRPTR)bak_path, dst);
    }

    DeleteFile((CONST_STRPTR)tmp_path);
    return FALSE;
}

/* Same NIL:-handles convention as mp_launch_printer_prefs() below: an
 * async SystemTags() child must not inherit and later close this program's
 * (and its launching Shell's) own console handles. Multiview displays an
 * AmigaGuide file directly, so no amigaguide.library calls are needed here.
 *
 * PROGDIR: is deliberately NOT passed straight through in the command
 * string, unlike the Art/ and Drivers/ PROGDIR: paths used elsewhere in
 * this file: those are opened directly by THIS process, where PROGDIR: is
 * a valid local assign pointing at this program's own drawer. This
 * command line instead runs in a brand-new Process that SystemTags()
 * spawns to run Multiview - PROGDIR: is local to the process that has it,
 * not inherited by a freshly spawned one, so that new process's own
 * PROGDIR: (if it resolves at all) has no reason to still mean "this
 * program's drawer". The Help menu item silently did nothing as a result:
 * Multiview looked for the guide next to wherever ITS OWN idea of
 * PROGDIR: pointed, generally failed to find it, and its own std{in,out}
 * were redirected to NIL: like the rest of this async launch, so nothing
 * was visible either way. Resolving PROGDIR: to a real absolute path
 * *before* building the command line - while still running as this
 * process, where the assign is valid - sidesteps that entirely. */
static void mp_launch_help_guide(void) {
    BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    BPTR out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    BPTR lock;
    char dir[192];
    char cmd[256];
    BOOL resolved = FALSE;

    lock = Lock((CONST_STRPTR)"PROGDIR:", ACCESS_READ);
    if (lock) {
        resolved = NameFromLock(lock, (STRPTR)dir, sizeof(dir));
        UnLock(lock);
    }

    if (resolved) {
        size_t len = strlen(dir);
        const char *sep = (len && dir[len - 1] == ':') ? "" : "/";
        snprintf(cmd, sizeof(cmd), "Multiview \"%s%sMintPrintSettings.guide\"",
                 dir, sep);
    } else {
        /* Could not resolve PROGDIR: at all (should not happen for a
         * normally-launched program) - fall back to the old string and
         * let the failure path below report it, rather than not trying. */
        strcpy(cmd, "Multiview PROGDIR:MintPrintSettings.guide");
    }

    if (SystemTags((CONST_STRPTR)cmd,
                   SYS_Asynch, TRUE,
                   SYS_Input, (ULONG)in, SYS_Output, (ULONG)out,
                   TAG_DONE) != 0) {
        printf("Could not open MintPrintSettings.guide automatically\n");
        printf("It should be in the same drawer as MintPrintSettings.\n");
        if (in) Close(in);
        if (out) Close(out);
    }
}

static void mp_launch_printer_prefs(void) {
    /* SYS_Asynch without explicit SYS_Input/SYS_Output shares the CALLER's
     * own console handles with the new process - and an async process
     * closes its input/output when it exits, which then closes the
     * caller's (this program's, and its launching Shell's) console out
     * from under it. Give the child its own private NIL: handles instead
     * so it owns and closes only those. */
    BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    BPTR out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);

    if (SystemTags((CONST_STRPTR)"SYS:Prefs/Printer", SYS_Asynch, TRUE,
                   SYS_Input, (ULONG)in, SYS_Output, (ULONG)out,
                   TAG_DONE) != 0) {
        printf("Could not launch SYS:Prefs/Printer automatically\n");
        printf("Please open Printer preferences manually.\n");
        if (in) Close(in);
        if (out) Close(out);
    }
}

/* Reads this project's own driver build version out of a driver file, by
 * scanning for the driver's own "$VER: MintPRINT <version>.<revision>"
 * string embedded in printertag.s/printertag_classic.s (see there for why:
 * the compiled driver FILE on disk is a standard AmigaDOS hunk-format load
 * module, not a raw blob starting at its code entry point, so there is no
 * reliable FIXED BYTE OFFSET to read this from - a scannable marker is
 * exactly what AmigaOS's own "Version" command already does for "$VER:"
 * strings, so this now scans for the real thing rather than the ad-hoc
 * "MPDRVREV:<decimal>" prefix earlier driver revisions used). Returns
 * FALSE if the file can't be opened or the string isn't found. */
#define MP_DRIVER_VER_MARKER "$VER: MintPRINT "
#define MP_DRIVER_VER_SCAN_MAX 65536

static BOOL mp_read_driver_version(CONST_STRPTR path, struct MPDriverVersion *out) {
    BPTR file;
    UBYTE *buf;
    LONG got;
    LONG marker_len = (LONG)strlen(MP_DRIVER_VER_MARKER);
    LONG i;
    BOOL found = FALSE;

    file = Open(path, MODE_OLDFILE);
    if (!file) return FALSE;

    buf = AllocVec(MP_DRIVER_VER_SCAN_MAX, MEMF_ANY);
    if (!buf) {
        Close(file);
        return FALSE;
    }

    got = Read(file, buf, MP_DRIVER_VER_SCAN_MAX);
    Close(file);

    if (got < marker_len) {
        FreeVec(buf);
        return FALSE;
    }

    for (i = 0; i <= got - marker_len; i++) {
        if (memcmp(buf + i, MP_DRIVER_VER_MARKER, marker_len) == 0) {
            LONG j = i + marker_len;
            ULONG version = 0, revision = 0;
            BOOL have_version = FALSE, have_revision = FALSE;

            while (j < got && buf[j] >= '0' && buf[j] <= '9') {
                version = version * 10UL + (ULONG)(buf[j] - '0');
                have_version = TRUE;
                j++;
            }
            if (have_version && j < got && buf[j] == '.') {
                j++;
                while (j < got && buf[j] >= '0' && buf[j] <= '9') {
                    revision = revision * 10UL + (ULONG)(buf[j] - '0');
                    have_revision = TRUE;
                    j++;
                }
            }
            if (have_version) {
                out->version = (UWORD)version;
                out->revision = have_revision ? (UWORD)revision : 0;
                found = TRUE;
            }
            break;
        }
    }

    FreeVec(buf);
    return found;
}

/* TRUE if a is a newer driver build than b - version compared first, then
 * revision within a matching version, the same precedence AmigaOS's own
 * version.revision pairs use. */
static BOOL mp_driver_version_newer(const struct MPDriverVersion *a,
                                    const struct MPDriverVersion *b) {
    if (a->version != b->version) return a->version > b->version;
    return a->revision > b->revision;
}

static void show_about(struct Window *win) {
    struct EasyStruct es;
    char msg[512];
    char installed_str[32];
    char bundled_str[32];
    char os_desc[64];
    struct MPDriverVersion installed_ver, bundled_ver;
    CONST_STRPTR src_path = MINTPRINT_DRIVER_SRC;
    CONST_STRPTR variant_name = mp_needs_os31_driver() ? "OS2.0-3.1" : "V44+";

    mp_describe_amiga_os(os_desc, sizeof(os_desc));

    if (mp_read_driver_version(MINTPRINT_DRIVER_DEST, &installed_ver)) {
        snprintf(installed_str, sizeof(installed_str), "v%u.%u",
                 (unsigned)installed_ver.version, (unsigned)installed_ver.revision);
    } else {
        strcpy(installed_str, "not installed");
    }
    if (mp_read_driver_version(src_path, &bundled_ver)) {
        snprintf(bundled_str, sizeof(bundled_str), "v%u.%u",
                 (unsigned)bundled_ver.version, (unsigned)bundled_ver.revision);
    } else {
        strcpy(bundled_str, "not found");
    }

    /* Deliberately short lines, and no PROGDIR: path shown here - see the
     * comment on mp_describe_amiga_os() above for why: EasyRequest sizes
     * its window to the single widest line with no wrapping, and this
     * requester's own longest line was pushing the window off-screen on a
     * default low-resolution AmigaOS display. */
    snprintf(msg, sizeof(msg),
        "MintPRINT v" MINTPRINT_SETTINGS_VERSION
        " - IPP/AirPrint printing for AmigaOS\n\n"
        "Detected: %s\n"
        "Driver build: %s\n\n"
        "Installed driver: %s\n"
        "Bundled driver: %s\n\n"
        "Bug reports and source:\n"
        "github.com/boingball/MintPRINT\n\n"
        "If this saved you a trip to the printer shop:\n"
        "buymeacoffee.com/boingball",
        os_desc, variant_name,
        installed_str, bundled_str);

    es.es_StructSize = sizeof(struct EasyStruct);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)"About MintPrint Settings";
    es.es_TextFormat = (UBYTE *)msg;
    es.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequest(win, &es, NULL);
}

static void check_and_offer_driver_install(struct Window *win) {
    struct EasyStruct es;
    char msg[256];
    char os_desc[64];
    struct MPDriverVersion src_ver, dest_ver;
    BOOL have_src_ver, have_dest_ver;
    CONST_STRPTR src_path = MINTPRINT_DRIVER_SRC;
    CONST_STRPTR variant_name = mp_needs_os31_driver() ? "OS2.0-3.1" : "V44+";

    mp_describe_amiga_os(os_desc, sizeof(os_desc));
    printf("Detected %s (workbench.library v%u) - using the %s driver (%s).\n",
           os_desc, (unsigned)mp_os_version(), variant_name, src_path);

    if (!mp_file_exists(src_path)) {
        printf("Bundled driver not found at %s; skipping install check.\n", src_path);
        printf("Check that both Drivers/MintPRINT-V44/ and Drivers/MintPRINT-OS31/\n");
        printf("are present next to this program.\n");
        return;
    }

    es.es_StructSize = sizeof(struct EasyStruct);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)"MintPrint Settings";

    if (mp_file_exists(MINTPRINT_DRIVER_DEST)) {
        /* Already installed - only bother the user if the copy bundled
         * next to this program is a newer build than what's installed.
         * An installed driver with no readable "$VER: MintPRINT" string
         * at all (predates version tracking - true for anyone who hasn't
         * updated since before it was added) is NOT "up to date": it's
         * unreadable because it's older, not because it's current. Only
         * skip the prompt when the installed version is actually known
         * and already at least as new as the bundled one. */
        have_src_ver = mp_read_driver_version(src_path, &src_ver);
        have_dest_ver = mp_read_driver_version(MINTPRINT_DRIVER_DEST, &dest_ver);

        if (!have_src_ver) {
            return; /* nothing to offer - can't even read the bundled copy */
        }
        if (have_dest_ver && !mp_driver_version_newer(&src_ver, &dest_ver)) {
            return; /* installed copy is already current */
        }

        if (have_dest_ver) {
            snprintf(msg, sizeof(msg),
                     "A newer MintPRINT driver is available.\n"
                     "Installed: v%u.%u  Bundled: v%u.%u\n\n"
                     "Detected: %s (%s driver)\n\n"
                     "Update DEVS:Printers/MintPRINT now?",
                     (unsigned)dest_ver.version, (unsigned)dest_ver.revision,
                     (unsigned)src_ver.version, (unsigned)src_ver.revision,
                     os_desc, variant_name);
        } else {
            snprintf(msg, sizeof(msg),
                     "A newer MintPRINT driver is available.\n"
                     "Installed: pre-versioning  Bundled: v%u.%u\n\n"
                     "Detected: %s (%s driver)\n\n"
                     "Update DEVS:Printers/MintPRINT now?",
                     (unsigned)src_ver.version, (unsigned)src_ver.revision,
                     os_desc, variant_name);
        }
        es.es_TextFormat = (UBYTE *)msg;
        es.es_GadgetFormat = (UBYTE *)"Update|Later";

        if (!EasyRequest(win, &es, NULL)) return;

        if (mp_copy_file(src_path, MINTPRINT_DRIVER_DEST)) {
            printf("Updated MintPRINT driver to v%u.%u in DEVS:Printers/MintPRINT\n",
                   (unsigned)src_ver.version, (unsigned)src_ver.revision);
            printf("Reboot (or otherwise unload the old driver segment) before printing.\n");

            es.es_TextFormat = (UBYTE *)"MintPRINT driver updated.\n\nReboot before printing - the old driver segment\nalready resident in memory will not pick up this\nfile until then.";
            es.es_GadgetFormat = (UBYTE *)"OK";
            EasyRequest(win, &es, NULL);
        } else {
            es.es_TextFormat = (UBYTE *)"Could not copy the driver to DEVS:Printers/.\nCheck disk space and write access.";
            es.es_GadgetFormat = (UBYTE *)"OK";
            EasyRequest(win, &es, NULL);
        }
        return;
    }

    snprintf(msg, sizeof(msg),
             "The MintPRINT driver is not installed in\n"
             "DEVS:Printers/.\n\n"
             "Detected: %s (%s driver)\n\n"
             "Install it now?",
             os_desc, variant_name);
    es.es_TextFormat = (UBYTE *)msg;
    es.es_GadgetFormat = (UBYTE *)"Install|Cancel";

    if (EasyRequest(win, &es, NULL)) {
        if (mp_copy_file(src_path, MINTPRINT_DRIVER_DEST)) {
            printf("Installed MintPRINT driver to DEVS:Printers/MintPRINT\n");

            es.es_TextFormat = (UBYTE *)"MintPRINT driver installed.\n\nOpen Printer preferences now and select\n'MintPRINT' as your printer, then save.\n\nReboot before printing - a driver segment already\nresident in memory will not pick up this file until then.";
            es.es_GadgetFormat = (UBYTE *)"Open Printer Prefs|Later";
            if (EasyRequest(win, &es, NULL)) {
                mp_launch_printer_prefs();
            }
        } else {
            es.es_TextFormat = (UBYTE *)"Could not copy the driver to DEVS:Printers/.\nCheck disk space and write access.";
            es.es_GadgetFormat = (UBYTE *)"OK";
            EasyRequest(win, &es, NULL);
        }
    }
}

/* ---------------------------------------------------------------------
 * LAN printer discovery (SSDP)
 *
 * Sends a single SSDP M-SEARCH multicast and collects distinct source
 * addresses that reply within a few seconds. Any AirPrint/network printer
 * that answers UPnP discovery (most consumer inkjets/lasers do, alongside
 * mDNS) shows up here as a candidate; the actual IPP capability check
 * still goes through the same query_printer_attributes() used by the
 * Query button once the user picks one from the list.
 * ------------------------------------------------------------------- */
static void ssdp_extract_server(const char *buf, char *out, int out_size) {
    const char *p = strstr(buf, "SERVER:");
    if (!p) p = strstr(buf, "Server:");
    if (!p) p = strstr(buf, "server:");
    out[0] = '\0';
    if (p) {
        int i = 0;
        p += 7;
        while (*p == ' ') p++;
        while (*p && *p != '\r' && *p != '\n' && i < out_size - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
    }
}

static BOOL discovery_ip_seen(struct DiscoveredPrinter *results, int count, const char *ip) {
    int i;
    for (i = 0; i < count; i++) {
        if (strcmp(results[i].ip, ip) == 0) return TRUE;
    }
    return FALSE;
}

static int ssdp_discover_printers(struct DiscoveredPrinter *results, int max_results) {
    int sockfd;
    struct sockaddr_in dest;
    char msearch[256];
    char *buf;
    int count = 0;
    int poll_num;
    const int max_polls = 10; /* ~500ms per poll => ~5s total scan time */

    if (max_results <= 0) return 0;

    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        printf("Discovery: could not create UDP socket\n");
        return 0;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(1900);
    dest.sin_addr.s_addr = inet_addr((STRPTR)"239.255.255.250");

    snprintf(msearch, sizeof(msearch),
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: ssdp:all\r\n"
        "\r\n");

    if (sendto(sockfd, msearch, strlen(msearch), 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        printf("Discovery: SSDP send failed (no route to 239.255.255.250?)\n");
        CloseSocket(sockfd);
        return 0;
    }

    buf = malloc(1024);
    if (!buf) {
        CloseSocket(sockfd);
        return 0;
    }

    for (poll_num = 0; poll_num < max_polls && count < max_results; poll_num++) {
        fd_set readfds;
        struct timeval tv;
        long ready;
        struct sockaddr_in from;
        socklen_t fromlen;
        ssize_t received;

        if (window) {
            struct IntuiMessage *imsg;
            while ((imsg = GT_GetIMsg(window->UserPort))) {
                GT_ReplyIMsg(imsg);
            }
        }

        /* Bound each poll to ~500ms with WaitSelect rather than trusting
         * SO_RCVTIMEO on a datagram socket (not every bsdsocket.library
         * stack honours it) or a non-blocking-mode ioctl (this NDK's name
         * for it, FNONBIO, turned out not to work either). WaitSelect is
         * the one bsdsocket.library primitive this is built directly on. */
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        ready = WaitSelect(sockfd + 1, &readfds, NULL, NULL, &tv, NULL);
        if (ready <= 0) {
            continue; /* timeout or error this poll; try again */
        }

        fromlen = sizeof(from);
        memset(&from, 0, sizeof(from));
        received = recvfrom(sockfd, buf, 1023, 0, (struct sockaddr *)&from, &fromlen);
        if (received <= 0) {
            continue;
        }
        buf[received] = '\0';

        {
            char ipstr[16];
            const unsigned char *addr_bytes = (const unsigned char *)&from.sin_addr;

            /* sin_addr is already in network (big-endian) byte order, so the
             * raw bytes are the dotted-decimal octets left to right. Formats
             * manually rather than via inet_ntoa(), which this NDK does not
             * declare for bsdsocket.library. */
            snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u",
                     addr_bytes[0], addr_bytes[1], addr_bytes[2], addr_bytes[3]);

            /* Skip loopback replies (e.g. the host's own SSDP responder
             * echoing back through some emulated/NAT network setups) -
             * never a real network printer. */
            if (addr_bytes[0] == 127) {
                continue;
            }

            if (ipstr[0] && !discovery_ip_seen(results, count, ipstr)) {
                char server_info[64];
                ssdp_extract_server(buf, server_info, sizeof(server_info));

                strncpy(results[count].ip, ipstr, sizeof(results[count].ip) - 1);
                results[count].ip[sizeof(results[count].ip) - 1] = '\0';

                if (server_info[0]) {
                    snprintf(results[count].label, sizeof(results[count].label),
                             "%s (%s)", ipstr, server_info);
                } else {
                    snprintf(results[count].label, sizeof(results[count].label),
                             "%s", ipstr);
                }

                printf("Discovery: found %s\n", results[count].label);
                count++;
            }
        }
    }

    free(buf);
    CloseSocket(sockfd);
    return count;
}

/* ---------------------------------------------------------------------
 * LAN printer discovery (mDNS / Bonjour / AirPrint)
 *
 * Most current printers advertise IPP over mDNS-SD (_ipp._tcp.local),
 * not SSDP, so this is the discovery path that actually matters for
 * AirPrint-style printers. Builds a minimal DNS PTR query by hand (no
 * name compression in the query - only ever one question) with the "QU"
 * unicast-response bit set, so responders reply directly to our source
 * port instead of over multicast. That means this never needs to join
 * the 224.0.0.251 multicast group to receive replies, keeping it on the
 * same plain send/WaitSelect/recvfrom shape already proven for SSDP.
 *
 * Deliberately does not decode the DNS response payload (PTR/SRV/TXT
 * records, name-compression pointers, ...): that is real parsing work
 * with real edge cases, and getting it wrong risks the same kind of
 * lock-up/crash this file has already hit twice on this NDK. All that is
 * used from a reply is which address it came from - good enough to
 * populate the picker; the follow-up IPP query after selection is what
 * actually pulls in the printer's real details.
 * ------------------------------------------------------------------- */
static int build_mdns_ptr_query(unsigned char *buf, int buf_size) {
    static const unsigned char header[12] = {
        0x00, 0x00, /* ID - unused, mDNS clients don't need to match it */
        0x00, 0x00, /* Flags - standard query */
        0x00, 0x01, /* QDCOUNT = 1 */
        0x00, 0x00, /* ANCOUNT */
        0x00, 0x00, /* NSCOUNT */
        0x00, 0x00  /* ARCOUNT */
    };
    static const char *labels[] = { "_ipp", "_tcp", "local", NULL };
    int off;
    int i;

    if (buf_size < 33) return 0;

    memcpy(buf, header, sizeof(header));
    off = sizeof(header);

    for (i = 0; labels[i]; i++) {
        int len = (int)strlen(labels[i]);
        buf[off++] = (unsigned char)len;
        memcpy(buf + off, labels[i], len);
        off += len;
    }
    buf[off++] = 0x00; /* root label terminator */

    buf[off++] = 0x00; buf[off++] = 0x0C; /* QTYPE = PTR (12) */
    buf[off++] = 0x80; buf[off++] = 0x01; /* QCLASS = IN, QU bit set */

    return off;
}

/* Appends newly-found, distinct, non-loopback responders to results[],
 * starting at index *count_io, up to max_results. Returns the new count. */
static int mdns_discover_printers(struct DiscoveredPrinter *results, int count_io, int max_results) {
    int sockfd;
    struct sockaddr_in dest;
    unsigned char query[64];
    int query_len;
    char *buf;
    int count = count_io;
    int poll_num;
    const int max_polls = 10; /* ~500ms per poll => ~5s total scan time */

    if (count >= max_results) return count;

    query_len = build_mdns_ptr_query(query, sizeof(query));
    if (query_len <= 0) {
        printf("Discovery: could not build mDNS query\n");
        return count;
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        printf("Discovery: could not create mDNS socket\n");
        return count;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(5353);
    dest.sin_addr.s_addr = inet_addr((STRPTR)"224.0.0.251");

    if (sendto(sockfd, (char *)query, query_len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        printf("Discovery: mDNS send failed (no route to 224.0.0.251?)\n");
        CloseSocket(sockfd);
        return count;
    }

    buf = malloc(1024);
    if (!buf) {
        CloseSocket(sockfd);
        return count;
    }

    for (poll_num = 0; poll_num < max_polls && count < max_results; poll_num++) {
        fd_set readfds;
        struct timeval tv;
        long ready;
        struct sockaddr_in from;
        socklen_t fromlen;
        ssize_t received;

        if (window) {
            struct IntuiMessage *imsg;
            while ((imsg = GT_GetIMsg(window->UserPort))) {
                GT_ReplyIMsg(imsg);
            }
        }

        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        ready = WaitSelect(sockfd + 1, &readfds, NULL, NULL, &tv, NULL);
        if (ready <= 0) {
            continue;
        }

        fromlen = sizeof(from);
        memset(&from, 0, sizeof(from));
        received = recvfrom(sockfd, buf, 1023, 0, (struct sockaddr *)&from, &fromlen);
        /* A real DNS response needs at least a 12-byte header with a
         * non-zero answer count, and a unicast QU reply comes from the
         * responder's own port 5353. Cheap enough sanity checks to reject
         * unrelated UDP traffic without decoding the message itself. */
        if (received < 12 || from.sin_port != htons(5353)) {
            continue;
        }
        if (buf[6] == 0 && buf[7] == 0) {
            continue; /* ANCOUNT == 0: not actually answering anything */
        }

        {
            char ipstr[16];
            const unsigned char *addr_bytes = (const unsigned char *)&from.sin_addr;

            snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u",
                     addr_bytes[0], addr_bytes[1], addr_bytes[2], addr_bytes[3]);

            if (addr_bytes[0] == 127) {
                continue;
            }

            if (!discovery_ip_seen(results, count, ipstr)) {
                strncpy(results[count].ip, ipstr, sizeof(results[count].ip) - 1);
                results[count].ip[sizeof(results[count].ip) - 1] = '\0';
                snprintf(results[count].label, sizeof(results[count].label),
                         "%s (mDNS/IPP)", ipstr);

                printf("Discovery: found %s\n", results[count].label);
                count++;
            }
        }
    }

    free(buf);
    CloseSocket(sockfd);
    return count;
}

/* Runs both discovery mechanisms and merges the results: SSDP catches
 * printers/print servers that answer UPnP discovery, mDNS catches the
 * more common AirPrint/Bonjour-style IPP advertisement. */
static int discover_printers_on_lan(struct DiscoveredPrinter *results, int max_results) {
    int count;

    printf("Searching LAN for printers (SSDP)...\n");
    count = ssdp_discover_printers(results, max_results);

    printf("Searching LAN for printers (mDNS)...\n");
    count = mdns_discover_printers(results, count, max_results);

    return count;
}

/* Small GadTools dialog listing discovered candidates as a cycle gadget.
 * Mirrors the main window's CreateContext/CreateGadget/OpenWindowTags
 * pattern so it reuses the same, already-proven idioms. */
static BOOL run_discovery_selection(struct Window *parent,
                                     struct DiscoveredPrinter *results,
                                     int count,
                                     char *chosen_ip,
                                     int chosen_ip_size) {
    struct Screen *dscreen;
    APTR dvi;
    struct Gadget *dglist = NULL;
    struct Gadget *gad;
    struct NewGadget ng;
    struct Window *dwin;
    STRPTR *labels;
    BOOL picked = FALSE;
    BOOL terminated = FALSE;
    UWORD topborder;
    int i;
    ULONG selected = 0;

    (void)parent;

    if (count <= 0) return FALSE;

    dscreen = LockPubScreen(NULL);
    if (!dscreen) return FALSE;

    dvi = GetVisualInfo(dscreen, TAG_DONE);
    if (!dvi) {
        UnlockPubScreen(NULL, dscreen);
        return FALSE;
    }

    labels = AllocVec((count + 1) * sizeof(STRPTR), MEMF_CLEAR);
    if (!labels) {
        FreeVisualInfo(dvi);
        UnlockPubScreen(NULL, dscreen);
        return FALSE;
    }
    for (i = 0; i < count; i++) {
        labels[i] = (STRPTR)results[i].label;
    }
    labels[count] = NULL;

    topborder = dscreen->WBorTop + (dscreen->Font->ta_YSize + 1);

    gad = CreateContext(&dglist);
    if (!gad) {
        FreeVec(labels);
        FreeVisualInfo(dvi);
        UnlockPubScreen(NULL, dscreen);
        return FALSE;
    }

    ng.ng_TextAttr = &Topaz80;
    ng.ng_VisualInfo = dvi;
    ng.ng_Flags = 0;
    ng.ng_LeftEdge = 10;
    ng.ng_TopEdge = 10 + topborder;
    ng.ng_Width = 410;
    ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"Found:";
    ng.ng_GadgetID = GAD_DISC_CYCLE;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)labels,
        GTCY_Active, 0,
        TAG_DONE);
    if (!gad) {
        FreeGadgets(dglist);
        FreeVec(labels);
        FreeVisualInfo(dvi);
        UnlockPubScreen(NULL, dscreen);
        return FALSE;
    }

    ng.ng_TopEdge += 26;
    ng.ng_LeftEdge = 10;
    ng.ng_Width = 120;
    ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"_Use Selected";
    ng.ng_GadgetID = GAD_DISC_USE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_',
        TAG_DONE);
    if (!gad) {
        FreeGadgets(dglist);
        FreeVec(labels);
        FreeVisualInfo(dvi);
        UnlockPubScreen(NULL, dscreen);
        return FALSE;
    }

    ng.ng_LeftEdge = 290;
    ng.ng_Width = 120;
    ng.ng_GadgetText = (STRPTR)"_Cancel";
    ng.ng_GadgetID = GAD_DISC_CANCEL;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_',
        TAG_DONE);
    if (!gad) {
        FreeGadgets(dglist);
        FreeVec(labels);
        FreeVisualInfo(dvi);
        UnlockPubScreen(NULL, dscreen);
        return FALSE;
    }

    dwin = OpenWindowTags(NULL,
        WA_Title, (ULONG)"Select Discovered Printer",
        WA_Gadgets, (ULONG)dglist,
        WA_Width, 430,
        WA_InnerHeight, 70,
        WA_DragBar, TRUE,
        WA_DepthGadget, TRUE,
        WA_Activate, TRUE,
        WA_CloseGadget, TRUE,
        WA_SimpleRefresh, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | CYCLEIDCMP,
        WA_PubScreen, (ULONG)dscreen,
        TAG_DONE);

    if (!dwin) {
        FreeGadgets(dglist);
        FreeVec(labels);
        FreeVisualInfo(dvi);
        UnlockPubScreen(NULL, dscreen);
        return FALSE;
    }

    GT_RefreshWindow(dwin, NULL);

    while (!terminated) {
        struct IntuiMessage *imsg;

        Wait(1L << dwin->UserPort->mp_SigBit);
        imsg = GT_GetIMsg(dwin->UserPort);
        while (!terminated && imsg) {
            struct Gadget *g = (struct Gadget *)imsg->IAddress;
            ULONG cls = imsg->Class;
            UWORD code = imsg->Code;
            GT_ReplyIMsg(imsg);

            if (cls == IDCMP_CLOSEWINDOW) {
                terminated = TRUE;
            } else if (cls == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(dwin);
                GT_EndRefresh(dwin, TRUE);
            } else if (cls == IDCMP_GADGETUP) {
                if (g->GadgetID == GAD_DISC_CYCLE) {
                    if ((ULONG)code < (ULONG)count)
                        selected = (ULONG)code;
                } else if (g->GadgetID == GAD_DISC_CANCEL) {
                    terminated = TRUE;
                } else if (g->GadgetID == GAD_DISC_USE) {
                    if (selected < (ULONG)count) {
                        strncpy(chosen_ip, results[selected].ip, chosen_ip_size - 1);
                        chosen_ip[chosen_ip_size - 1] = '\0';
                        picked = TRUE;
                    }
                    terminated = TRUE;
                }
            }
            imsg = GT_GetIMsg(dwin->UserPort);
        }
    }

    CloseWindow(dwin);
    FreeGadgets(dglist);
    FreeVec(labels);
    FreeVisualInfo(dvi);
    UnlockPubScreen(NULL, dscreen);

    return picked;
}

/* Spooler management window: lists jobs the driver is currently tracking
 * (see driver_core.c's mp_job_begin()/mp_write_job_status() and the
 * MPSPOOL drawer both write into) with their live STATE and, for a failed
 * one, why - and lets the user delete a held job, retry/reprint it, or
 * print extra copies. */
#define MP_SPOOL_LIST_MAX 32

struct MPSpoolJobEntry {
    struct Node node; /* must be first - GadTools ListView owns this list */
    char job_path[MAX_ATTR_LEN + 48];
    char status_path[MAX_ATTR_LEN + 56];
    char basename[80]; /* job_path's filename alone, for the list label */
    struct DateStamp date; /* status sidecar's fib_Date - sort key, newest first */
    char state[16];
    char reason[64];
    char host[64]; /* HOST= from the .status sidecar - who this job targets */
    int port;      /* PORT=, -1 if the sidecar has none (pre-1.3.0 job) */
    char label[192]; /* node.ln_Name points here */
};

/* Kept static (not a stack local) so a Refresh - which redoes this scan
 * in place without recursing - never risks stack growth across repeated
 * presses. Only ever touched from the mp_spool_win_*() functions below,
 * single-threaded, one instance of the window open at a time. */
static struct MPSpoolJobEntry mp_spool_jobs[MP_SPOOL_LIST_MAX];

/* The Spooler window's entire live state, as globals rather than one
 * function's stack locals - process_window_events()'s main loop needs to
 * reach all of it between messages, since the window is non-modal: it
 * Waits on this window's UserPort signal bit alongside the main window's
 * own (see that function), so both windows keep processing input at the
 * same time instead of one blocking the other the way a nested modal
 * event loop would. g_spool_win doubling as "is the window open" (NULL
 * when it isn't) is what lets that Wait() call skip this signal bit
 * entirely while the window is closed. */
static struct Window *g_spool_win = NULL;
static struct Screen *g_spool_screen = NULL;
static APTR g_spool_vi = NULL;
static struct Gadget *g_spool_sglist = NULL;
static struct Gadget *g_spool_job_listview = NULL;
static struct Gadget *g_spool_delete_gadget = NULL;
static struct Gadget *g_spool_retry_gadget = NULL;
static struct Gadget *g_spool_copies_gadget = NULL;
static struct List g_spool_job_list;
static int g_spool_count = 0;
static ULONG g_spool_selected = 0;
static BOOL g_spool_selection_made = FALSE;
static int g_spool_retry_unit = -1; /* -1 = not yet defaulted to current_unit_index */
/* -1 = not yet positioned; the window is centred the first time it opens
 * and then keeps whatever LeftEdge/TopEdge it was last closed at (drags
 * included), rather than recentring itself every time it is reopened. */
static WORD g_spool_win_left = -1;
static WORD g_spool_win_top = -1;

/* Turns the numeric (error, http_status, ipp_status) triple a job's
 * .status sidecar records (driver_core.c's mp_write_job_status(),
 * ERROR=) into the kind of short phrase a print queue normally shows for
 * "why did this fail". error is mp_ipp_print_document()'s
 * (driver/ipp_client.c) own return code, naming exactly which step
 * failed; ipp_status is only meaningful once a real IPP response came
 * back (error -15/-16 below - see that function's own rc comments).
 * IPP's response status alone can't distinguish "out of paper" from "out
 * of ink" - that lives in a separate printer-state-reasons attribute
 * this driver doesn't currently request - so 0x0504 (server-error-
 * device-error) is reported as a hardware problem in general, not a
 * specific cause; naming one precisely would be a guess this data
 * doesn't support. */
static void mp_spool_error_reason(LONG error, LONG http_status,
                                  UWORD ipp_status, char *out, size_t out_cap)
{
    const char *reason;

    switch (error) {
        case 0:   reason = "OK"; break;
        case -2:  reason = "Could not open job file"; break;
        case -3:  reason = "Job file is empty"; break;
        case -7:
        case -8:  reason = "Network unavailable"; break;
        case -9:  reason = "Invalid printer address"; break;
        case -10: reason = "Connection refused"; break;
        case -18: reason = "Connection timed out"; break;
        case -11:
        case -13: reason = "Sending job failed (connection dropped)"; break;
        case -12: reason = "Error reading job file"; break;
        case -14: reason = "Printer sent an invalid response"; break;
        case -17: reason = "Printer accepted job but never responded"; break;
        case -15:
            if (http_status == 401 || http_status == 403)
                reason = "Printer refused (access denied)";
            else if (http_status == 404)
                reason = "Printer rejected request (wrong IPP path?)";
            else if (http_status >= 500)
                reason = "Printer error (HTTP)";
            else
                reason = "Printer rejected request (HTTP)";
            break;
        case -16:
            switch (ipp_status) {
                case 0x0507: reason = "Printer busy"; break;
                case 0x0504:
                    reason = "Printer hardware problem (check paper/ink)";
                    break;
                case 0x0506: reason = "Printer not accepting jobs"; break;
                case 0x0508: reason = "Job canceled at printer"; break;
                default:
                    if (ipp_status >= 0x0500) reason = "Printer error";
                    else if (ipp_status >= 0x0400) reason = "Printer rejected job";
                    else reason = "Printer returned an unexpected status";
                    break;
            }
            break;
        default:  reason = "Failed"; break;
    }

    strncpy(out, reason, out_cap - 1);
    out[out_cap - 1] = '\0';
}

/* Scans <driver_spool_buffer>MPSPOOL/ for job files this driver is
 * tracking. Each *.status sidecar names a job file (the same name minus
 * ".status") and holds its current STATE=/ERROR= - read here so this
 * window never needs any live connection to the driver's spool process,
 * and still shows a job's outcome long after that process (and the print
 * that created it) is gone. Newest first: the timestamp embedded in each
 * job's own filename is DDMMYYHHMMSS, which does not sort correctly as a
 * plain string across month/year boundaries (e.g. "010823" < "150723"
 * lexicographically despite being the later date), so this sorts by each
 * sidecar file's real fib_Date instead - a simple insertion sort, plenty
 * for the at-most MP_SPOOL_LIST_MAX (32) entries here. list is NewList()'d
 * and filled with jobs[0] through jobs[count-1] via AddTail(), in that
 * sorted order, ready for GTLV_Labels. Returns the number of jobs found;
 * 0 if the drawer doesn't exist yet, or Spooler isn't a real device. */
static int mp_scan_spool_jobs(struct MPSpoolJobEntry *jobs, int max_jobs,
                              struct List *list)
{
    char dir_path[MAX_ATTR_LEN + 16];
    BPTR lock;
    /* A plain stack struct, not AllocDosObject(DOS_FIB, ...): Examine()/
     * ExNext() have taken a caller-supplied struct FileInfoBlock * since
     * Kickstart 1.x, and a normal C local already satisfies its longword
     * alignment - one fewer NDK symbol (DOS_FIB, a V36+ addition) to
     * depend on. */
    struct FileInfoBlock fib;
    int count = 0;
    int i;

    NewList(list);
    if (!mp_spool_keep_available()) return 0;

    snprintf(dir_path, sizeof(dir_path), "%sMPSPOOL", driver_spool_buffer);
    lock = Lock((CONST_STRPTR)dir_path, ACCESS_READ);
    if (!lock) return 0;

    if (Examine(lock, &fib)) {
        while (count < max_jobs && ExNext(lock, &fib)) {
            char *name = (char *)fib.fib_FileName;
            size_t len = strlen(name);

            /* fib_DirEntryType < 0 -> plain file, not a drawer. */
            if (fib.fib_DirEntryType < 0 && len > 7 &&
                strcmp(name + len - 7, ".status") == 0) {
                BPTR sfh;
                size_t blen = (len - 7 < sizeof(jobs[count].basename) - 1)
                    ? len - 7 : sizeof(jobs[count].basename) - 1;

                memcpy(jobs[count].basename, name, blen);
                jobs[count].basename[blen] = '\0';
                jobs[count].date = fib.fib_Date;

                snprintf(jobs[count].status_path,
                         sizeof(jobs[count].status_path),
                         "%s/%s", dir_path, name);
                snprintf(jobs[count].job_path, sizeof(jobs[count].job_path),
                         "%s/%s", dir_path, jobs[count].basename);

                strcpy(jobs[count].state, "UNKNOWN");
                jobs[count].reason[0] = '\0';
                jobs[count].host[0] = '\0';
                jobs[count].port = -1;
                sfh = Open((CONST_STRPTR)jobs[count].status_path,
                          MODE_OLDFILE);
                if (sfh) {
                    char status[768];
                    LONG got = Read(sfh, status, sizeof(status) - 1);
                    char *p;

                    /* Some AmigaDOS/NDK combinations are fussy about the
                     * signed char buffer passed to FGets(). Read the small
                     * sidecar in one bounded operation instead; this also
                     * handles older sidecars that contain only STATE/ERROR.
                     */
                    if (got > 0) {
                        status[got] = '\0';
                        p = strstr(status, "STATE=");
                        if (p) {
                            char *eol = strchr(p, '\n');
                            size_t n = eol ? (size_t)(eol - (p + 6))
                                           : strlen(p + 6);
                            if (n >= sizeof(jobs[count].state))
                                n = sizeof(jobs[count].state) - 1;
                            memcpy(jobs[count].state, p + 6, n);
                            jobs[count].state[n] = '\0';
                            trim_config_line(jobs[count].state);
                        }
                        p = strstr(status, "HOST=");
                        if (p) {
                            char *eol = strchr(p, '\n');
                            size_t n = eol ? (size_t)(eol - (p + 5))
                                           : strlen(p + 5);
                            if (n >= sizeof(jobs[count].host))
                                n = sizeof(jobs[count].host) - 1;
                            memcpy(jobs[count].host, p + 5, n);
                            jobs[count].host[n] = '\0';
                            trim_config_line(jobs[count].host);
                        }
                        p = strstr(status, "PORT=");
                        if (p) jobs[count].port = atoi(p + 5);
                        p = strstr(status, "ERROR=");
                        if (p) {
                            long e = 0, h = 0;
                            int ipp = 0;
                            if (sscanf(p + 6, "%ld %ld %d", &e, &h, &ipp) == 3)
                                mp_spool_error_reason(e, h, (UWORD)ipp,
                                    jobs[count].reason,
                                    sizeof(jobs[count].reason));
                        }
                    }
                    Close(sfh);
                }

                ++count;
            }
        }
    }

    UnLock(lock);

    /* Newest first, by fib_Date (ds_Days, then ds_Minute, then ds_Tick -
     * each a plain increasing count, so comparing them in that order is
     * a correct chronological compare). */
    for (i = 1; i < count; ++i) {
        struct MPSpoolJobEntry tmp = jobs[i];
        int j = i - 1;

        while (j >= 0 &&
               (jobs[j].date.ds_Days < tmp.date.ds_Days ||
                (jobs[j].date.ds_Days == tmp.date.ds_Days &&
                 (jobs[j].date.ds_Minute < tmp.date.ds_Minute ||
                  (jobs[j].date.ds_Minute == tmp.date.ds_Minute &&
                   jobs[j].date.ds_Tick < tmp.date.ds_Tick))))) {
            jobs[j + 1] = jobs[j];
            --j;
        }
        jobs[j + 1] = tmp;
    }

    for (i = 0; i < count; ++i) {
        /* "who": the recorded destination for a tracked job, or a dash
         * for one spooled before HOST=/PORT= sidecars existed. */
        char who[24];

        if (jobs[i].host[0])
            snprintf(who, sizeof(who), "%s:%d", jobs[i].host,
                     jobs[i].port > 0 ? jobs[i].port : 80);
        else
            strcpy(who, "-");

        /* Column widths are a compromise: none of this fits comfortably
         * on an Amiga screen without truncating something, and the
         * reason (state names why a job failed - "connection refused",
         * "printer hardware problem", ...) matters more moment-to-moment
         * than the exact job name or destination, both already visible
         * elsewhere (job name in Delete/Retry's own file path; "who" is
         * exactly what the "Retry to:" picker is about to resubmit
         * against). Selecting a row also puts the state and full,
         * untruncated reason in the window title - see
         * mp_spool_win_process()'s GAD_SPOOL_JOB_LIST handling - so
         * nothing here is ever permanently unreadable, just abbreviated
         * for the at-a-glance list. */
        if (jobs[i].reason[0])
            snprintf(jobs[i].label, sizeof(jobs[i].label),
                     "%-20.20s %-10.10s %-15.15s %s", jobs[i].basename,
                     jobs[i].state, who, jobs[i].reason);
        else
            snprintf(jobs[i].label, sizeof(jobs[i].label),
                     "%-20.20s %-10.10s %s", jobs[i].basename,
                     jobs[i].state, who);

        jobs[i].node.ln_Name = jobs[i].label;
        AddTail(list, &jobs[i].node);
    }

    return count;
}

static CONST_STRPTR mp_spool_document_format_for_ext(const char *path)
{
    size_t len = strlen(path);

    if (len > 4 && strcmp(path + len - 4, ".jpg") == 0)
        return (CONST_STRPTR)"image/jpeg";
    if (len > 4 && strcmp(path + len - 4, ".pwg") == 0)
        return (CONST_STRPTR)"image/pwg-raster";
    if (len > 4 && strcmp(path + len - 4, ".pdf") == 0)
        return (CONST_STRPTR)"application/pdf";
    if (len > 3 && strcmp(path + len - 3, ".ps") == 0)
        return (CONST_STRPTR)"application/postscript";
    if (len > 4 && strcmp(path + len - 4, ".urf") == 0)
        return (CONST_STRPTR)"image/urf";
    return (CONST_STRPTR)"application/octet-stream";
}

/* A job is actionable only after the driver has closed the file and reached
 * a terminal state. Allowing Retry/Delete while RENDERING or SUBMITTING can
 * race the driver and either submit a truncated file or remove its live
 * output underneath it. */
static BOOL mp_spool_job_actionable(const struct MPSpoolJobEntry *job)
{
    if (!job) return FALSE;
    return strcmp(job->state, "DONE") == 0 ||
           strcmp(job->state, "FAILED") == 0;
}

static void mp_spool_update_action_buttons(struct Window *win)
{
    BOOL enabled = g_spool_selection_made &&
                   g_spool_selected < (ULONG)g_spool_count &&
                   mp_spool_job_actionable(
                       &mp_spool_jobs[g_spool_selected]);
    if (!win) return;
    GT_SetGadgetAttrs(g_spool_delete_gadget, win, NULL,
                      GA_Disabled, (ULONG)(enabled ? FALSE : TRUE), TAG_DONE);
    GT_SetGadgetAttrs(g_spool_retry_gadget, win, NULL,
                      GA_Disabled, (ULONG)(enabled ? FALSE : TRUE), TAG_DONE);
    GT_SetGadgetAttrs(g_spool_copies_gadget, win, NULL,
                      GA_Disabled, (ULONG)(enabled ? FALSE : TRUE), TAG_DONE);
}

/* Reads HOST=/PORT=/PATH= straight out of a saved Unit%d file, without
 * touching any of the live GUI/driver-config globals (ip_buffer,
 * driver_path_buffer, ...) that reload_current_unit() would disturb - so
 * the Spooler window's Unit picker can target a specific saved profile
 * regardless of which Unit the main window happens to have loaded right
 * now. Returns TRUE only if the file existed and had a non-empty HOST=. */
static BOOL mp_load_unit_endpoint(int idx, char *host, size_t host_cap,
                                  int *port, char *path, size_t path_cap)
{
    BPTR file;
    char env_path[64];
    char envarc_path[64];
    char line[192];
    BOOL found = FALSE;

    host[0] = '\0';
    *port = 80;
    strncpy(path, "/ipp/print", path_cap - 1);
    path[path_cap - 1] = '\0';

    unit_config_path(idx, FALSE, env_path, sizeof(env_path));
    unit_config_path(idx, TRUE, envarc_path, sizeof(envarc_path));

    file = Open((CONST_STRPTR)env_path, MODE_OLDFILE);
    if (!file)
        file = Open((CONST_STRPTR)envarc_path, MODE_OLDFILE);
    if (!file)
        return FALSE;

    while (FGets(file, line, sizeof(line))) {
        trim_config_line(line);
        if (strncmp(line, "HOST=", 5) == 0) {
            strncpy(host, line + 5, host_cap - 1);
            host[host_cap - 1] = '\0';
            found = host[0] != '\0';
        } else if (strncmp(line, "PORT=", 5) == 0) {
            int p = atoi(line + 5);
            if (p >= 1 && p <= 65535) *port = p;
        } else if (strncmp(line, "PATH=", 5) == 0 && line[5] == '/') {
            strncpy(path, line + 5, path_cap - 1);
            path[path_cap - 1] = '\0';
        }
    }
    Close(file);
    return found;
}

/* Resubmits an already-rendered, retained job file over IPP - used for
 * Retry (a held, failed job), Reprint (a successful one sent again), and
 * each pass of multi-copy printing. unit_index selects which saved Unit's
 * HOST/PORT/PATH to send it to (the Spooler window's Unit picker - lets a
 * job be reassigned to a different printer than the one it originally
 * failed against); if that Unit has no saved HOST=, this falls back to
 * whatever the main window currently has loaded, the previous behaviour.
 * Either way, only the endpoint comes from today's settings - the
 * retained file's own content already reflects whichever job-template
 * options (media, colour, quality, ...) were live when it was first
 * rendered, so resending it with today's attributes layered on top would
 * be misleading; empty attributes are the same "nothing extra" no-op
 * mp_ipp_print_document() already treats them as, see that function's own
 * comment in driver/ipp_client.c. Updates the job's own .status sidecar
 * with the outcome and the endpoint actually used, the same
 * STATE=/HOST=/PORT=/ERROR= format the driver itself writes, so a
 * following Refresh shows it. Returns TRUE on success. */
static BOOL mp_spool_retry_job(struct MPSpoolJobEntry *job, int unit_index)
{
    struct MPConfig cfg;
    struct MPIPPResult result;
    CONST_STRPTR document_format;
    char host[64];
    char path[MAX_ATTR_LEN];
    int port = 80;
    LONG rc;
    BPTR sfh;

    memset(&cfg, 0, sizeof(cfg));

    if (!mp_load_unit_endpoint(unit_index, host, sizeof(host), &port,
                               path, sizeof(path)) || !host[0]) {
        int fallback_port = -1;
        if (!parse_ip_and_port(ip_buffer, host, sizeof(host),
                               &fallback_port) || !host[0])
            return FALSE;
        port = (fallback_port > 0 && fallback_port <= 65535)
                   ? fallback_port : 80;
        strncpy(path, driver_path_buffer[0] == '/' ? driver_path_buffer
                                                     : "/ipp/print",
               sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    strncpy(cfg.host, host, sizeof(cfg.host) - 1);
    cfg.port = (UWORD)port;
    strncpy(cfg.path, path, sizeof(cfg.path) - 1);

    document_format = mp_spool_document_format_for_ext(job->job_path);

    result.error = -1;
    result.http_status = 0;
    result.ipp_status = 0xffff;
    result.document_bytes = 0;
    rc = mp_ipp_print_document(&cfg, (CONST_STRPTR)job->job_path,
                               document_format, &result);

    sfh = Open((CONST_STRPTR)job->status_path, MODE_NEWFILE);
    if (sfh) {
        char buf[96];
        int n = snprintf(buf, sizeof(buf), "STATE=%s\n",
                         rc == 0 ? "DONE" : "FAILED");
        Write(sfh, buf, n);
        n = snprintf(buf, sizeof(buf), "HOST=%s\n", cfg.host);
        Write(sfh, buf, n);
        n = snprintf(buf, sizeof(buf), "PORT=%d\n", (int)cfg.port);
        Write(sfh, buf, n);
        if (rc != 0) {
            n = snprintf(buf, sizeof(buf), "ERROR=%ld %ld %d\n",
                        (long)result.error, (long)result.http_status,
                        (int)result.ipp_status);
            Write(sfh, buf, n);
        }
        Close(sfh);
    }

    return rc == 0;
}

/* Small OK/Cancel dialog asking how many copies to print - see
 * GAD_SPOOL_COPIES in mp_spool_win_process() below, which resubmits the
 * selected job that many times (mp_spool_retry_job() per copy) rather
 * than relying on the IPP copies Job Template attribute, since not every
 * printer this driver targets is known to honour it - see the project's
 * own multi-copy design discussion. Returns TRUE if OK was pressed, with
 * *out_copies set to 1-99; FALSE (out_copies untouched past its initial
 * 1) on Cancel or any setup failure. */
static BOOL run_copies_dialog(struct Window *parent, int *out_copies)
{
    struct Screen *cscreen;
    APTR cvi;
    struct Gadget *cglist = NULL;
    struct Gadget *gad;
    struct Gadget *copies_field;
    struct NewGadget ng;
    struct Window *cwin;
    static char copies_buffer[8];
    BOOL confirmed = FALSE;
    BOOL terminated = FALSE;
    UWORD topborder;

    (void)parent;
    *out_copies = 1;
    strcpy(copies_buffer, "1");

    cscreen = LockPubScreen(NULL);
    if (!cscreen) return FALSE;
    cvi = GetVisualInfo(cscreen, TAG_DONE);
    if (!cvi) {
        UnlockPubScreen(NULL, cscreen);
        return FALSE;
    }

    topborder = cscreen->WBorTop + (cscreen->Font->ta_YSize + 1);

    gad = CreateContext(&cglist);
    if (!gad) {
        FreeVisualInfo(cvi);
        UnlockPubScreen(NULL, cscreen);
        return FALSE;
    }

    ng.ng_TextAttr = &Topaz80;
    ng.ng_VisualInfo = cvi;
    ng.ng_Flags = 0;
    ng.ng_LeftEdge = 10;
    ng.ng_TopEdge = 10 + topborder;
    ng.ng_Width = 100;
    ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"Copies:";
    ng.ng_GadgetID = GAD_SPOOL_COPIES_FIELD;
    gad = CreateGadget(STRING_KIND, gad, &ng,
        GTST_String, (ULONG)copies_buffer,
        GTST_MaxChars, sizeof(copies_buffer) - 1,
        GA_Immediate, TRUE,
        TAG_DONE);
    if (!gad) {
        FreeGadgets(cglist);
        FreeVisualInfo(cvi);
        UnlockPubScreen(NULL, cscreen);
        return FALSE;
    }
    copies_field = gad;

    ng.ng_TopEdge += 26;
    ng.ng_LeftEdge = 10;
    ng.ng_Width = 90;
    ng.ng_GadgetText = (STRPTR)"_OK";
    ng.ng_GadgetID = GAD_SPOOL_COPIES_OK;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, GT_Underscore, '_', TAG_DONE);
    if (!gad) {
        FreeGadgets(cglist);
        FreeVisualInfo(cvi);
        UnlockPubScreen(NULL, cscreen);
        return FALSE;
    }

    ng.ng_LeftEdge = 120;
    ng.ng_GadgetText = (STRPTR)"_Cancel";
    ng.ng_GadgetID = GAD_SPOOL_COPIES_CANCEL;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, GT_Underscore, '_', TAG_DONE);
    if (!gad) {
        FreeGadgets(cglist);
        FreeVisualInfo(cvi);
        UnlockPubScreen(NULL, cscreen);
        return FALSE;
    }

    cwin = OpenWindowTags(NULL,
        WA_Title, (ULONG)"Print Copies",
        WA_Gadgets, (ULONG)cglist,
        WA_Width, 230,
        WA_InnerHeight, 70,
        WA_DragBar, TRUE,
        WA_DepthGadget, TRUE,
        WA_Activate, TRUE,
        WA_CloseGadget, TRUE,
        WA_SimpleRefresh, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | STRINGIDCMP,
        WA_PubScreen, (ULONG)cscreen,
        TAG_DONE);
    if (!cwin) {
        FreeGadgets(cglist);
        FreeVisualInfo(cvi);
        UnlockPubScreen(NULL, cscreen);
        return FALSE;
    }

    GT_RefreshWindow(cwin, NULL);

    while (!terminated) {
        struct IntuiMessage *imsg;

        Wait(1L << cwin->UserPort->mp_SigBit);
        imsg = GT_GetIMsg(cwin->UserPort);
        while (!terminated && imsg) {
            struct Gadget *g = (struct Gadget *)imsg->IAddress;
            ULONG cls = imsg->Class;
            GT_ReplyIMsg(imsg);

            if (cls == IDCMP_CLOSEWINDOW) {
                terminated = TRUE;
            } else if (cls == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(cwin);
                GT_EndRefresh(cwin, TRUE);
            } else if (cls == IDCMP_GADGETUP) {
                if (g->GadgetID == GAD_SPOOL_COPIES_CANCEL) {
                    terminated = TRUE;
                } else if (g->GadgetID == GAD_SPOOL_COPIES_OK) {
                    char *value = mp_string_gadget_value(copies_field);
                    int n = value ? atoi(value) : 1;
                    if (n < 1) n = 1;
                    if (n > 99) n = 99;
                    *out_copies = n;
                    confirmed = TRUE;
                    terminated = TRUE;
                }
            }
            imsg = GT_GetIMsg(cwin->UserPort);
        }
    }

    CloseWindow(cwin);
    FreeGadgets(cglist);
    FreeVisualInfo(cvi);
    UnlockPubScreen(NULL, cscreen);
    return confirmed;
}

/* Rescans MPSPOOL and swaps the results into the already-open Spooler
 * window's listview in place - the window itself is never closed. Follows
 * MintAMP's RadioRefreshResults(): detach the gadget from its current
 * label list first (GTLV_Labels/GTLV_Selected set to ~0 tells GadTools to
 * let go of the old struct List's nodes) before touching that struct
 * List's contents, then rebuild it and reattach. Mutating a live
 * LISTVIEW_KIND's list while the gadget still points at it is exactly the
 * kind of thing that produced this window's earlier "selects then
 * deselects itself" symptom - detaching first is what makes the swap
 * safe. *count/*selected/*selection_made are reset the same way a fresh
 * open would: a rescan can renumber, add, or remove rows, so whatever was
 * selected before no longer means anything. Also resyncs
 * Delete/Retry/Copies' enabled state to whether the list has anything in
 * it now, and clears any "select a job first"/"Retrying..." title text
 * left over from whichever action triggered this refresh. */
static void mp_spool_refresh_list_live(struct Window *swin,
                                       struct Gadget *job_listview,
                                       struct Gadget *delete_gadget,
                                       struct Gadget *retry_gadget,
                                       struct Gadget *copies_gadget,
                                       struct List *job_list,
                                       int *count, ULONG *selected,
                                       BOOL *selection_made)
{
    BOOL have_selection;

    GT_SetGadgetAttrs(job_listview, swin, NULL,
                      GTLV_Labels, (ULONG)~0,
                      GTLV_Selected, (ULONG)~0,
                      TAG_DONE);

    *count = mp_scan_spool_jobs(mp_spool_jobs, MP_SPOOL_LIST_MAX, job_list);
    *selected = 0;
    *selection_made = FALSE;
    have_selection = *count > 0;

    GT_SetGadgetAttrs(job_listview, swin, NULL,
                      GTLV_Labels, (ULONG)job_list,
                      GTLV_Selected, (ULONG)~0,
                      GA_Disabled, (ULONG)(have_selection ? FALSE : TRUE),
                      TAG_DONE);
    /* Nothing is selected after a rescan. Keep the action buttons disabled
     * until the user selects a terminal (DONE/FAILED) job. */
    (void)delete_gadget;
    (void)retry_gadget;
    (void)copies_gadget;
    mp_spool_update_action_buttons(swin);
    SetWindowTitles(swin, (STRPTR)"Spooler Management", (STRPTR)~0);
}

/* Closes the Spooler window (if open) and releases everything opening it
 * took - screen lock, visual info, gadget list - leaving g_spool_win NULL
 * so process_window_events()'s Wait() stops listening on its signal bit.
 * Records LeftEdge/TopEdge first so the next mp_spool_win_open() reopens
 * in the same place rather than recentring. */
static void mp_spool_win_close(void)
{
    if (!g_spool_win) return;

    g_spool_win_left = g_spool_win->LeftEdge;
    g_spool_win_top = g_spool_win->TopEdge;
    CloseWindow(g_spool_win);
    g_spool_win = NULL;

    FreeGadgets(g_spool_sglist);
    g_spool_sglist = NULL;
    g_spool_job_listview = NULL;
    g_spool_delete_gadget = NULL;
    g_spool_retry_gadget = NULL;
    g_spool_copies_gadget = NULL;

    FreeVisualInfo(g_spool_vi);
    g_spool_vi = NULL;
    UnlockPubScreen(NULL, g_spool_screen);
    g_spool_screen = NULL;
}

/* Opens the Spooler window, or - if one is already open - just brings the
 * existing one to the front instead of opening a second instance sharing
 * the same g_spool_*() globals underneath it. Non-modal: unlike the
 * discovery-selection and Copies dialogs, this does not run its own
 * IDCMP loop. It returns immediately after opening, and
 * process_window_events()'s own main-window loop below folds this
 * window's UserPort signal bit into the same Wait() call as the main
 * window's, calling mp_spool_win_process() whenever it fires - the same
 * technique already used there for test_print_job's async completion.
 * That is what lets both windows accept input at the same time instead
 * of one blocking the other the way a nested modal loop would. */
static void mp_spool_win_open(struct Window *parent)
{
    struct Gadget *sglist = NULL;
    struct Gadget *gad;
    struct NewGadget ng;
    UWORD topborder;
    BOOL have_selection;

    (void)parent;

    if (g_spool_win) {
        WindowToFront(g_spool_win);
        ActivateWindow(g_spool_win);
        return;
    }

    g_spool_screen = LockPubScreen(NULL);
    if (!g_spool_screen) return;

    g_spool_vi = GetVisualInfo(g_spool_screen, TAG_DONE);
    if (!g_spool_vi) {
        UnlockPubScreen(NULL, g_spool_screen);
        g_spool_screen = NULL;
        return;
    }

    topborder = g_spool_screen->WBorTop +
                (g_spool_screen->Font->ta_YSize + 1);

    /* Centred the first time this ever opens; every later open reuses
     * wherever it was last closed (drags included) instead of recentring. */
    if (g_spool_win_left < 0) {
        g_spool_win_left = (g_spool_screen->Width > 620)
                                ? (g_spool_screen->Width - 620) / 2 : 0;
        g_spool_win_top = (g_spool_screen->Height > 220 + topborder)
                               ? (g_spool_screen->Height - 220) / 2
                               : topborder;
    }

    /* Which saved Unit a Retry/Copies resubmission targets - defaults to
     * whatever the main window currently has active the first time this
     * opens, but the Unit cycle gadget below lets it be reassigned to any
     * other saved printer profile, and that choice (like the window's
     * position) is remembered across closing and reopening this window. */
    if (g_spool_retry_unit < 0) {
        g_spool_retry_unit = current_unit_index;
        if (g_spool_retry_unit < 0) g_spool_retry_unit = 0;
        if (g_spool_retry_unit >= MAX_UNITS)
            g_spool_retry_unit = MAX_UNITS - 1;
    }

    g_spool_count = mp_scan_spool_jobs(mp_spool_jobs, MP_SPOOL_LIST_MAX,
                                       &g_spool_job_list);
    g_spool_selected = 0;
    g_spool_selection_made = FALSE;
    have_selection = g_spool_count > 0;

    gad = CreateContext(&sglist);
    if (!gad) {
        FreeVisualInfo(g_spool_vi); g_spool_vi = NULL;
        UnlockPubScreen(NULL, g_spool_screen); g_spool_screen = NULL;
        return;
    }

    /* A real scrolling multi-row list (GadTools LISTVIEW_KIND), not
     * the single-entry-at-a-time CYCLE_KIND browsing gadget the
     * discovery dialog uses - every tracked job and its status is
     * visible at once, the way MintAMP/MintVID-style list windows
     * show theirs. */
    ng.ng_TextAttr = &Topaz80;
    ng.ng_VisualInfo = g_spool_vi;
    ng.ng_Flags = 0;
    ng.ng_LeftEdge = 10;
    ng.ng_TopEdge = 10 + topborder;
    ng.ng_Width = 600;
    ng.ng_Height = 150;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID = GAD_SPOOL_JOB_LIST;
    /* No GTLV_ReadOnly here: that attribute, despite its name, does
     * not mean "not editable" (a plain text listview like this one
     * was never editable in the first place) - it means "the user
     * cannot select an entry at all", which is exactly why rows
     * couldn't be clicked. Selectable-but-not-editable is simply the
     * GadTools default with no tag needed. */
    /* GTLV_Selected starts at ~0 (no row pre-highlighted), the same
     * sentinel MintAMP's working radio-results listview uses - not 0,
     * which pre-selects row 0 and can confuse the "did my click do
     * anything" read on the very first paint. GTLV_ShowSelected is
     * included too, matching that same gadget, even though this list
     * has no companion display gadget to copy the selection into. */
    gad = CreateGadget(LISTVIEW_KIND, gad, &ng,
        GTLV_Labels, (ULONG)&g_spool_job_list,
        GTLV_Selected, (ULONG)~0,
        GTLV_ShowSelected, (ULONG)NULL,
        GA_Disabled, (ULONG)(have_selection ? FALSE : TRUE),
        TAG_DONE);
    if (!gad) {
        FreeGadgets(sglist);
        FreeVisualInfo(g_spool_vi); g_spool_vi = NULL;
        UnlockPubScreen(NULL, g_spool_screen); g_spool_screen = NULL;
        return;
    }
    g_spool_job_listview = gad;

    ng.ng_TopEdge += 162;
    ng.ng_LeftEdge = 10;
    ng.ng_Width = 100;
    ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"_Refresh";
    ng.ng_GadgetID = GAD_SPOOL_REFRESH;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, GT_Underscore, '_', TAG_DONE);
    if (!gad) goto fail;

    ng.ng_LeftEdge = 120;
    ng.ng_GadgetText = (STRPTR)"_Delete";
    ng.ng_GadgetID = GAD_SPOOL_DELETE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_', GA_Disabled, (ULONG)(have_selection ? FALSE : TRUE),
        TAG_DONE);
    if (!gad) goto fail;
    g_spool_delete_gadget = gad;

    ng.ng_LeftEdge = 230;
    ng.ng_GadgetText = (STRPTR)"_Retry";
    ng.ng_GadgetID = GAD_SPOOL_RETRY;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_', GA_Disabled, (ULONG)(have_selection ? FALSE : TRUE),
        TAG_DONE);
    if (!gad) goto fail;
    g_spool_retry_gadget = gad;

    ng.ng_LeftEdge = 340;
    ng.ng_Width = 110;
    ng.ng_GadgetText = (STRPTR)"_Copies...";
    ng.ng_GadgetID = GAD_SPOOL_COPIES;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_', GA_Disabled, (ULONG)(have_selection ? FALSE : TRUE),
        TAG_DONE);
    if (!gad) goto fail;
    g_spool_copies_gadget = gad;

    ng.ng_LeftEdge = 460;
    ng.ng_Width = 90;
    ng.ng_GadgetText = (STRPTR)"_Close";
    ng.ng_GadgetID = GAD_SPOOL_CLOSE;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, GT_Underscore, '_', TAG_DONE);
    if (!gad) goto fail;

    /* Which printer a Retry/Copies resubmission goes to - defaults to
     * the main window's active Unit, reassignable to any other saved
     * Unit profile right here without having to close this window,
     * switch it in the main window, and reopen. Purely a resubmission
     * target: it does not change which Unit a job's "assigned to"
     * host:port column shows - that always reflects where the job
     * actually went (or will go, once retried against this choice). */
    /* PLACETEXT_LEFT draws the label to the left of ng_LeftEdge, not
     * inside it - the same off-window trap the "Keep Jobs (HDD)"
     * checkbox above hit at LeftEdge=10 (see its own comment). Leaving
     * room here by starting the box itself at LeftEdge=90 instead. */
    ng.ng_TopEdge += 22;
    ng.ng_LeftEdge = 90;
    ng.ng_Width = 520;
    ng.ng_GadgetText = (STRPTR)"Retry to:";
    ng.ng_GadgetID = GAD_SPOOL_UNIT;
    ng.ng_Flags = PLACETEXT_LEFT;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)unit_dropdown_labels,
        GTCY_Active, (ULONG)g_spool_retry_unit,
        TAG_DONE);
    if (!gad) goto fail;

    /* Gadgets are attached after OpenWindowTags via AddGList/RefreshGList,
     * not the WA_Gadgets tag - the same sequence MintAMP's working
     * listview windows use. WA_Gadgets attaches the list at open time
     * too in principle, but this is the pattern proven to leave a
     * GadTools LISTVIEW_KIND actually clickable on real hardware. */
    g_spool_win = OpenWindowTags(NULL,
        WA_Title, (ULONG)"Spooler Management",
        WA_Left, (ULONG)g_spool_win_left,
        WA_Top, (ULONG)g_spool_win_top,
        WA_Width, 620,
        WA_InnerHeight, 220,
        WA_DragBar, TRUE,
        WA_DepthGadget, TRUE,
        WA_Activate, TRUE,
        WA_CloseGadget, TRUE,
        WA_SimpleRefresh, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | LISTVIEWIDCMP,
        WA_PubScreen, (ULONG)g_spool_screen,
        TAG_DONE);
    if (!g_spool_win) goto fail;

    g_spool_sglist = sglist;
    AddGList(g_spool_win, sglist, (UWORD)-1, -1, NULL);
    RefreshGList(sglist, g_spool_win, NULL, -1);
    GT_RefreshWindow(g_spool_win, NULL);
    return;

fail:
    FreeGadgets(sglist);
    FreeVisualInfo(g_spool_vi); g_spool_vi = NULL;
    UnlockPubScreen(NULL, g_spool_screen); g_spool_screen = NULL;
    g_spool_job_listview = NULL;
    g_spool_delete_gadget = NULL;
    g_spool_retry_gadget = NULL;
    g_spool_copies_gadget = NULL;
}

/* Drains and dispatches every IntuiMessage currently queued on the
 * Spooler window's port - called from process_window_events()'s main
 * loop whenever that window's signal bit comes back from Wait(), the
 * same way test_print_job's completion port is handled there. swin is
 * captured up front and used for the whole drain even if a Close request
 * is seen partway through: every message GadTools hands back must be
 * GT_ReplyIMsg()'d before mp_spool_win_close() calls CloseWindow(),
 * including ones still queued from a rapid double-click on Retry/
 * Copies/Delete that arrived while the first click's (possibly blocking,
 * network-bound) handler was still running - so this keeps draining and
 * replying right up to the end, it just stops acting on anything once a
 * close was requested, which is what stops a queued second Retry click
 * from firing a second resubmission behind the first one's back. */
static void mp_spool_win_process(void)
{
    struct Window *swin = g_spool_win;
    struct IntuiMessage *imsg;
    BOOL want_close = FALSE;

    if (!swin) return;

    imsg = GT_GetIMsg(swin->UserPort);
    while (imsg) {
        struct Gadget *g = (struct Gadget *)imsg->IAddress;
        ULONG cls = imsg->Class;
        UWORD code = imsg->Code;
        GT_ReplyIMsg(imsg);

        if (want_close) {
            imsg = GT_GetIMsg(swin->UserPort);
            continue;
        }

        if (cls == IDCMP_CLOSEWINDOW) {
            want_close = TRUE;
        } else if (cls == IDCMP_REFRESHWINDOW) {
            GT_BeginRefresh(swin);
            GT_EndRefresh(swin, TRUE);
        } else if (cls == IDCMP_GADGETUP) {
            if (g->GadgetID == GAD_SPOOL_JOB_LIST) {
                if ((ULONG)code < (ULONG)g_spool_count) {
                    g_spool_selected = (ULONG)code;
                    g_spool_selection_made = TRUE;
                    /* GadTools does not persist which row a click
                     * selected on its own - a later IDCMP_REFRESHWINDOW
                     * (there are more of those than you'd expect: window
                     * activation, another window uncovering this one,
                     * ...) repaints the listview from its own stored
                     * GTLV_Selected, which otherwise still says "none",
                     * making the highlight vanish right after the click.
                     * Mirroring the selection back here is what makes it
                     * stick. */
                    GT_SetGadgetAttrs(g_spool_job_listview, swin, NULL,
                                      GTLV_Selected, g_spool_selected,
                                      TAG_DONE);
                    mp_spool_update_action_buttons(swin);
                    /* The list truncates each row to fit the window - a
                     * failure reason like "Printer rejected request
                     * (wrong IPP path?)" does not fit in the column
                     * budget mp_scan_spool_jobs() has to work with on any
                     * Amiga screen this driver targets. The window title
                     * has the room a list column doesn't, so show the
                     * selected job's full state/reason there instead.
                     * static, not a stack local: SetWindowTitles() keeps
                     * only a pointer, not a copy, and this has to stay
                     * valid for however long this title stays on screen -
                     * across GadTools refresh events this function isn't
                     * even running for - not just this one call. */
                    {
                        static char spool_title[192];
                        struct MPSpoolJobEntry *job =
                            &mp_spool_jobs[g_spool_selected];
                        if (job->reason[0])
                            snprintf(spool_title, sizeof(spool_title),
                                    "Spooler Management - %s: %s",
                                    job->state, job->reason);
                        else
                            snprintf(spool_title, sizeof(spool_title),
                                    "Spooler Management - %s", job->state);
                        SetWindowTitles(swin, (STRPTR)spool_title,
                                        (STRPTR)~0);
                    }
                }
            } else if (g->GadgetID == GAD_SPOOL_UNIT) {
                g_spool_retry_unit = (int)code;
            } else if (g->GadgetID == GAD_SPOOL_CLOSE) {
                want_close = TRUE;
            } else if (g->GadgetID == GAD_SPOOL_REFRESH) {
                /* Swaps the listview's contents in place - the window
                 * stays open, same as MintAMP's search results Refresh -
                 * see mp_spool_refresh_list_live()'s own comment. */
                mp_spool_refresh_list_live(swin, g_spool_job_listview,
                    g_spool_delete_gadget, g_spool_retry_gadget,
                    g_spool_copies_gadget, &g_spool_job_list,
                    &g_spool_count, &g_spool_selected,
                    &g_spool_selection_made);
            } else if (g->GadgetID == GAD_SPOOL_DELETE) {
                if (g_spool_selection_made &&
                    g_spool_selected < (ULONG)g_spool_count &&
                    mp_spool_job_actionable(
                        &mp_spool_jobs[g_spool_selected])) {
                    DeleteFile((CONST_STRPTR)
                        mp_spool_jobs[g_spool_selected].job_path);
                    DeleteFile((CONST_STRPTR)
                        mp_spool_jobs[g_spool_selected].status_path);
                    mp_spool_refresh_list_live(swin, g_spool_job_listview,
                        g_spool_delete_gadget, g_spool_retry_gadget,
                        g_spool_copies_gadget, &g_spool_job_list,
                        &g_spool_count, &g_spool_selected,
                        &g_spool_selection_made);
                } else {
                    SetWindowTitles(swin,
                        (STRPTR)"Spooler Management - select a job first",
                        (STRPTR)~0);
                }
            } else if (g->GadgetID == GAD_SPOOL_RETRY) {
                if (g_spool_selection_made &&
                    g_spool_selected < (ULONG)g_spool_count &&
                    mp_spool_job_actionable(
                        &mp_spool_jobs[g_spool_selected])) {
                    /* Blocking (network I/O) - said so up front, and the
                     * buttons are disabled for the same reason a second
                     * click mid-retry queued up behind this one is
                     * drained above without triggering a second
                     * resubmission. The main window stays responsive
                     * throughout, since this whole dispatch only runs
                     * when the Spooler window's own signal bit fires -
                     * but this call itself still blocks until the
                     * network round-trip finishes. */
                    SetWindowTitles(swin,
                        (STRPTR)"Spooler Management - Retrying...",
                        (STRPTR)~0);
                    GT_SetGadgetAttrs(g_spool_retry_gadget, swin, NULL,
                                      GA_Disabled, TRUE, TAG_DONE);
                    GT_SetGadgetAttrs(g_spool_copies_gadget, swin, NULL,
                                      GA_Disabled, TRUE, TAG_DONE);
                    mp_spool_retry_job(&mp_spool_jobs[g_spool_selected],
                                       g_spool_retry_unit);
                    mp_spool_refresh_list_live(swin, g_spool_job_listview,
                        g_spool_delete_gadget, g_spool_retry_gadget,
                        g_spool_copies_gadget, &g_spool_job_list,
                        &g_spool_count, &g_spool_selected,
                        &g_spool_selection_made);
                } else {
                    SetWindowTitles(swin,
                        (STRPTR)"Spooler Management - select a job first",
                        (STRPTR)~0);
                }
            } else if (g->GadgetID == GAD_SPOOL_COPIES) {
                int copies = 1;
                if (g_spool_selection_made &&
                    g_spool_selected < (ULONG)g_spool_count &&
                    mp_spool_job_actionable(
                        &mp_spool_jobs[g_spool_selected]) &&
                    run_copies_dialog(swin, &copies)) {
                    int i;
                    GT_SetGadgetAttrs(g_spool_retry_gadget, swin, NULL,
                                      GA_Disabled, TRUE, TAG_DONE);
                    GT_SetGadgetAttrs(g_spool_copies_gadget, swin, NULL,
                                      GA_Disabled, TRUE, TAG_DONE);
                    for (i = 0; i < copies; ++i) {
                        /* static: see the row-selection title's own
                         * comment above on why a stack local isn't safe
                         * for a SetWindowTitles() string. */
                        static char title[64];
                        snprintf(title, sizeof(title),
                                "Spooler Management - Retrying (%d/%d)...",
                                i + 1, copies);
                        SetWindowTitles(swin, (STRPTR)title, (STRPTR)~0);
                        mp_spool_retry_job(
                            &mp_spool_jobs[g_spool_selected],
                            g_spool_retry_unit);
                    }
                    mp_spool_refresh_list_live(swin, g_spool_job_listview,
                        g_spool_delete_gadget, g_spool_retry_gadget,
                        g_spool_copies_gadget, &g_spool_job_list,
                        &g_spool_count, &g_spool_selected,
                        &g_spool_selection_made);
                } else if (!g_spool_selection_made) {
                    SetWindowTitles(swin,
                        (STRPTR)"Spooler Management - select a job first",
                        (STRPTR)~0);
                }
            }
        }
        imsg = GT_GetIMsg(swin->UserPort);
    }

    if (want_close)
        mp_spool_win_close();
}

/* Bounded, GUI-responsive connect(): socket -> IoctlSocket(FIONBIO on) ->
 * connect() -> if EINPROGRESS, WaitSelect() on the write+exception sets in
 * short chunks (pumping GUI events between them) until connected, refused,
 * or timeout_secs is up -> getsockopt(SO_ERROR) to find out which ->
 * IoctlSocket(FIONBIO off).
 *
 * connect() alone is a plain blocking call with no timeout - SO_RCVTIMEO
 * only ever covered recv() - so an unreachable host that doesn't actively
 * refuse the connection (dropped SYNs, asleep, etc.) could block far
 * longer than any UI expects, with nothing pumping GUI events while
 * stuck. This uses the same WaitSelect()-with-a-bounded-timeval shape
 * already proven working in this file for SSDP/mDNS discovery - a
 * previous attempt at non-blocking sockets elsewhere in this file
 * referenced a constant called "FNONBIO", which is not a real BSD ioctl
 * name (the real one, used here, is FIONBIO) and is the more likely
 * explanation for that attempt not working, rather than non-blocking
 * mode being unavailable on this NDK/stack.
 *
 * Returns 0 on success, -1 on failure (errno set: from SO_ERROR when the
 * stack reported one, or ETIMEDOUT for this function's own timeout). The
 * socket is always left blocking again before returning, whatever the
 * outcome, so callers don't need to know this happened. */
static int mp_connect_with_timeout(int sockfd, struct sockaddr_in *addr, int timeout_secs) {
    long nonblock = 1;
    long block = 0;
    int rc;
    int connect_errno;

    if (IoctlSocket(sockfd, FIONBIO, (char *)&nonblock) < 0) {
        /* Non-blocking mode unavailable on this stack for some reason -
         * fall back to a plain blocking connect (still covered by the
         * SO_SNDTIMEO best-effort set by the caller) rather than failing
         * outright. */
        printf("connect: IoctlSocket(FIONBIO on) failed, falling back to blocking\n");
        return connect(sockfd, (struct sockaddr *)addr, sizeof(*addr));
    }

    rc = connect(sockfd, (struct sockaddr *)addr, sizeof(*addr));
    connect_errno = (rc < 0) ? Errno() : 0;
    printf("connect: immediate rc=%d errno=%d Errno()=%d\n", rc, errno, connect_errno);

    /* AmigaOS bsdsocket.library does NOT update the standard C errno
     * global - confirmed for real: a prior build of this exact function
     * logged "rc=-1 errno=0" on every attempt, which is what happens when
     * nothing ever sets errno at all, not what any real connect() failure
     * looks like. bsdsocket.library tracks its own error state instead,
     * read back with its own Errno() function (see proto/bsdsocket.h) -
     * that's what's checked below, not errno.
     *
     * Which value Errno() returns for "in progress, not decided yet" on a
     * non-blocking connect also isn't standardised across
     * bsdsocket.library stacks, so both EINPROGRESS and EWOULDBLOCK are
     * accepted rather than gambling on just one. */
    if (rc < 0 && (connect_errno == EINPROGRESS || connect_errno == EWOULDBLOCK)) {
        int elapsed_ms = 0;
        const int chunk_ms = 250;
        int outcome = -2; /* -2 = still waiting, -1 = failed, 0 = connected */

        while (outcome == -2 && elapsed_ms < timeout_secs * 1000) {
            fd_set wfds, efds;
            struct timeval tv;
            long ready;

            if (window) {
                struct IntuiMessage *imsg;
                while ((imsg = GT_GetIMsg(window->UserPort))) {
                    GT_ReplyIMsg(imsg);
                }
            }

            FD_ZERO(&wfds);
            FD_SET(sockfd, &wfds);
            FD_ZERO(&efds);
            FD_SET(sockfd, &efds);
            tv.tv_sec = 0;
            tv.tv_usec = chunk_ms * 1000;

            ready = WaitSelect(sockfd + 1, NULL, &wfds, &efds, &tv, NULL);
            if (ready > 0 && (FD_ISSET(sockfd, &wfds) || FD_ISSET(sockfd, &efds))) {
                int so_err = 0;
                socklen_t optlen = sizeof(so_err);
                int gso_rc = getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char *)&so_err, &optlen);
                printf("connect: WaitSelect ready, getsockopt rc=%d so_err=%d Errno()=%d\n",
                       gso_rc, so_err, (gso_rc < 0) ? Errno() : 0);
                if (gso_rc == 0) {
                    if (so_err == 0) {
                        outcome = 0;
                    } else {
                        errno = so_err;
                        outcome = -1;
                    }
                } else {
                    /* getsockopt(SO_ERROR) itself failing is a real
                     * possibility on an older/limited bsdsocket.library -
                     * if so, write-readiness alone is the best signal
                     * available that the connection attempt resolved,
                     * so trust it as success rather than treating an
                     * unsupported diagnostic call as a connection
                     * failure. */
                    printf("connect: getsockopt(SO_ERROR) unsupported, trusting write-ready as success\n");
                    outcome = 0;
                }
            }
            elapsed_ms += chunk_ms;
        }

        if (outcome == -2) {
            printf("connect: timed out after %dms waiting for write-ready\n", elapsed_ms);
            errno = ETIMEDOUT;
            outcome = -1;
        }
        rc = outcome;
    }

    IoctlSocket(sockfd, FIONBIO, (char *)&block);
    printf("connect: mp_connect_with_timeout returning %d\n", rc);
    return rc;
}

/* ---------------------------------------------------------------------
 * Optional printer picture advertised by IPP's printer-icons attribute.
 *
 * Fetch the first HTTP URI only.  PNG decoding is handled internally by
 * the same LodePNG decoder used by MintAMP, producing RGBA pixels with real
 * alpha.  MintPRINT area-averages that image down to 32x32, composites the
 * translucent edge pixels against the GUI background, then maps the result
 * to the current screen's pens.  No picture.datatype is required.
 * This is deliberately optional: unsupported URI/download/decode failure
 * simply leaves the preview blank.
 * ------------------------------------------------------------------ */
static void mp_clear_printer_icon(void) {
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

static BOOL mp_load_printer_icon_rgba(void) {
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

static void mp_refresh_printer_icon(void) {
    mp_clear_printer_icon();

    if (!printer_icon_uri[0]) {
        /* A successful Query saying there is no printer-icons attribute
         * makes any older artwork for this Unit stale. */
        mp_delete_printer_icon_cache();
        return;
    }

    /* This only ever runs from an explicit Query press (the one call
     * site is the Query success handler), which already does a much
     * heavier IPP round-trip - so always attempt a genuinely fresh
     * fetch+decode here rather than trusting a same-URI cache hit
     * forever. That used to mean a single bad decode (a truncated
     * transfer, or a genuinely malformed source image the printer
     * briefly served) got written once and then displayed on every
     * later Query indefinitely, since the printer's icon URI itself
     * doesn't change - reported as "the corrupted image keeps
     * displaying, guess it's cached". Query is now the icon's one
     * chance to self-heal; the cache is only a fallback for when the
     * live attempt itself fails (printer briefly unreachable etc.), not
     * an assumption that a URI match means the art is still good. */
    if (mp_fetch_printer_icon_file(printer_icon_uri) &&
        mp_load_printer_icon_rgba()) {
        mp_save_printer_icon_cache();
    } else {
        mp_load_printer_icon_cache(TRUE);
    }

    DeleteFile((CONST_STRPTR)MP_PRINTER_ICON_TEMP);
}

static UBYTE mp_printer_icon_nearest_pen(const ULONG *palette,
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
    UBYTE bg_r, bg_g, bg_b;

    if (!window || !screen)
        return;

    rp = window->RPort;
    cm = screen->ViewPort.ColorMap;
    if (!cm)
        return;

    screen_pen_count = mp_screen_pen_count(screen, cm);
    mp_fill_screen_palette32(cm, screen_pen_count, screen_palette);
    SetDrMd(rp, JAM1);
    SetAPen(rp, 0);
    RectFill(rp, left - 1, top - 1,
             left + MP_PRINTER_ICON_SIZE, top + MP_PRINTER_ICON_SIZE);

    if (!mp_printer_icon_valid)
        return;

    /* Partial alpha is composited against pen 0, which is exactly the
     * background we just cleared the icon box with - needed both to
     * populate the cached base pens below and, every redraw, to decide
     * per pixel whether the actual on-screen colour a viewer would see
     * is itself near-neutral. */
    bg_r = (UBYTE)((screen_palette[0] >> 24) & 0xffUL);
    bg_g = (UBYTE)((screen_palette[1] >> 24) & 0xffUL);
    bg_b = (UBYTE)((screen_palette[2] >> 24) & 0xffUL);

    /* Convert RGBA to the current screen's pens once per downloaded icon,
     * not on every refresh. */
    if (!mp_printer_icon_pens_valid) {
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
        const UBYTE *p;
        ULONG a;
        UBYTE r, g, b, smax, smin;

        if (!mp_printer_icon_mask[i])
            continue;
        pen = mp_printer_icon_pens[i];

        /* Per pixel, not per screen: a screen's own advertised colour
         * count/depth turned out not to be a reliable signal for whether
         * it actually offers a smooth grey ramp (see
         * mp_screen_pen_count()'s comment on why Count itself can be
         * unreliable) - so this only asks whether the actual on-screen
         * colour this pixel composites to (background-blended, same as
         * the cached base pen above - not the raw un-composited RGBA)
         * looks like it was meant to be grey, and if so, prefers a
         * screen pen that is too, on any screen depth. A pixel that
         * composites to a real colour still gets the plain nearest-colour
         * cached pen unchanged. */
        p = mp_printer_icon_rgba + i * 4;
        a = p[3];
        r = (UBYTE)(((ULONG)p[0] * a + (ULONG)bg_r * (255UL - a) + 127UL) / 255UL);
        g = (UBYTE)(((ULONG)p[1] * a + (ULONG)bg_g * (255UL - a) + 127UL) / 255UL);
        b = (UBYTE)(((ULONG)p[2] * a + (ULONG)bg_b * (255UL - a) + 127UL) / 255UL);
        smax = (r > g) ? r : g;
        smin = (r < g) ? r : g;
        if (b > smax) smax = b;
        if (b < smin) smin = b;
        if ((int)smax - (int)smin <= MP_NEUTRAL_SAT_THRESHOLD)
            pen = mp_low_colour_gray_pen(screen_palette, screen_pen_count,
                                         r, g, b);

        if ((LONG)pen != last_pen) {
            SetAPen(rp, pen);
            last_pen = (LONG)pen;
        }
        x = i % MP_PRINTER_ICON_SIZE;
        y = i / MP_PRINTER_ICON_SIZE;
        WritePixel(rp, left + x, top + y);
    }
}

// Updated query_printer_attributes with fixed mapping logic and tray name parsing
int query_printer_attributes(const char *ip, int port, char *response, int maxlen) {
    custom_printf("CLEAR");
    if (operation_in_progress) {
        printf("Operation already in progress, please wait...\n");
        return -1;
    }
    operation_in_progress = TRUE;

    // Reset all supported values
    num_supported_formats = 0;
    num_supported_media = 0;
    num_supported_output_modes = 0;
    num_supported_sides = 0;
    num_supported_scaling = 0;
    num_supported_orientations = 0;
    num_supported_media_sources = 0;
    num_supported_print_modes = 0;
    num_supported_quality = 0;
    num_supported_dpi = 0;
    num_media_tray_mappings = 0;
    has_media_ready = FALSE;
    jpeg_constraints_queried = TRUE;
    jpeg_k_octets_reported = FALSE;
    jpeg_x_dimension_reported = FALSE;
    jpeg_y_dimension_reported = FALSE;
    supports_create_job = FALSE;
    supports_send_document = FALSE;
    supports_multiple_document_jobs = FALSE;
    supports_single_document_handling = FALSE;
    strcpy(pwg_sheet_back_value, "normal");
    printer_make_model[0] = '\0';
    printer_icon_uri[0] = '\0';
    num_marker_names = 0;
    num_marker_colors = 0;
    num_marker_types = 0;
    num_marker_levels = 0;
    num_marker_low_levels = 0;
    num_marker_high_levels = 0;
    printer_state_value = 0;
    num_printer_state_reasons = 0;

    // Allocate buffers for parsing
    char *name = malloc(512);
    char *value = malloc(512);
    if (!name || !value) {
        printf("Memory allocation failed\n");
        if (name) free(name);
        if (value) free(value);
        operation_in_progress = FALSE;
        return -1;
    }

    // Build IPP payload for Get-Printer-Attributes request
    unsigned char *ipp_payload = malloc(2048);// Dynamically allocate
    if (!ipp_payload) {
        printf("Failed to allocate memory for IPP payload\n");
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }
    int offset = 0;

    /* 631 is the default/implied port for the ipp:// scheme and is safe to
     * omit; any other port (e.g. 80) must be stated explicitly or the URI
     * silently claims a printer on 631 while we actually connect elsewhere.
     * Use the configured IPP path (driver_path_buffer) rather than a
     * hardcoded one, so this matches whatever the user's printer actually
     * needs (e.g. Tallguy58's Canon needs /ipp/print, not /ipp). */
    char uri[128];
    if (port == 631) {
        snprintf(uri, sizeof(uri), "ipp://%s%s", ip, driver_path_buffer);
    } else {
        snprintf(uri, sizeof(uri), "ipp://%s:%d%s", ip, port, driver_path_buffer);
    }
    int uri_len = strlen(uri);

    ipp_payload[offset++] = 0x01; ipp_payload[offset++] = 0x01; // IPP 1.1
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0B; // Get-Printer-Attributes
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x01; // Request ID

    ipp_payload[offset++] = 0x01; // Operation attributes group
    ipp_payload[offset++] = 0x47; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x12;
    memcpy(&ipp_payload[offset], "attributes-charset", 18); offset += 18;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x05;
    memcpy(&ipp_payload[offset], "utf-8", 5); offset += 5;

    ipp_payload[offset++] = 0x48; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x1b;
    memcpy(&ipp_payload[offset], "attributes-natural-language", 27); offset += 27;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x02;
    memcpy(&ipp_payload[offset], "en", 2); offset += 2;

    ipp_payload[offset++] = 0x45; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0b;
    memcpy(&ipp_payload[offset], "printer-uri", 11); offset += 11;
    ipp_payload[offset++] = (uri_len >> 8) & 0xFF;
    ipp_payload[offset++] = uri_len & 0xFF;
    memcpy(&ipp_payload[offset], uri, uri_len); offset += uri_len;

    /* requested-attributes is 1setOf keyword (RFC 8011 3.1.7): each
     * attribute name is its own value, not one comma-joined string - a
     * single keyword value can't hold multiple keywords. The first value
     * carries the attribute's own name; every value after it is an
     * "additional value" and repeats with name-length 0.
     * (This also used to send the name itself truncated to 18 bytes -
     * "requested-attribut" - instead of the full 20-byte
     * "requested-attributes", which alone was enough to make a strict
     * printer not recognise the filter at all.) */
    {
        static const char *mp_requested_attrs[] = {
            "media-source-supported", "media-ready", "printer-input-tray",
            "printer-state", "printer-state-reasons", "print-color-mode-supported",
            "print-scaling-supported", "print-quality-supported",
            "printer-resolution-default", "printer-resolution-supported",
            "pwg-raster-document-resolution-supported",
            "pwg-raster-document-sheet-back",
            "document-format-supported", "printer-make-and-model",
            "sides-supported", "operations-supported",
            "multiple-document-jobs-supported",
            "multiple-document-handling-supported",
            "jpeg-k-octets-supported", "jpeg-x-dimension-supported",
            "jpeg-y-dimension-supported",
            "printer-icons",
            "marker-names", "marker-colors", "marker-types",
            "marker-levels", "marker-low-levels", "marker-high-levels",
            NULL
        };
        int i;
        for (i = 0; mp_requested_attrs[i]; i++) {
            int attr_len = strlen(mp_requested_attrs[i]);
            ipp_payload[offset++] = 0x44; // keyword
            if (i == 0) {
                ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x14;
                memcpy(&ipp_payload[offset], "requested-attributes", 20); offset += 20;
            } else {
                ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x00; // additional value
            }
            ipp_payload[offset++] = (attr_len >> 8) & 0xFF;
            ipp_payload[offset++] = attr_len & 0xFF;
            memcpy(&ipp_payload[offset], mp_requested_attrs[i], attr_len); offset += attr_len;
        }
    }
    /* No print-scaling (or any other Job Template attribute) belongs here -
     * this is a capability query, not a job. A stray "print-scaling"
     * keyword used to get appended to this request's operation-attributes
     * group; at least one real printer (Canon TS8300) responded to that
     * with an empty printer-attributes group instead of an error, which
     * looked like "query succeeded, printer reported nothing supported". */
    ipp_payload[offset++] = 0x03; // End of attributes

    // Build HTTP header
    char *http_header = malloc(256); // Dynamically allocate
    if (!http_header) {
        printf("Failed to allocate memory for HTTP header\n");
        free(ipp_payload);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }
    snprintf(http_header, 256,
             "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/ipp\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
             driver_path_buffer, ip, port, offset);

    // Open socket
    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        snprintf(response, maxlen, "Socket creation failed");
        free(http_header);
        free(ipp_payload);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }
    
    // Set a very short timeout to minimize blocking
    struct timeval timeout = {5, 0}; // 5-second timeout
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)) < 0) {
        printf("Failed to set socket timeout\n");
        CloseSocket(sockfd);
        free(http_header);
        free(ipp_payload);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }
    /* connect() itself is bounded by mp_connect_with_timeout() below, not
     * by this - SO_SNDTIMEO is only formally specified for send(), and is
     * set here purely as a secondary safety net for the rare case where
     * that helper's non-blocking-mode setup fails and it falls back to a
     * plain blocking connect(). */
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = inet_addr((STRPTR)ip);
    if (serv_addr.sin_addr.s_addr == INADDR_NONE) {
        snprintf(response, maxlen, "Invalid IP address: %s", ip);
        CloseSocket(sockfd);
        free(http_header);
        free(ipp_payload);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }

    printf("Connecting to printer...\n");
    /* Some AirPrint-capable inkjets (HP OfficeJet/Envy series confirmed -
     * see issue #30) let their Wi-Fi radio drop into a power-save state
     * between jobs. UDP discovery (SSDP/mDNS) still gets a reply because
     * the radio wakes for broadcast/multicast traffic, but the printer's
     * first real TCP SYN after that can take noticeably longer than 5
     * seconds to answer while the radio comes back up - a wired printer,
     * or the same printer once its radio is already awake, answers almost
     * immediately. 8 seconds gives that wake-up room without making a
     * genuinely dead endpoint (refused/unreachable) noticeably slower to
     * give up on, since those fail via ECONNREFUSED/host-unreachable long
     * before the timeout regardless of its length. */
    if (mp_connect_with_timeout(sockfd, &serv_addr, 8) < 0) {
        snprintf(response, maxlen, "Failed to connect to printer");
        CloseSocket(sockfd);
        free(http_header);
        free(ipp_payload);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        /* Distinct from -1 (data/parsing failures worth retrying, e.g. a
         * genuinely transient truncated response) so perform_query_flow()
         * can tell "this endpoint doesn't answer at all" apart from "that
         * attempt was flaky" and skip straight to the next port instead
         * of repeating a now-deterministically-bounded connect failure. */
        return -2;
    }

    // Process GUI events
    if (window) {
        struct IntuiMessage *imsg;
        while ((imsg = GT_GetIMsg(window->UserPort))) {
            GT_ReplyIMsg(imsg);
        }
    }

    // Send request
    printf("Sending request...\n");
    if (send(sockfd, http_header, strlen(http_header), 0) < 0 ||
        send(sockfd, (char *)ipp_payload, offset, 0) < 0) {
        snprintf(response, maxlen, "Failed to send request");
        CloseSocket(sockfd);
        free(http_header);
        free(ipp_payload);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }

    free(http_header);
    free(ipp_payload);

    // Process GUI events
    if (window) {
        struct IntuiMessage *imsg;
        while ((imsg = GT_GetIMsg(window->UserPort))) {
            GT_ReplyIMsg(imsg);
        }
    }

    // Receive response in bounded, GUI-responsive chunks. HTTP permits three
    // body framing modes here: Content-Length, Transfer-Encoding: chunked, or
    // connection close. Canon firmware has now exercised all the awkward
    // parts at once: an interim 100 response followed by a final response
    // whose header spelling/framing cannot be assumed.
    printf("Waiting for response...\n");
    int total_received = 0;
    int max_idle_attempts = 40; // 40 idle waits at 100ms = 4 seconds
    int idle_attempts = 0;
    int header_start = 0; // advanced past any interim "1xx" response below
    int body_off = -1;
    int content_len = -1; // -1 = not yet known
    int chunked = FALSE;
    int connection_closed = FALSE;
    int final_http_status = -1; // set once the final (non-1xx) header is seen

    while (total_received < maxlen - 1 && idle_attempts < max_idle_attempts) {
        fd_set rfds;
        struct timeval wait_tv;
        long ready;
        ssize_t received;

        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        wait_tv.tv_sec = 0;
        wait_tv.tv_usec = 100000;
        ready = WaitSelect(sockfd + 1, &rfds, NULL, NULL, &wait_tv, NULL);
        if (ready == 0) {
            ++idle_attempts;
            goto query_receive_pump_gui;
        }
        if (ready < 0) {
            int recv_err = Errno();
            if (recv_err == EAGAIN || recv_err == EWOULDBLOCK) {
                ++idle_attempts;
                goto query_receive_pump_gui;
            }
            printf("WaitSelect receive error: %d\n", recv_err);
            snprintf(response, maxlen, "Receive wait error");
            CloseSocket(sockfd);
            free(name);
            free(value);
            operation_in_progress = FALSE;
            return -1;
        }

        received = recv(sockfd, response + total_received,
                        maxlen - 1 - total_received, 0);
        if (received > 0) {
            total_received += received;
            response[total_received] = '\0';
            idle_attempts = 0;

            while (body_off < 0) {
                int off = mp_http_find_body(response, total_received, header_start);
                int status;
                if (off < 0) break;
                status = mp_http_status(response, total_received, header_start);
                if (status >= 100 && status < 200) {
                    // Parse again immediately: the final response may already
                    // be in this same recv() buffer after the interim header.
                    printf("Skipping interim HTTP %d response\n", status);
                    header_start = off;
                    continue;
                }

                body_off = off;
                final_http_status = status;
                chunked = mp_http_header_has_token(response, header_start,
                                                   body_off,
                                                   "Transfer-Encoding",
                                                   "chunked");
                if (chunked) {
                    printf("HTTP response uses chunked transfer encoding\n");
                } else {
                    content_len = mp_http_content_length(response, header_start,
                                                         body_off);
                    if (content_len >= 0)
                        printf("Content-Length: %d\n", content_len);
                    else
                        printf("HTTP response has no length; reading until close\n");
                }
            }

            if (body_off >= 0) {
                if (chunked) {
                    int complete = mp_http_chunked_complete(response + body_off,
                                                            total_received - body_off);
                    if (complete < 0) {
                        printf("Malformed chunked HTTP response\n");
                        snprintf(response, maxlen, "Malformed chunked response");
                        CloseSocket(sockfd);
                        free(name);
                        free(value);
                        operation_in_progress = FALSE;
                        return -1;
                    }
                    if (complete > 0) break;
                } else if (content_len >= 0 &&
                           (total_received - body_off) >= content_len) {
                    break; // Got the full declared IPP payload
                }
            }
        } else if (received == 0) {
            connection_closed = TRUE;
            break;
        } else {
            int recv_err = Errno();
            if (recv_err != EAGAIN && recv_err != EWOULDBLOCK) {
                printf("Receive error: %d\n", recv_err);
                snprintf(response, maxlen, "Receive error");
                CloseSocket(sockfd);
                free(name);
                free(value);
                operation_in_progress = FALSE;
                return -1;
            }
            ++idle_attempts;
        }

query_receive_pump_gui:
        // Process GUI events to keep the mouse responsive
        if (window) {
            struct IntuiMessage *imsg;
            while ((imsg = GT_GetIMsg(window->UserPort))) {
                GT_ReplyIMsg(imsg);
            }
        }
    }

    if (total_received == 0) {
        snprintf(response, maxlen, "No response or timeout");
        CloseSocket(sockfd);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }

    response[total_received] = '\0';

    // Find the start of the IPP payload (past any interim response already
    // skipped above)
    if (body_off < 0) {
        body_off = mp_http_find_body(response, total_received, header_start);
        if (body_off >= 0 && final_http_status < 0)
            final_http_status = mp_http_status(response, total_received, header_start);
    }
    if (body_off < 0) {
        printf("Failed to find IPP payload (no \\r\\n\\r\\n separator)\n");
        CloseSocket(sockfd);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }
    char *ipp_start = response + body_off;
    int ipp_len = total_received - body_off;

    if (chunked) {
        int decoded_len = mp_http_decode_chunked(ipp_start, ipp_len);
        if (decoded_len < 0) {
            printf("Incomplete chunked IPP response\n");
            CloseSocket(sockfd);
            free(name);
            free(value);
            operation_in_progress = FALSE;
            return -1;
        }
        ipp_len = decoded_len;
        response[body_off + ipp_len] = '\0';
    } else if (content_len < 0 && !connection_closed &&
               idle_attempts >= max_idle_attempts) {
        printf("Timed out waiting for close-delimited IPP response\n");
        CloseSocket(sockfd);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }

    // Don't parse a response we know is short: if the printer told us how
    // many body bytes to expect and we didn't get that many (timed out or
    // the connection dropped mid-response), that's a failed scan, not a
    // printer that reported empty capabilities.
    if (content_len >= 0 && ipp_len < content_len) {
        printf("Incomplete IPP response: got %d of %d declared bytes\n", ipp_len, content_len);
        CloseSocket(sockfd);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }

    if (ipp_len < 8) {
        printf("IPP response too short: %d bytes\n", ipp_len);
        CloseSocket(sockfd);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }

    // Check the IPP header
    printf("IPP Version: 0x%02x%02x\n", (unsigned char)ipp_start[0], (unsigned char)ipp_start[1]);
    printf("IPP Status: 0x%02x%02x\n", (unsigned char)ipp_start[2], (unsigned char)ipp_start[3]);
    printf("Request ID: 0x%02x%02x%02x%02x\n", (unsigned char)ipp_start[4], (unsigned char)ipp_start[5], (unsigned char)ipp_start[6], (unsigned char)ipp_start[7]);

    // Reject a failed Get-Printer-Attributes response outright, before any
    // capability attributes are parsed/cached: a non-200 HTTP status or an
    // IPP status outside the 0x0000 (successful) class means the printer
    // did not actually answer the query, and whatever bytes follow must not
    // be read as capabilities.
    if (final_http_status != 200) {
        printf("Get-Printer-Attributes failed: HTTP status %d\n", final_http_status);
        CloseSocket(sockfd);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }
    {
        unsigned int ipp_status = ((unsigned char)ipp_start[2] << 8) |
                                   (unsigned char)ipp_start[3];
        if (ipp_status >= 0x0100) {
            printf("Get-Printer-Attributes failed: IPP status 0x%04x\n", ipp_status);
            CloseSocket(sockfd);
            free(name);
            free(value);
            operation_in_progress = FALSE;
            return -1;
        }
    }

    int pos = 8; // Skip header
    int attributes_processed = 0;
    int max_attributes = 1000; // Safety limit to prevent infinite loops
    char current_name[512] = ""; // Store the current attribute name for multi-value attributes

    while (pos < ipp_len && attributes_processed < max_attributes) {
        unsigned char tag = ipp_start[pos++];
        if (tag == 0x03) {
            break; // End of attributes
        }

        if (tag >= 0x01 && tag <= 0x05) { // Attribute group
            int group_start_pos = pos;
            while (pos < ipp_len && ipp_start[pos] > 0x05) {
                int attr_start_pos = pos;
                unsigned char value_tag = ipp_start[pos++];

                if (pos + 2 > ipp_len) {
                    pos = ipp_len; // Force exit
                    break;
                }
                int name_len = ((unsigned char)ipp_start[pos] << 8) | (unsigned char)ipp_start[pos + 1]; pos += 2;

                if (name_len == 0) {
                    strncpy(name, current_name, 512);
                    name[511] = '\0';
                } else {
                    if (name_len < 0 || name_len >= 512 || pos + name_len > ipp_len) {
                        pos = ipp_len; // Force exit
                        break;
                    }
                    strncpy(name, ipp_start + pos, name_len); name[name_len] = '\0'; pos += name_len;
                    strncpy(current_name, name, 512);
                    current_name[511] = '\0';
                }

                if (pos + 2 > ipp_len) {
                    pos = ipp_len; // Force exit
                    break;
                }
                int value_len = ((unsigned char)ipp_start[pos] << 8) | (unsigned char)ipp_start[pos + 1]; pos += 2;
                if (value_len < 0 || value_len >= 512 || pos + value_len > ipp_len) {
                    pos = ipp_len; // Force exit
                    break;
                }

                if (value_tag == 0x34 || value_tag == 0x37) {
                    pos += value_len;
                } else {
                    strncpy(value, ipp_start + pos, value_len); value[value_len] = '\0'; pos += value_len;

                    if (strcmp(name, "media-source-supported") == 0 && value_tag == 0x44) {
                        store_value(supported_media_sources, &num_supported_media_sources, value);
                    } else if (strcmp(name, "media-ready") == 0 && value_tag == 0x44) {
                        has_media_ready = TRUE;
                        store_value(supported_media, &num_supported_media, value);
                        int found = 0;
                        for (int i = 0; i < num_media_tray_mappings; i++) {
                            if (strcmp(media_tray_map[i].source, "auto") == 0) {
                                found = 1;
                                break;
                            }
                        }
                        if (!found && num_media_tray_mappings < MAX_VALUES) {
                            strncpy(media_tray_map[num_media_tray_mappings].media, value, MAX_ATTR_LEN - 1);
                            strncpy(media_tray_map[num_media_tray_mappings].source, "auto", MAX_ATTR_LEN - 1);
                            strncpy(media_tray_map[num_media_tray_mappings].trayName, "AUTO", MAX_ATTR_LEN - 1);
                            strncpy(media_tray_map[num_media_tray_mappings].medianame, "Unknown", MAX_ATTR_LEN - 1);
                            num_media_tray_mappings++;
                        }
                    } else if (strcmp(name, "printer-input-tray") == 0 && value_tag == 0x30) {
                        char source[MAX_ATTR_LEN] = "";
                        char trayName[MAX_ATTR_LEN] = "";
                        char medianame[MAX_ATTR_LEN] = "Unknown";
                        char media[MAX_ATTR_LEN] = "";
                        int index = -1;

                        char value_copy[512];
                        strncpy(value_copy, value, sizeof(value_copy) - 1);
                        value_copy[sizeof(value_copy) - 1] = '\0';

                        char *token = strtok(value_copy, ";");
                        while (token) {
                            if (strncmp(token, "name=", 5) == 0) {
                                strncpy(trayName, token + 5, MAX_ATTR_LEN - 1);
                            } else if (strncmp(token, "tray-name=", 10) == 0) {
                                strncpy(trayName, token + 10, MAX_ATTR_LEN - 1);
                            } else if (strncmp(token, "medianame=", 10) == 0) {
                                strncpy(medianame, token + 10, MAX_ATTR_LEN - 1);
                            } else if (strncmp(token, "media=", 6) == 0) {
                                strncpy(media, token + 6, MAX_ATTR_LEN - 1);
                            } else if (strncmp(token, "index=", 6) == 0) {
                                index = atoi(token + 6);
                                if (index == 1) strncpy(source, "auto", MAX_ATTR_LEN - 1);
                                else if (index == 2) strncpy(source, "by-pass-tray", MAX_ATTR_LEN - 1);
                                else if (index == 3) strncpy(source, "tray-1", MAX_ATTR_LEN - 1);
                                else if (index == 4) strncpy(source, "tray-2", MAX_ATTR_LEN - 1);

                                if (trayName[0] == '\0') {
                                    strncpy(trayName, source, MAX_ATTR_LEN - 1);
                                }
                            }
                            token = strtok(NULL, ";");
                        }

                        int found = 0;
                        for (int i = 0; i < num_media_tray_mappings; i++) {
                            if (strcmp(media_tray_map[i].source, source) == 0) {
                                strncpy(media_tray_map[i].trayName, trayName, MAX_ATTR_LEN - 1);
                                strncpy(media_tray_map[i].medianame, medianame, MAX_ATTR_LEN - 1);
                                found = 1;
                                break;
                            }
                        }
                        if (!found && num_media_tray_mappings < MAX_VALUES && media[0] != '\0') {
                            strncpy(media_tray_map[num_media_tray_mappings].media, media, MAX_ATTR_LEN - 1);
                            strncpy(media_tray_map[num_media_tray_mappings].source, source, MAX_ATTR_LEN - 1);
                            strncpy(media_tray_map[num_media_tray_mappings].trayName, trayName, MAX_ATTR_LEN - 1);
                            strncpy(media_tray_map[num_media_tray_mappings].medianame, medianame, MAX_ATTR_LEN - 1);
                            num_media_tray_mappings++;
                        }
                    } else if (strcmp(name, "printer-state") == 0 &&
                               value_tag == 0x23 && value_len == 4) {
                        /* printer-state is an IPP enum (RFC 8011 5.4.11),
                         * not the 0x21 'integer' tag. Fed into
                         * mp_printer_status_label() and shown under the
                         * ink/toner strips - see mp_draw_marker_strips(). */
                        printer_state_value = (int)mp_ipp_decode_be32(
                            (const UBYTE *)ipp_start + pos - value_len);
                        printf("Printer state: %d\n", printer_state_value);
                    } else if (strcmp(name, "printer-state-reasons") == 0 &&
                               value_tag == 0x44) {
                        /* 1setOf keyword (RFC 8011 5.4.12), same parallel-
                         * array shape as marker-names/marker-colors/etc.
                         * above - see mp_printer_status_label(). */
                        store_value(printer_state_reasons,
                                    &num_printer_state_reasons, value);
                    } else if (strcmp(name, "print-color-mode-supported") == 0 && value_tag == 0x44) {
                        store_value(supported_print_modes, &num_supported_print_modes, value);
                        printf("Added print-color-mode-supported: %s\n", value); }
                    else if (strcmp(name, "print-scaling-supported") == 0 && value_tag == 0x44) {
                        store_value(supported_scaling, &num_supported_scaling, value);
                        printf("Added print-scaling-supported: %s\n", value);
                    } else if (strcmp(name, "sides-supported") == 0 &&
                               value_tag == 0x44) {
                        if (strcmp(value, "one-sided") == 0 ||
                            strcmp(value, "two-sided-long-edge") == 0 ||
                            strcmp(value, "two-sided-short-edge") == 0) {
                            store_value(supported_sides, &num_supported_sides,
                                        value);
                            printf("Added sides-supported: %s\n", value);
                        }
                    } else if (strcmp(name, "operations-supported") == 0 &&
                               value_tag == 0x23 && value_len == 4) {
                        const UBYTE *raw =
                            (const UBYTE *)ipp_start + pos - value_len;
                        ULONG operation = ((ULONG)raw[0] << 24) |
                                          ((ULONG)raw[1] << 16) |
                                          ((ULONG)raw[2] << 8) |
                                          (ULONG)raw[3];
                        if (operation == 0x0005UL) supports_create_job = TRUE;
                        if (operation == 0x0006UL) supports_send_document = TRUE;
                    } else if (strcmp(name,
                                      "multiple-document-jobs-supported") == 0 &&
                               value_tag == 0x22 && value_len == 1) {
                        const UBYTE *raw =
                            (const UBYTE *)ipp_start + pos - value_len;
                        supports_multiple_document_jobs = raw[0] ? TRUE : FALSE;
                    } else if (strcmp(name,
                                      "multiple-document-handling-supported") == 0 &&
                               value_tag == 0x44 &&
                               strcmp(value, "single-document") == 0) {
                        supports_single_document_handling = TRUE;
                    } else if (strcmp(name,
                                      "pwg-raster-document-sheet-back") == 0 &&
                               value_tag == 0x44) {
                        if (strcmp(value, "normal") == 0 ||
                            strcmp(value, "rotated") == 0 ||
                            strcmp(value, "flipped") == 0 ||
                            strcmp(value, "manual-tumble") == 0) {
                            strncpy(pwg_sheet_back_value, value,
                                    sizeof(pwg_sheet_back_value) - 1);
                            pwg_sheet_back_value[
                                sizeof(pwg_sheet_back_value) - 1] = '\0';
                            printf("PWG sheet-back: %s\n",
                                   pwg_sheet_back_value);
                        }
                    } else if ((strcmp(name, "printer-resolution-default") == 0 ||
                                strcmp(name, "printer-resolution-supported") == 0 ||
                                strcmp(name, "pwg-raster-document-resolution-supported") == 0) &&
                               value_tag == 0x32 && value_len == 9) {
                        mp_add_ipp_resolution((const UBYTE *)ipp_start + pos - value_len,
                                              value_len);
                    } else if (strcmp(name, "print-quality-supported") == 0 &&
                               value_tag == 0x23 && value_len == 4) {
                        /* print-quality-supported is an IPP enum (RFC 8011
                         * 5.4.13, value tag 0x23), not the 0x21 'integer'
                         * tag - and its value is a 4-byte big-endian binary
                         * integer, not decimal text, so atoi() on it was
                         * always wrong. */
                        unsigned long quality = mp_ipp_decode_be32(
                            (const UBYTE *)ipp_start + pos - value_len);
                        const char *quality_name = NULL;

                        switch (quality) {
                            case 3: quality_name = "draft"; break;
                            case 4: quality_name = "normal"; break;
                            case 5: quality_name = "high"; break;
                            default: break; /* ignore unknown enum values */
                        }

                        if (quality_name) {
                            BOOL already_have = FALSE;
                            int qi;
                            for (qi = 0; qi < num_supported_quality; qi++) {
                                if (strcmp(supported_quality[qi], quality_name) == 0) {
                                    already_have = TRUE;
                                    break;
                                }
                            }
                            if (!already_have && num_supported_quality < MAX_QUALITIES) {
                                strcpy(supported_quality[num_supported_quality++], quality_name);
                                printf("Added print-quality-supported: %s\n", quality_name);
                            }
                        }
                    } else if (strcmp(name, "document-format-supported") == 0 && value_tag == 0x49) {
                        store_value(supported_formats, &num_supported_formats, value);
                    } else if (strcmp(name, "jpeg-k-octets-supported") == 0) {
                        jpeg_k_octets_reported = TRUE;
                    } else if (strcmp(name, "jpeg-x-dimension-supported") == 0) {
                        jpeg_x_dimension_reported = TRUE;
                    } else if (strcmp(name, "jpeg-y-dimension-supported") == 0) {
                        jpeg_y_dimension_reported = TRUE;
                    } else if (strcmp(name, "printer-make-and-model") == 0 &&
                               (value_tag == 0x41 || value_tag == 0x42)) {
                        strncpy(printer_make_model, value, sizeof(printer_make_model) - 1);
                        printer_make_model[sizeof(printer_make_model) - 1] = '\0';
                    } else if (strcmp(name, "printer-icons") == 0 &&
                               value_tag == 0x45 && printer_icon_uri[0] == '\0') {
                        strncpy(printer_icon_uri, value, sizeof(printer_icon_uri) - 1);
                        printer_icon_uri[sizeof(printer_icon_uri) - 1] = '\0';
                    } else if (strcmp(name, "marker-names") == 0 &&
                               (value_tag == 0x41 || value_tag == 0x42 ||
                                value_tag == 0x44)) {
                        store_value(marker_names, &num_marker_names, value);
                    } else if (strcmp(name, "marker-colors") == 0 &&
                               (value_tag == 0x41 || value_tag == 0x42 ||
                                value_tag == 0x44)) {
                        /* "#RRGGBB" (PWG5100.13) or CSS-style names
                         * ("cyan", "multi-color", ...) depending on the
                         * printer - resolved to a screen pen when drawn,
                         * not here. */
                        store_value(marker_colors, &num_marker_colors, value);
                    } else if (strcmp(name, "marker-types") == 0 &&
                               value_tag == 0x44) {
                        store_value(marker_types, &num_marker_types, value);
                    } else if (strcmp(name, "marker-levels") == 0 &&
                               value_tag == 0x21 && value_len == 4) {
                        /* Percent full, 0-100; RFC 3805 reserves negative
                         * values (-1, -2, ...) to mean "unknown"/"some
                         * value not currently reportable", not a real
                         * level - store as-is and let the drawing code
                         * treat negative as unknown rather than clamping
                         * here and losing that distinction. */
                        int level = (int)mp_ipp_decode_be32(
                            (const UBYTE *)ipp_start + pos - value_len);
                        store_int_value(marker_levels, &num_marker_levels, level);
                    } else if (strcmp(name, "marker-low-levels") == 0 &&
                               value_tag == 0x21 && value_len == 4) {
                        int level = (int)mp_ipp_decode_be32(
                            (const UBYTE *)ipp_start + pos - value_len);
                        store_int_value(marker_low_levels, &num_marker_low_levels, level);
                    } else if (strcmp(name, "marker-high-levels") == 0 &&
                               value_tag == 0x21 && value_len == 4) {
                        int level = (int)mp_ipp_decode_be32(
                            (const UBYTE *)ipp_start + pos - value_len);
                        store_int_value(marker_high_levels, &num_marker_high_levels, level);
                    }
                }

                if (pos == attr_start_pos) {
                    pos = ipp_len; // Force exit
                    break;
                }

                if (num_supported_print_modes > 0) {
                    // Ensure selected_print_mode is still valid
                    int match_found = 0;
                    for (int i = 0; i < num_supported_print_modes; i++) {
                        if (strcmp(supported_print_modes[i], selected_print_mode) == 0) {
                            print_mode = i;
                            match_found = 1;
                            break;
                        }
                    }
                    if (!match_found) {
                        strcpy(selected_print_mode, supported_print_modes[0]);
                        print_mode = 0;
                        printf("No match for saved print mode, defaulting to: %s\n", selected_print_mode);
                    }
                }

                attributes_processed++;
                if (window) {
                    struct IntuiMessage *imsg;
                    while ((imsg = GT_GetIMsg(window->UserPort))) {
                        GT_ReplyIMsg(imsg);
                    }
                }
            }

            if (pos == group_start_pos) {
                pos++;
            }
        } else {
            continue;
        }
    }

    if (attributes_processed >= max_attributes) {
        printf("Reached maximum attribute limit (%d), aborting parsing\n", max_attributes);
    }

    /* A response that parses but yields nothing usable (no formats, no
     * quality, no scaling, no media, no print modes, no make/model) is not
     * a printer that genuinely supports nothing - every real IPP
     * Everywhere/AirPrint printer reports at least some of these. Treat it
     * as a failed query so perform_query_flow() retries the next
     * port/attempt instead of caching an empty result. */
    if (num_supported_formats == 0 && num_supported_quality == 0 &&
        num_supported_scaling == 0 && num_supported_media == 0 &&
        num_supported_print_modes == 0 && printer_make_model[0] == '\0') {
        printf("Printer reported no usable capabilities - treating as a failed query\n");
        snprintf(response, maxlen, "Printer reported no capabilities");
        CloseSocket(sockfd);
        free(name);
        free(value);
        operation_in_progress = FALSE;
        return -1;
    }

    int media_index = 0;
    for (int i = 0; i < num_supported_media_sources; i++) {
        if (strcmp(supported_media_sources[i], "auto") == 0) continue;
        if (media_index >= num_supported_media) break;
        int found = 0;
        for (int j = 0; j < num_media_tray_mappings; j++) {
            if (strcmp(media_tray_map[j].source, supported_media_sources[i]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found && num_media_tray_mappings < MAX_VALUES) {
            strncpy(media_tray_map[num_media_tray_mappings].media, supported_media[media_index], MAX_ATTR_LEN - 1);
            strncpy(media_tray_map[num_media_tray_mappings].source, supported_media_sources[i], MAX_ATTR_LEN - 1);
            strncpy(media_tray_map[num_media_tray_mappings].trayName, supported_media_sources[i], MAX_ATTR_LEN - 1);
            strncpy(media_tray_map[num_media_tray_mappings].medianame, "Unknown", MAX_ATTR_LEN - 1);
            num_media_tray_mappings++;
        }
        media_index++;
    }

    printf("Media-Tray Mappings:\n");
    for (int i = 0; i < num_media_tray_mappings; i++) {
        printf("- %s -> %s (%s), medianame=%s\n", media_tray_map[i].media, media_tray_map[i].source, media_tray_map[i].trayName, media_tray_map[i].medianame);
    }
    printf("Supported Sources:\n");
    for (int i = 0; i < num_supported_media_sources; i++) {
        printf("- %s\n", supported_media_sources[i]);
    }
    printf("Supported Print Modes:\n");
    for (int i = 0; i < num_supported_print_modes; i++) {
        printf("- %s\n", supported_print_modes[i]);
    }

    // Check if the current print mode is supported
    const char *current_mode = print_mode == 0 ? "monochrome" : "color";
    int mode_supported = 0;
    for (int i = 0; i < num_supported_print_modes; i++) {
        if (strcmp(supported_print_modes[i], current_mode) == 0) {
            mode_supported = 1;
            break;
        }
    }
    if (!mode_supported) {
        printf("Warning: Selected print mode '%s' is not supported by the printer.\n", current_mode);
    }

    // Cleanup
    CloseSocket(sockfd);
    free(name);
    free(value);
    operation_in_progress = FALSE;

    if (window && vi) update_media_dropdown(window);
    if (window) update_print_mode_dropdown(window);
    if (window) update_scaling_dropdown(window);
    ensure_quality_defaults();
    if (window) update_quality_dropdown(window);
    /* update_engine_dropdown() must run before update_dpi_dropdown(): it is
     * what settles driver_engine_buffer on "pwg-raster" for a printer that
     * advertises it (see mp_rebuild_engine_options_from_query()), and the
     * DPI dropdown only offers the 300* compatibility entry when the engine
     * buffer already reads "pwg-raster" at the time it is built. Building
     * DPI first left a fresh Query showing plain "600 dpi" with no compat
     * entry until the config was saved and the app reopened, when the
     * cached-capabilities path (apply_cached_capabilities()) happened to
     * call them in the correct order (issue #43). */
    if (window) update_engine_dropdown(window, TRUE);
    if (window) update_dpi_dropdown(window);
    if (window) update_sides_dropdown(window);

    if (printer_make_model[0]) {
        printf("Printer: %s\n", printer_make_model);
    } else {
        printf("Printer did not report printer-make-and-model\n");
    }

    if (window) {
        mp_update_model_display(window);
        /* Preview the freshly-queried (not yet saved) model in the Unit
         * dropdown's current entry, rather than waiting for Save. */
        refresh_unit_dropdown(window);
    }

    if (num_supported_formats > 0) {
        printf("Printer document formats (%d):\n", num_supported_formats);
        for (int i = 0; i < num_supported_formats; i++) {
            printf("- %s\n", supported_formats[i]);
        }
    } else {
        printf("Printer did not report document-format-supported\n");
    }

    mp_warn_if_jpeg_nominal();

    printf("query_printer_attributes completed\n");
    return 0;
}

/* Shared by the Query button and the post-discovery "Use Selected" path:
 * tries 631 first (falling back to a user-typed port or 80), and on
 * success applies the fetched capabilities to the gadgets exactly like a
 * manual Query click. */
static void perform_query_flow(struct Window *win, const char *ip_only, int port_hint,
                               char *response, int response_size) {
    /* 631 is the IANA-registered IPP port and the one real printers' full
     * capability set lives on; port 80 is only ever a bonus/compat
     * endpoint some printers also answer on, sometimes with a lesser or
     * different response. When we don't have an explicit port (discovery
     * always calls with port_hint 0, and manual entry without a ":port"
     * does too), try 631 first so we don't latch onto an 80 response that
     * "succeeds" but is missing capabilities like scaling. An explicit
     * user-typed port is still tried first, ahead of the 631 fallback. */
    int ports_to_try[2];
    if (port_hint > 0) {
        ports_to_try[0] = port_hint;
        ports_to_try[1] = 631;
    } else {
        ports_to_try[0] = 631;
        ports_to_try[1] = 80;
    }
    int i, attempt;
    BOOL ok = FALSE;

    // A scan can fail an individual attempt for purely transient network
    // reasons (a slow/incomplete response - see query_printer_attributes).
    // Retry a few times per port before moving on, rather than treating one
    // flaky attempt as "the printer has no capabilities".
    for (i = 0; i < 2 && !ok; i++) {
        for (attempt = 0; attempt < 3 && !ok; attempt++) {
            int qrc;
            printf("Trying %s:%d (attempt %d/3)...\n", ip_only, ports_to_try[i], attempt + 1);
            qrc = query_printer_attributes(ip_only, ports_to_try[i], response,
                                           response_size);
            if (qrc == 0) {
                struct Gadget *ip_gadget;

                snprintf(ip_buffer, sizeof(ip_buffer), "%s:%d", ip_only, ports_to_try[i]);

                ip_gadget = glist;
                while (ip_gadget && ip_gadget->GadgetID != GAD_IP_STRING) {
                    ip_gadget = ip_gadget->NextGadget;
                }
                if (ip_gadget) {
                    GT_SetGadgetAttrs(ip_gadget, win, NULL,
                                      GTST_String, (ULONG)ip_buffer,
                                      TAG_DONE);
                }

                if (save_capability_cache(ip_only, ports_to_try[i], driver_path_buffer))
                    printf("Printer capabilities cached\n");
                else
                    printf("Warning: could not save printer capability cache\n");

                apply_job_defaults_to_gadgets(win);
                mp_check_any_engine_supported(win);
                ok = TRUE;
            } else {
                printf("Query attempt %d/3 on %s:%d failed\n", attempt + 1, ip_only, ports_to_try[i]);
                /* -2 = connect() itself couldn't be established (see
                 * mp_connect_with_timeout). A genuinely dead endpoint
                 * (nothing listening, no route) fails fast via
                 * ECONNREFUSED/host-unreachable well before the timeout,
                 * so it costs little to give it a second try - but some
                 * Wi-Fi printers (HP OfficeJet/Envy confirmed - issue #30)
                 * let their radio drop into power-save between jobs, and
                 * the first real TCP SYN after that can be slow enough to
                 * hit the connect timeout even though the printer is very
                 * much there and answers promptly once its radio is
                 * awake. Bailing to the next port after just one -2 used
                 * to mean that single slow wake-up permanently cost this
                 * port its shot, even though a second attempt right after
                 * would very likely have connected. Two attempts before
                 * moving on covers that case; a third would only slow
                 * down reaching the fallback port for endpoints that are
                 * actually dead. Retries stay worthwhile for -1 (the
                 * connection succeeded but something after that was
                 * flaky, e.g. a truncated response - see
                 * query_printer_attributes). */
                if (qrc == -2 && attempt > 0) break;
            }
        }
    }

    if (!ok) {
        custom_printf("CLEAR");
        custom_printf("Scan failed - please try Query again");
        mp_clear_printer_icon();
        mp_load_printer_icon_cache(FALSE);
    } else {
        mp_refresh_printer_icon();
    }
    /* Redraw either way: query_printer_attributes() already reset the
     * marker-* arrays before this loop even on failure, so a failed
     * re-query correctly clears a previous printer's ink levels off the
     * panel instead of leaving them looking current. */
    mp_draw_marker_strips();
    mp_draw_sides_hint();
    mp_draw_printer_icon();
}

/* Allocate the Query response only while a Query is running. Classic
 * systems may not have a contiguous 250 KiB block available even when
 * their total free memory is healthy, so retain the full buffer where
 * possible and fall back to still-useful 128 KiB or 64 KiB capacities.
 * query_printer_attributes() receives the actual capacity and therefore
 * cannot write as though a smaller fallback were still MAX_BUFFER bytes. */
static char *mp_alloc_query_response(int *out_size) {
    static const ULONG sizes[] = {
        (ULONG)MAX_BUFFER, 131072UL, 65536UL
    };
    int i;

    if (!out_size)
        return NULL;
    *out_size = 0;

    for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); ++i) {
        char *response = (char *)AllocVec(sizes[i], MEMF_ANY);
        if (response) {
            *out_size = (int)sizes[i];
            if (sizes[i] < (ULONG)MAX_BUFFER)
                printf("Using reduced %lu-byte Query response buffer\n",
                       (unsigned long)sizes[i]);
            return response;
        }
    }

    printf("Could not allocate a Query response buffer (minimum 65536 bytes)\n");
    return NULL;
}

static void perform_query_flow_allocated(struct Window *win,
                                         const char *ip_only,
                                         int port_hint) {
    int response_size;
    char *response = mp_alloc_query_response(&response_size);

    if (!response)
        return;

    perform_query_flow(win, ip_only, port_hint, response, response_size);
    FreeVec(response);
}

int send_pwg_print_job(const char *ip, int port, const char *media, const char *print_mode, unsigned char *pwg_data, int pwg_size) {
    if (operation_in_progress) {
        printf("Operation already in progress, please wait...\n");
        return -1;
    }
    operation_in_progress = TRUE;

    const char *selected_source = "auto";
    for (int i = 0; i < num_media_tray_mappings; i++) {
        if (strcmp(media_tray_map[i].media, media) == 0) {
            selected_source = media_tray_map[i].source;
            break;
        }
    }
    printf("Selected media: %s, source: %s, print mode: %s\n", media, selected_source, print_mode);

    struct sockaddr_in serv_addr;
    int sockfd = -1;
    unsigned char *ipp_payload = NULL;
    int offset = 0;
    char *http_header = NULL;
    char *response_buffer = NULL;
    int result = -1;

    ipp_payload = malloc(2048);
    if (!ipp_payload) {
        printf("Failed to allocate memory for IPP payload\n");
        operation_in_progress = FALSE;
        return -1;
    }
    memset(ipp_payload, 0, 2048);

    char uri[128];
    snprintf(uri, sizeof(uri), "ipp://%s/ipp", ip);
    int uri_len = strlen(uri);

    ipp_payload[offset++] = 0x01; ipp_payload[offset++] = 0x01;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x02;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x01;

    ipp_payload[offset++] = 0x01;
    ipp_payload[offset++] = 0x47; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x12;
    memcpy(&ipp_payload[offset], "attributes-charset", 18); offset += 18;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x05;
    memcpy(&ipp_payload[offset], "utf-8", 5); offset += 5;

    ipp_payload[offset++] = 0x48; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x1b;
    memcpy(&ipp_payload[offset], "attributes-natural-language", 27); offset += 27;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x02;
    memcpy(&ipp_payload[offset], "en", 2); offset += 2;

    ipp_payload[offset++] = 0x45; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0b;
    memcpy(&ipp_payload[offset], "printer-uri", 11); offset += 11;
    ipp_payload[offset++] = (uri_len >> 8) & 0xFF;
    ipp_payload[offset++] = uri_len & 0xFF;
    memcpy(&ipp_payload[offset], uri, uri_len); offset += uri_len;

    const char *job_name = "Amiga";
    int job_name_len = strlen(job_name);
    ipp_payload[offset++] = 0x42; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x08;
    memcpy(&ipp_payload[offset], "job-name", 8); offset += 8;
    ipp_payload[offset++] = (job_name_len >> 8) & 0xFF;
    ipp_payload[offset++] = job_name_len & 0xFF;
    memcpy(&ipp_payload[offset], job_name, job_name_len); offset += job_name_len;

    const char *doc_format = "image/pwg-raster";
    int doc_format_len = strlen(doc_format);
    ipp_payload[offset++] = 0x49; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0e;
    memcpy(&ipp_payload[offset], "document-format", 14); offset += 14;
    ipp_payload[offset++] = (doc_format_len >> 8) & 0xFF;
    ipp_payload[offset++] = doc_format_len & 0xFF;
    memcpy(&ipp_payload[offset], doc_format, doc_format_len); offset += doc_format_len;

    ipp_payload[offset++] = 0x02;

    int media_len = strlen(media);
    ipp_payload[offset++] = 0x44;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x05;
    memcpy(&ipp_payload[offset], "media", 5); offset += 5;
    ipp_payload[offset++] = (media_len >> 8) & 0xFF;
    ipp_payload[offset++] = media_len & 0xFF;
    memcpy(&ipp_payload[offset], media, media_len); offset += media_len;

    int source_len = strlen(selected_source);
    ipp_payload[offset++] = 0x44;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0c;
    memcpy(&ipp_payload[offset], "media-source", 12); offset += 12;
    ipp_payload[offset++] = (source_len >> 8) & 0xFF;
    ipp_payload[offset++] = source_len & 0xFF;
    memcpy(&ipp_payload[offset], selected_source, source_len); offset += source_len;


    if (!print_mode || strlen(print_mode) == 0) {
        print_mode = "monochrome";
    }
    int print_mode_len = strlen(print_mode);
    ipp_payload[offset++] = 0x44;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0f;
    memcpy(&ipp_payload[offset], "print-color-mode", 17); offset += 17;
    ipp_payload[offset++] = (print_mode_len >> 8) & 0xFF;
    ipp_payload[offset++] = print_mode_len & 0xFF;
    memcpy(&ipp_payload[offset], print_mode, print_mode_len); offset += print_mode_len;

    int scaling_len = strlen(selected_scaling);
    ipp_payload[offset++] = 0x44;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0d;
    memcpy(&ipp_payload[offset], "print-scaling", 13); offset += 13;
    ipp_payload[offset++] = (scaling_len >> 8) & 0xFF;
    ipp_payload[offset++] = scaling_len & 0xFF;
    memcpy(&ipp_payload[offset], selected_scaling, scaling_len); offset += scaling_len;

    int quality_value = 4;
    if (strcmp(selected_quality, "draft") == 0) quality_value = 3;
    else if (strcmp(selected_quality, "high") == 0) quality_value = 5;

    ipp_payload[offset++] = 0x21;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0d;
    memcpy(&ipp_payload[offset], "print-quality", 13); offset += 13;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x04;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x00;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = quality_value;

    ipp_payload[offset++] = 0x21; // enum
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x12;
    memcpy(&ipp_payload[offset], "printer-resolution", 18); offset += 18;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x06;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x01; // cross feed units = dpi
    ipp_payload[offset++] = 0x01; ipp_payload[offset++] = 0x2c; // 300
    ipp_payload[offset++] = 0x01; ipp_payload[offset++] = 0x2c; // 300
    ipp_payload[offset++] = 0x03;

    http_header = malloc(256);
    if (!http_header) {
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }
    snprintf(http_header, 256,
        "POST /ipp HTTP/1.1\r\nHost: %s\r\nContent-Type: application/ipp\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        ip, offset + pwg_size);

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        free(http_header);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }
    printf("Sent HTTP header\n");
    struct timeval timeout = {10, 0};
    struct timeval send_timeout = {10, 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char*)&send_timeout, sizeof(send_timeout));
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = inet_addr((STRPTR)ip);
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        CloseSocket(sockfd);
        free(http_header);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }
    printf("Sent IPP payload (%d bytes)\n", offset);
    printf("T. contlen: %d \n (header: %d\n, pwg: %d)\n", offset + pwg_size, offset, pwg_size);
    printf("Sending PWG data (%d bytes)...\n", pwg_size);
    if (send(sockfd, http_header, strlen(http_header), 0) < 0 ||
        send(sockfd, (char *)ipp_payload, offset, 0) < 0 ||
        safe_send(sockfd, (char *)pwg_data, pwg_size) < 0) {
        CloseSocket(sockfd);
        free(http_header);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }

    free(http_header);
    free(ipp_payload);

    response_buffer = malloc(4096);
    if (!response_buffer) {
        CloseSocket(sockfd);
        operation_in_progress = FALSE;
        return -1;
    }

    {
        int total_received = 0;
        int header_start = 0;
        int body_off = -1;
        int attempt;

        for (attempt = 0; attempt < 10; attempt++) {
            ssize_t received = recv(sockfd, response_buffer + total_received,
                                    4096 - 1 - total_received, 0);
            if (received <= 0) break;
            total_received += (int)received;
            response_buffer[total_received] = '\0';
            body_off = mp_http_find_body(response_buffer, total_received, header_start);
            if (body_off < 0) continue;
            {
                int status = mp_http_status(response_buffer, total_received, header_start);
                if (status >= 100 && status < 200) {
                    header_start = body_off;
                    body_off = -1;
                    continue;
                }
            }
            break;
        }

        if (body_off >= 0) {
            char *ipp_start = response_buffer + body_off;
            printf("IPP Status: 0x%02x%02x\n", ipp_start[2], ipp_start[3]);
        } else {
            printf("No response or receive timeout.\n");
        }
    }

    free(response_buffer);
    CloseSocket(sockfd);
    operation_in_progress = FALSE;
    return 0;
}

// Updated send_print_job to send the selected tray (media-source) and print mode
int send_print_job(const char *ip, int port, const char *filename, const char *media, const char *print_mode) {
    if (operation_in_progress) {
        printf("Operation already in progress, please wait...\n");
        return -1;
    }
    operation_in_progress = TRUE;

    // Find the selected media's tray
    const char *selected_source = "auto"; // Default fallback
    for (int i = 0; i < num_media_tray_mappings; i++) {
        if (strcmp(media_tray_map[i].media, media) == 0) {
            selected_source = media_tray_map[i].source;
            break;
        }
    }
    printf("Selected media: %s, source: %s, print mode: %s\n", media, selected_source, print_mode);

    struct sockaddr_in serv_addr;
    int sockfd = -1;
    unsigned char *ipp_payload = NULL;
    int offset = 0;
    unsigned char *file_data = NULL;
    FILE *file = NULL;
    char *http_header = NULL;
    char *response_buffer = NULL; // Dynamically allocate
    int result = -1;

    // Allocate IPP payload dynamically
    ipp_payload = malloc(2048);
    if (!ipp_payload) {
        printf("Failed to allocate memory for IPP payload\n");
        operation_in_progress = FALSE;
        return -1;
    }
    memset(ipp_payload, 0, 2048);

    char uri[128];
    snprintf(uri, sizeof(uri), "ipp://%s/ipp", ip);
    int uri_len = strlen(uri);

    // Open the file
    file = fopen(filename, "rb");
    if (!file) {
        printf("Failed to open file: %s\n", filename);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // Allocate buffer for file data
    file_data = malloc(file_size);

    if (file_size <= 0) {
        printf("Invalid or empty file\n");
        fclose(file);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }

    if (!file_data) {
        printf("Failed to allocate memory for file data\n");
        fclose(file);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }

    // Read the file
    fread(file_data, 1, file_size, file);
    fclose(file);
    file = NULL;

    // Build IPP payload
    ipp_payload[offset++] = 0x01; ipp_payload[offset++] = 0x01; // IPP version 1.1
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x02; // Print-Job operation
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x01; // Request ID

    ipp_payload[offset++] = 0x01; // Operation attributes group
    ipp_payload[offset++] = 0x47; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x12;
    memcpy(&ipp_payload[offset], "attributes-charset", 18); offset += 18;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x05;
    memcpy(&ipp_payload[offset], "utf-8", 5); offset += 5;

    ipp_payload[offset++] = 0x48; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x1b;
    memcpy(&ipp_payload[offset], "attributes-natural-language", 27); offset += 27;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x02;
    memcpy(&ipp_payload[offset], "en", 2); offset += 2;

    ipp_payload[offset++] = 0x45; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0b;
    memcpy(&ipp_payload[offset], "printer-uri", 11); offset += 11;
    ipp_payload[offset++] = (uri_len >> 8) & 0xFF;
    ipp_payload[offset++] = uri_len & 0xFF;
    memcpy(&ipp_payload[offset], uri, uri_len); offset += uri_len;

    const char *job_name = "Amiga";
    int job_name_len = strlen(job_name);
    ipp_payload[offset++] = 0x42; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x08;
    memcpy(&ipp_payload[offset], "job-name", 8); offset += 8;
    ipp_payload[offset++] = (job_name_len >> 8) & 0xFF;
    ipp_payload[offset++] = job_name_len & 0xFF;
    memcpy(&ipp_payload[offset], job_name, job_name_len); offset += job_name_len;

    const char *doc_format = "image/jpeg";
    int doc_format_len = strlen(doc_format);
    ipp_payload[offset++] = 0x49; ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0e;
    memcpy(&ipp_payload[offset], "document-format", 14); offset += 14;
    ipp_payload[offset++] = (doc_format_len >> 8) & 0xFF;
    ipp_payload[offset++] = doc_format_len & 0xFF;
    memcpy(&ipp_payload[offset], doc_format, doc_format_len); offset += doc_format_len;

    ipp_payload[offset++] = 0x02; // Job Template Attributes group

    int media_len = strlen(media);
    ipp_payload[offset++] = 0x44; // keyword
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x05;
    memcpy(&ipp_payload[offset], "media", 5); offset += 5;
    ipp_payload[offset++] = (media_len >> 8) & 0xFF;
    ipp_payload[offset++] = media_len & 0xFF;
    memcpy(&ipp_payload[offset], media, media_len); offset += media_len;

    // Add media-source attribute
    int source_len = strlen(selected_source);
    ipp_payload[offset++] = 0x44; // keyword
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0c;
    memcpy(&ipp_payload[offset], "media-source", 12); offset += 12;
    ipp_payload[offset++] = (source_len >> 8) & 0xFF;
    ipp_payload[offset++] = source_len & 0xFF;
    memcpy(&ipp_payload[offset], selected_source, source_len); offset += source_len;
/*
    if (!print_mode || strlen(print_mode) == 0) {
        printf("Invalid print mode (empty), falling back to 'monochrome'\n");
        print_mode = "monochrome";
    }
    // Add print-color-mode attribute
    int print_mode_len = strlen(print_mode);
    ipp_payload[offset++] = 0x44; // keyword
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0f;
    memcpy(&ipp_payload[offset], "print-color-mode", 17); offset += 17;
    ipp_payload[offset++] = (print_mode_len >> 8) & 0xFF;
    ipp_payload[offset++] = print_mode_len & 0xFF;
    memcpy(&ipp_payload[offset], print_mode, print_mode_len); offset += print_mode_len;

    //Scaling Options
    int scaling_len = strlen(selected_scaling);
    ipp_payload[offset++] = 0x44; // keyword (for print-scaling)
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0d;
    memcpy(&ipp_payload[offset], "print-scaling", 13); offset += 13;
    ipp_payload[offset++] = (scaling_len >> 8) & 0xFF;
    ipp_payload[offset++] = scaling_len & 0xFF;
    memcpy(&ipp_payload[offset], selected_scaling, scaling_len); offset += scaling_len;

    // Quality Options
    int quality_value = 4; // default to normal
    if (strcmp(selected_quality, "draft") == 0) quality_value = 3;
    else if (strcmp(selected_quality, "high") == 0) quality_value = 5;

    ipp_payload[offset++] = 0x21; // enum
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x0d;
    memcpy(&ipp_payload[offset], "print-quality", 13); offset += 13;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x04;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = 0x00;
    ipp_payload[offset++] = 0x00; ipp_payload[offset++] = quality_value;
*/
    ipp_payload[offset++] = 0x03; // End of attributes

    http_header = malloc(256);
    if (!http_header) {
        printf("Failed to allocate memory for HTTP header\n");
        free(file_data);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }
    snprintf(http_header, 256,
        "POST /ipp HTTP/1.1\r\nHost: %s\r\nContent-Type: application/ipp\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        ip, offset + file_size);

    printf("Sending JPEG to printer at %s...\n", ip);
    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        printf("Socket creation failed.\n");
        free(http_header);
        free(file_data);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }

    struct timeval timeout = {10, 0}; // 10-second timeout
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)) < 0) {
        printf("Failed to set socket timeout\n");
        CloseSocket(sockfd);
        free(http_header);
        free(file_data);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = inet_addr((STRPTR)ip);
    if (serv_addr.sin_addr.s_addr == INADDR_NONE) {
        printf("Invalid IP address: %s\n", ip);
        CloseSocket(sockfd);
        free(http_header);
        free(file_data);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("Failed to connect to printer.\n");
        CloseSocket(sockfd);
        free(http_header);
        free(file_data);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }

    // Process GUI events
    if (window) {
        struct IntuiMessage *imsg;
        while ((imsg = GT_GetIMsg(window->UserPort))) {
            GT_ReplyIMsg(imsg);
        }
    }

    // Send the data
    if (send(sockfd, http_header, strlen(http_header), 0) < 0 ||
        send(sockfd, (char *)ipp_payload, offset, 0) < 0 ||
        send(sockfd, file_data, file_size, 0) < 0) {
        printf("Failed sending data.\n");
        CloseSocket(sockfd);
        free(http_header);
        free(file_data);
        free(ipp_payload);
        operation_in_progress = FALSE;
        return -1;
    }

    free(http_header);
    free(file_data);
    free(ipp_payload);

    printf("Waiting for response...\n");
    // Dynamically allocate response_buffer
    response_buffer = malloc(4096);
    if (!response_buffer) {
        printf("Failed to allocate memory for response buffer\n");
        CloseSocket(sockfd);
        operation_in_progress = FALSE;
        return -1;
    }

    {
        int total_received = 0;
        int header_start = 0;
        int body_off = -1;
        int attempt;

        for (attempt = 0; attempt < 10; attempt++) {
            ssize_t received = recv(sockfd, response_buffer + total_received,
                                    4096 - 1 - total_received, 0);
            if (received <= 0) break;
            total_received += (int)received;
            response_buffer[total_received] = '\0';
            body_off = mp_http_find_body(response_buffer, total_received, header_start);
            if (body_off < 0) continue;
            {
                int status = mp_http_status(response_buffer, total_received, header_start);
                if (status >= 100 && status < 200) {
                    header_start = body_off;
                    body_off = -1;
                    continue;
                }
            }
            break;
        }

        if (total_received == 0) {
            printf("No response or receive timeout.\n");
            CloseSocket(sockfd);
            free(response_buffer);
            operation_in_progress = FALSE;
            return -1;
        }

        if (body_off >= 0) {
            char *ipp_start = response_buffer + body_off;
            printf("IPP Status: 0x%02x%02x\n", ipp_start[2], ipp_start[3]);
        } else {
            printf("Could not find IPP response payload.\n");
        }
    }

    free(response_buffer);
    CloseSocket(sockfd);
    operation_in_progress = FALSE;
    printf("send_print_job completed successfully\n");
    return 0;
}

// Function to create all GadTools gadgets
struct Gadget *createAllGadgets(struct Gadget **glistptr, void *vi, UWORD topborder) {
    struct NewGadget ng;
    struct Gadget *gad;

    // Initialize the gadget list
    gad = CreateContext(glistptr);
    if (!gad) {
        printf("Failed to create gadget context\n");
        return NULL;
    }

    // Set up the NewGadget structure
    ng.ng_TextAttr = &Topaz80;
    ng.ng_VisualInfo = vi;
    ng.ng_Flags = NG_HIGHLABEL;

    // Unit selector - which saved printer profile (ENV:MintPRINT/UnitN) is
    // being viewed/edited. Only Unit0 is what the driver actually prints
    // with; switching here reloads the rest of the form from that unit's
    // saved file.
    ng.ng_LeftEdge = 64;
    ng.ng_TopEdge = 3 + topborder;
    ng.ng_Width = 328;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Unit:";
    ng.ng_GadgetID = GAD_UNIT_DROPDOWN;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)unit_dropdown_labels,
        GTCY_Active, (ULONG)current_unit_index,
        TAG_DONE);
    if (!gad) {
        printf("Failed to create unit dropdown\n");
        return NULL;
    }

    // Copies the selected unit's saved settings over Unit0, the only slot
    // the driver actually reads at print time - the practical way to
    // "switch which printer is active" without touching driver code.
    ng.ng_LeftEdge = 400;
    ng.ng_Width = 92;
    ng.ng_Height = 12;
    ng.ng_TopEdge = 3 + topborder;
    ng.ng_GadgetText = (STRPTR)"_Activate";
    ng.ng_GadgetID = GAD_SET_ACTIVE_BUTTON;
    ng.ng_Flags = 0;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_',
        TAG_DONE);
    if (!gad) {
        printf("Failed to create activate button\n");
        return NULL;
    }
    ng.ng_Flags = NG_HIGHLABEL;

    // IP string gadget
    ng.ng_LeftEdge = 144;
    ng.ng_TopEdge = 21 + topborder;
    ng.ng_Width = 248;
    ng.ng_Height = 12;
    /* Not "...IP/Host": both this field and the driver resolve it with
     * inet_addr() only, so a hostname like "printer.local" silently fails
     * rather than being looked up. Label matches actual behaviour instead
     * of implying DNS/mDNS name support that doesn't exist. */
    ng.ng_GadgetText = (STRPTR)"_Printer IPv4:";
    ng.ng_GadgetID = GAD_IP_STRING;
    gad = CreateGadget(STRING_KIND, gad, &ng,
        GTST_String, (ULONG)ip_buffer,
        GTST_MaxChars, sizeof(ip_buffer) - 1,
        GA_Immediate, TRUE,
        GT_Underscore, '_',
        GACT_RELVERIFY, TRUE,
        TAG_DONE);
    if (!gad) {
        printf("Failed to create IP string gadget\n");
        return NULL;
    }



    // Query button - shares the Printer Model row in the compact layout.
    ng.ng_LeftEdge = 400;
    ng.ng_Width = 92;
    ng.ng_Height = 12;
    ng.ng_TopEdge = 39 + topborder;
    ng.ng_GadgetText = (STRPTR)"_Query";
    ng.ng_GadgetID = GAD_QUERY_BUTTON;
    ng.ng_Flags = 0;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_',
        TAG_DONE);
    if (!gad) {
        printf("Failed to create query button\n");
        return NULL;
    }

    // Printer Model (read-only display) - shows printer-make-and-model
    // from the last successful Query for this unit. Not user-editable;
    // persisted via MODEL= in the unit's own config file on Save.
    ng.ng_LeftEdge = 128;
    ng.ng_TopEdge = 39 + topborder;
    ng.ng_Width = 264;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Printer Model:";
    ng.ng_GadgetID = GAD_MODEL_DISPLAY;
    gad = CreateGadget(TEXT_KIND, gad, &ng,
        GTTX_Text, (ULONG)printer_make_model,
        GTTX_Justification, GTJ_LEFT,
        TAG_DONE);
    if (!gad) {
        printf("Failed to create model display\n");
        return NULL;
    }

    // Driver IPP path
    ng.ng_LeftEdge = 130;
    ng.ng_TopEdge = 57 + topborder;
    ng.ng_Width = 104;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"IPP _Path:";
    ng.ng_GadgetID = GAD_IPP_PATH;
    gad = CreateGadget(STRING_KIND, gad, &ng,
        GTST_String, (ULONG)driver_path_buffer,
        GTST_MaxChars, sizeof(driver_path_buffer) - 1,
        GA_Immediate, TRUE,
        GT_Underscore, '_',
        GACT_RELVERIFY, TRUE,
        TAG_DONE);
    if (!gad) {
        printf("Failed to create IPP path gadget\n");
        return NULL;
    }

    // Discover button - shares the Printer IPv4 row in the compact layout.
    // Preserve the previous TopEdge afterwards so this isolated button does
    // not affect the state used by later gadget setup.
    {
        UWORD row2_top = ng.ng_TopEdge;
        ng.ng_LeftEdge = 400;
        ng.ng_TopEdge = 21 + topborder;
        ng.ng_Width = 92;
        ng.ng_Height = 12;
        ng.ng_GadgetText = (STRPTR)"_Discover";
        ng.ng_GadgetID = GAD_DISCOVER_BUTTON;
        ng.ng_Flags = 0;
        gad = CreateGadget(BUTTON_KIND, gad, &ng,
            GT_Underscore, '_',
            TAG_DONE);
        if (!gad) {
            printf("Failed to create discover button\n");
            return NULL;
        }
        ng.ng_TopEdge = row2_top;
    }

    // Printer document engine: JPEG, PostScript, PWG Raster, PDF, or
    // Apple Raster (URF).
    // Printer Engine has the longest label in the left column; x=132 keeps
    // a small left margin while leaving the compact ink panel free at x=320.
    // Width 140 (was 180): the longest option text ("Apple Raster") is
    // still comfortably inside that at Topaz80's 8px/char, and the box's
    // right edge (130+140=270) now sits well clear of the ink panel at
    // x=320 instead of crowding it at the old 310.
    ng.ng_LeftEdge = 135;
    ng.ng_TopEdge = 78 + topborder;
    ng.ng_Width = 140;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Printer Engine:";
    ng.ng_GadgetID = GAD_ENGINE;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)engine_labels,
        GTCY_Active, mp_engine_active_index(),
        TAG_DONE);
    if (!gad) {
        printf("Failed to create printer engine gadget\n");
        return NULL;
    }

    // Where job files spool: RAM (T:, as MintPRINT has always done) or a
    // real hard drive device, for memory-tight systems - see
    // mp_build_spool_options(). Same width/row as the old Debug slot so
    // the stacked cycle gadgets stay visually aligned; Debug itself now
    // lives on the button row next to Save.
    ng.ng_LeftEdge = 135;
    ng.ng_TopEdge = 95 + topborder;
    ng.ng_Width = 140;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Spooler:";
    ng.ng_GadgetID = GAD_SPOOLER;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)mp_spool_label_ptrs,
        GTCY_Active, mp_spool_active_index(),
        TAG_DONE);
    if (!gad) {
        printf("Failed to create spooler gadget\n");
        return NULL;
    }

    // Media dropdown - same width as Scaling below it now that the
    // prettified labels ("A4 (Bypass Tray)") need far less room than the
    // raw IPP keywords ("iso_a4_210x297mm (by-pass-tray)") did.
    ng.ng_LeftEdge = 135;
    ng.ng_TopEdge = 108 + topborder;
    ng.ng_Width = 180;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Media (Tray):";
    ng.ng_GadgetID = GAD_MEDIA_DROPDOWN;
    ng.ng_Flags = NG_HIGHLABEL;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)media_dropdown_items,
        GTCY_Active, 0,
        GA_Disabled, driver_media_buffer[0] ? FALSE : TRUE,
        TAG_DONE);
    if (!gad) {
        printf("Failed to create media dropdown\n");
        return NULL;
    }
    media_dropdown = gad;  // Save it globally

    // Scaling dropdown
    ng.ng_LeftEdge = 135;
    ng.ng_TopEdge = 126 + topborder;
    ng.ng_Width = 150;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Scaling:";
    ng.ng_GadgetID = GAD_SCALING_MODE;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
    GTCY_Labels, (ULONG)scaling_mode_labels,
    GTCY_Active, 0,
        GA_Disabled, driver_scaling_buffer[0] ? FALSE : TRUE,
    TAG_DONE);

    // Quality dropdown
    ng.ng_LeftEdge = 135;
    ng.ng_TopEdge = 144 + topborder;
    ng.ng_Width = 150;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Quality:";
    ng.ng_GadgetID = GAD_QUALITY_MODE;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)quality_mode_labels,
        GTCY_Active, 0,
        GA_Disabled, driver_quality_buffer[0] ? FALSE : TRUE,
        TAG_DONE);

    // Capture DPI - shares the Quality row and is populated by Query from
    // printer-resolution-supported/PWG raster resolution capabilities.
    ng.ng_LeftEdge = 350;
    ng.ng_Width = 100;
    ng.ng_Height = 12;
    ng.ng_TopEdge = 180 + topborder;
    ng.ng_GadgetText = (STRPTR)"DPI:";
    ng.ng_GadgetID = GAD_RESOLUTION;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)resolution_labels,
        GTCY_Active, (ULONG)mp_dpi_active_index(driver_resolution),
        GA_Disabled, num_supported_dpi > 0 ? FALSE : TRUE,
        TAG_DONE);
    if (!gad) {
        printf("Failed to create DPI gadget\n");
        return NULL;
    }

    // Print Mode radio buttons
    ng.ng_LeftEdge = 135;
    ng.ng_TopEdge = 162 + topborder;
    ng.ng_Width = 150;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Print Mode:";
    ng.ng_GadgetID = GAD_PRINT_MODE;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)print_mode_labels,
        GTCY_Active, 0,
        GA_Disabled, driver_color_buffer[0] ? FALSE : TRUE,
        TAG_DONE);
    if (!gad) {
        printf("Failed to create print mode radio buttons\n");
        return NULL;
    }

    /* Duplex needs multiple Amiga pages in one PWG Raster document. Query
     * enables these choices only for PWG Raster when the printer advertises
     * the requested sides value. */
    ng.ng_LeftEdge = 350;
    ng.ng_Width = 150;
    ng.ng_Height = 12;
    ng.ng_TopEdge = 162 + topborder;
    ng.ng_GadgetText = (STRPTR)"Sides:";
    ng.ng_GadgetID = GAD_SIDES;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)mp_sides_label_ptrs,
        GTCY_Active, mp_sides_active_index(),
        GA_Disabled, TRUE,
        TAG_DONE);
    if (!gad) {
        printf("Failed to create sides gadget\n");
        return NULL;
    }

    // Test Print button
    ng.ng_LeftEdge = 10;
    ng.ng_TopEdge = 198 + topborder;
    ng.ng_Width = 110;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"_Test Print";
    ng.ng_GadgetID = GAD_PRINT_BUTTON;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_',
        TAG_DONE);
    if (!gad) {
        printf("Failed to create print button\n");
        return NULL;
    }

    // Enable/disable diagnostic logs and retained rendered jobs. Shares
    // the button row's spare space between Test Print and Save; the
    // cycle's own label text ("Debug On"/"Debug Off") is self-explanatory
    // so it needs no separate GadgetText prefix, unlike the stacked
    // Printer Engine/Spooler cycles above.
    ng.ng_LeftEdge = 160;
    ng.ng_TopEdge = 198 + topborder;
    ng.ng_Width = 110;
    ng.ng_Height = 12;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID = GAD_DEBUG;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
        GTCY_Labels, (ULONG)debug_labels,
        GTCY_Active, driver_debug ? 1 : 0,
        TAG_DONE);
    if (!gad) {
        printf("Failed to create debug gadget\n");
        return NULL;
    }

    // Save button - same action as File -> Save Driver Settings.
    ng.ng_LeftEdge = 304;
    ng.ng_Width = 90;
    ng.ng_TopEdge = 198 + topborder;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"_Save";
    ng.ng_GadgetID = GAD_SAVE_BUTTON;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_',
        TAG_DONE);
    if (!gad) {
        printf("Failed to create save button\n");
        return NULL;
    }

    // Exit button
    ng.ng_LeftEdge = 408;
    ng.ng_Width = 90;
    ng.ng_TopEdge = 198 + topborder;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"_Exit";
    ng.ng_GadgetID = GAD_EXIT_BUTTON;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_',
        TAG_DONE);
    if (!gad) {
        printf("Failed to create exit button\n");
        return NULL;
    }

    // Keep spooled jobs - only meaningful, and only enabled, once Spooler
    // names a real hard drive (see mp_spool_keep_available()); the
    // GAD_SPOOLER handler below disables and unticks this live the moment
    // Spooler is switched back to RAM.
    //
    // PLACETEXT_RIGHT is required here: CHECKBOX_KIND's own default
    // placement is PLACETEXT_LEFT (label to the LEFT of the box, unlike
    // BUTTON_KIND/CYCLE_KIND above), which at this gadget's LeftEdge=10
    // - flush against the window's own left edge - draws the label
    // entirely off-window, leaving what looks like an unlabelled
    // checkbox.
    ng.ng_LeftEdge = 10;
    ng.ng_TopEdge = 180 + topborder;
    ng.ng_Width = 160;
    ng.ng_Height = 12;
    ng.ng_GadgetText = (STRPTR)"Keep Jobs (HDD)";
    ng.ng_GadgetID = GAD_SPOOL_KEEP;
    ng.ng_Flags = PLACETEXT_RIGHT;
    gad = CreateGadget(CHECKBOX_KIND, gad, &ng,
        GTCB_Checked, (ULONG)(mp_spool_keep_available() && driver_spool_keep),
        GA_Disabled, (ULONG)(mp_spool_keep_available() ? FALSE : TRUE),
        TAG_DONE);
    if (!gad) {
        printf("Failed to create keep-spooled-jobs checkbox\n");
        return NULL;
    }

    // Opens the Spooler management window listing tracked jobs - see
    // mp_spool_win_open(). Unlike run_discovery_selection(), it is
    // non-modal: it returns immediately and process_window_events()'s
    // main loop drives it from then on, so both windows stay usable.
    ng.ng_LeftEdge = 175;
    ng.ng_TopEdge = 180 + topborder;
    ng.ng_Width = 130;
    ng.ng_Height = 12;
    ng.ng_Flags = 0;
    ng.ng_GadgetText = (STRPTR)"_View Spooler";
    ng.ng_GadgetID = GAD_VIEW_SPOOL;
    gad = CreateGadget(BUTTON_KIND, gad, &ng,
        GT_Underscore, '_',
        TAG_DONE);
    if (!gad) {
        printf("Failed to create view spool button\n");
        return NULL;
    }

    return gad;
}

// Function to process window events using GadTools message handling
void process_window_events(struct Window *win) {
    struct IntuiMessage *imsg;
    ULONG imsgClass;
    UWORD imsgCode;
    struct Gadget *gad;
    BOOL terminated = FALSE;
    char ip_only[64];
    int port = -1;

    while (!terminated) {
        ULONG window_signal = 1L << win->UserPort->mp_SigBit;
        ULONG wait_mask = window_signal;
        ULONG received_signals;

        if (test_print_job.active && test_print_job.port)
            wait_mask |= 1L << test_print_job.port->mp_SigBit;
        /* Spooler window folded into the same Wait() as the main window's
         * own port, not a separate nested event loop - see
         * mp_spool_win_process()'s own comment for why: that is what
         * keeps this window and the Spooler window both live for input
         * at once instead of one blocking the other. */
        if (g_spool_win)
            wait_mask |= 1L << g_spool_win->UserPort->mp_SigBit;

        received_signals = Wait(wait_mask);
        if (test_print_job.active && test_print_job.port &&
            (received_signals & (1L << test_print_job.port->mp_SigBit))) {
            mp_test_print_complete(win);
        }
        if (g_spool_win &&
            (received_signals & (1L << g_spool_win->UserPort->mp_SigBit))) {
            mp_spool_win_process();
        }
        if (!(received_signals & window_signal))
            continue;

        imsg = GT_GetIMsg(win->UserPort);
        while (!terminated && imsg) {
            gad = (struct Gadget *)imsg->IAddress;
            imsgClass = imsg->Class;
            imsgCode = imsg->Code;
            /* IAddress is only actually a struct Gadget* for gadget-related
             * classes - deliberately NOT dereferencing gad->GadgetID here
             * for other classes (e.g. IDCMP_REFRESHWINDOW), where it can be
             * something else entirely. */

            GT_ReplyIMsg(imsg);

            switch (imsgClass) {
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                        case GAD_UNIT_DROPDOWN:
                        {
                            ULONG selected = (ULONG)imsgCode;
                            if (selected < (ULONG)MAX_UNITS && (int)selected != current_unit_index) {
                                current_unit_index = (int)selected;
                                custom_printf("CLEAR");
                                reload_current_unit(win);
                            }
                        }
                        break;

                        case GAD_SET_ACTIVE_BUTTON:
                        {
                            custom_printf("CLEAR");

                            if (current_unit_index == 0) {
                                custom_printf("Unit0 is already the active printer.\n");
                            } else if (!unit_file_exists(current_unit_index)) {
                                custom_printf("Unit%d has no saved settings yet - nothing to activate.\n",
                                              current_unit_index);
                            } else {
                                char src_env[64], src_envarc[64];
                                char dst_env[64], dst_envarc[64];
                                BOOL ok;

                                unit_config_path(current_unit_index, FALSE, src_env, sizeof(src_env));
                                unit_config_path(current_unit_index, TRUE, src_envarc, sizeof(src_envarc));
                                unit_config_path(0, FALSE, dst_env, sizeof(dst_env));
                                unit_config_path(0, TRUE, dst_envarc, sizeof(dst_envarc));

                                ok = ensure_config_dir((CONST_STRPTR)"ENV:MintPRINT") &&
                                     ensure_config_dir((CONST_STRPTR)"ENVARC:MintPRINT") &&
                                     mp_copy_file((CONST_STRPTR)src_env, (CONST_STRPTR)dst_env) &&
                                     mp_copy_file((CONST_STRPTR)src_envarc, (CONST_STRPTR)dst_envarc);

                                if (ok) {
                                    char src_cache_env[64], src_cache_envarc[64];
                                    char dst_cache_env[64], dst_cache_envarc[64];

                                    /* Best-effort: carry the cached capabilities over too, so
                                     * Unit0 doesn't need a fresh Query. Fine if there is none. */
                                    unit_cache_path(current_unit_index, FALSE, src_cache_env, sizeof(src_cache_env));
                                    unit_cache_path(current_unit_index, TRUE, src_cache_envarc, sizeof(src_cache_envarc));
                                    unit_cache_path(0, FALSE, dst_cache_env, sizeof(dst_cache_env));
                                    unit_cache_path(0, TRUE, dst_cache_envarc, sizeof(dst_cache_envarc));
                                    mp_copy_file((CONST_STRPTR)src_cache_env, (CONST_STRPTR)dst_cache_env);
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
                                                  current_unit_index);
                                    current_unit_index = 0;
                                    reload_current_unit(win);
                                    refresh_unit_dropdown(win);
                                } else {
                                    custom_printf("Could not copy Unit%d to Unit0.\n", current_unit_index);
                                }
                            }
                        }
                        break;

                        case GAD_MEDIA_DROPDOWN:
                        {
                            ULONG selected = (ULONG)imsgCode;
                            if (selected < (ULONG)num_media_tray_mappings) {
                                strncpy(driver_media_buffer,
                                        media_tray_map[selected].media,
                                        sizeof(driver_media_buffer) - 1);
                                driver_media_buffer[sizeof(driver_media_buffer) - 1] = '\0';
                                strncpy(driver_source_buffer,
                                        media_tray_map[selected].source,
                                        sizeof(driver_source_buffer) - 1);
                                driver_source_buffer[sizeof(driver_source_buffer) - 1] = '\0';
                                printf("Selected index = %lu, value = %s\n",
                                       selected, media_tray_map[selected].media);
                            } else {
                                printf("Invalid selection index = %lu\n", selected);
                            }
                        }
                        break;

                        case GAD_IP_STRING:
                        {
                            char *current_ip;
                            GT_RefreshWindow(win, NULL);
                            current_ip = mp_string_gadget_value(gad);
                            printf("Got pointer: %p\n", current_ip);
                            if (current_ip) {
                                printf("Raw IP string from gadget: '%s'\n", current_ip);
                                strncpy(ip_buffer, current_ip, sizeof(ip_buffer) - 1);
                                ip_buffer[sizeof(ip_buffer) - 1] = '\0';
                                printf("IP buffer after update: '%s'\n", ip_buffer);
                            } else {
                                printf("Failed to retrieve IP string from gadget\n");
                            }
                        }
                        break;
                        case GAD_IPP_PATH:
                        {
                            char *path = mp_string_gadget_value(gad);
                            if (path) {
                                strncpy(driver_path_buffer, path,
                                        sizeof(driver_path_buffer) - 1);
                                driver_path_buffer[sizeof(driver_path_buffer) - 1] = '\0';
                            }
                        }
                        break;

                        case GAD_DEBUG:
                            driver_debug = imsgCode ? TRUE : FALSE;
                            break;

                        case GAD_SPOOLER:
                        {
                            ULONG selected = (ULONG)imsgCode;
                            if (selected < (ULONG)mp_spool_option_count) {
                                strncpy(driver_spool_buffer,
                                        mp_spool_value_storage[selected],
                                        sizeof(driver_spool_buffer) - 1);
                                driver_spool_buffer[
                                    sizeof(driver_spool_buffer) - 1] = '\0';
                            }
                            mp_update_spool_keep_gadget(win);
                        }
                        break;

                        case GAD_SPOOL_KEEP:
                            driver_spool_keep = imsgCode ? TRUE : FALSE;
                            break;

                        case GAD_VIEW_SPOOL:
                            mp_spool_win_open(win);
                            break;

                        case GAD_QUALITY_MODE:
                        {
                            ULONG selected = (ULONG)imsgCode;
                            if (selected < (ULONG)num_supported_quality) {
                                strncpy(selected_quality, supported_quality[selected],
                                        sizeof(selected_quality) - 1);
                                selected_quality[sizeof(selected_quality) - 1] = '\0';
                                strncpy(driver_quality_buffer, supported_quality[selected],
                                        sizeof(driver_quality_buffer) - 1);
                                driver_quality_buffer[sizeof(driver_quality_buffer) - 1] = '\0';
                            }
                        }
                        break;

                        case GAD_PRINT_MODE:
                        {
                            ULONG selected = (ULONG)imsgCode;
                            print_mode = selected;
                            if (selected < num_supported_print_modes) {
                                strncpy(selected_print_mode, supported_print_modes[selected], MAX_ATTR_LEN - 1);
                                selected_print_mode[MAX_ATTR_LEN - 1] = '\0';
                                strncpy(driver_color_buffer, supported_print_modes[selected],
                                        sizeof(driver_color_buffer) - 1);
                                driver_color_buffer[sizeof(driver_color_buffer) - 1] = '\0';
                                printf("Print mode set to: %s\n", selected_print_mode);
                            }
                        }
                        break;

                        case GAD_ENGINE:
                        {
                            ULONG selected = (ULONG)imsgCode;
                            if (selected < (ULONG)mp_engine_count) {
                                strncpy(driver_engine_buffer,
                                        mp_engine_value_map[selected],
                                        sizeof(driver_engine_buffer) - 1);
                                driver_engine_buffer[
                                    sizeof(driver_engine_buffer) - 1] = '\0';
                                driver_engine_explicit = TRUE;
                                update_sides_dropdown(win);
                                update_dpi_dropdown(win);
                                mp_draw_sides_hint();
                            }
                        }
                        break;

                        case GAD_SIDES:
                        {
                            ULONG selected = (ULONG)imsgCode;
                            if (selected < (ULONG)mp_sides_option_count) {
                                strncpy(driver_sides_buffer,
                                        mp_sides_value_storage[selected],
                                        sizeof(driver_sides_buffer) - 1);
                                driver_sides_buffer[
                                    sizeof(driver_sides_buffer) - 1] = '\0';
                                printf("Sides set to: %s\n", driver_sides_buffer);
                                mp_draw_sides_hint();
                            }
                        }
                        break;

                        case GAD_RESOLUTION:
                        {
                            ULONG selected = (ULONG)imsgCode;
                            if (selected < (ULONG)mp_dpi_options.count) {
                                driver_resolution =
                                    mp_dpi_options.values[selected];
                                driver_resolution_explicit = TRUE;
                                if (mp_dpi_options.compatibility[selected])
                                    printf("DPI set to 300 compatibility mode (not printer-reported)\n");
                                else
                                    printf("DPI set to %d\n", driver_resolution);
                            }
                        }
                        break;

                        case GAD_SCALING_MODE:
                        {
                            ULONG selected = (ULONG)imsgCode;
                            if (selected < (ULONG)num_supported_scaling) {
                                strncpy(selected_scaling, supported_scaling[selected], MAX_ATTR_LEN - 1);
                                selected_scaling[MAX_ATTR_LEN - 1] = '\0';
                                strncpy(driver_scaling_buffer, supported_scaling[selected],
                                        sizeof(driver_scaling_buffer) - 1);
                                driver_scaling_buffer[sizeof(driver_scaling_buffer) - 1] = '\0';
                                printf("Scaling mode set to: %s\n", selected_scaling);
                            }
                        }
                        break;

                        case GAD_QUERY_BUTTON:
                        {
                            GT_RefreshWindow(win, NULL);
                        
                            // Get IP string from gadget
                            struct Gadget *ip_gadget = glist;
                            while (ip_gadget && ip_gadget->GadgetID != GAD_IP_STRING) {
                                ip_gadget = ip_gadget->NextGadget;
                            }
                        
                            if (ip_gadget) {
                                char *ip_string = mp_string_gadget_value(ip_gadget);
                                if (ip_string) {
                                    strncpy(ip_buffer, ip_string, sizeof(ip_buffer) - 1);
                                    ip_buffer[sizeof(ip_buffer) - 1] = '\0';
                                    printf("IP buffer updated to: '%s'\n", ip_buffer);
                                }
                            }
                        
                            // Parse IP and optional port
                            if (!parse_ip_and_port(ip_buffer, ip_only, sizeof(ip_only), &port)) {
                                /* Report the bad address and keep the window
                                 * open - this used to "return;", which exits
                                 * process_window_events()'s entire event loop
                                 * (and leaks the response buffer below it),
                                 * closing Settings instead of just failing
                                 * this one Query attempt. */
                                printf("Invalid IP format: '%s'\n", ip_buffer);
                                break;
                            }
                        
                            // Try default + fallback ports, apply capabilities on success
                            perform_query_flow_allocated(win, ip_only, port);
                        }
                        break;

                        case GAD_DISCOVER_BUTTON:
                        {
                            struct DiscoveredPrinter found[MAX_DISCOVERY_RESULTS];
                            int found_count;
                            char chosen_ip[16];

                            GT_RefreshWindow(win, NULL);
                            printf("CLEAR");

                            found_count = discover_printers_on_lan(found, MAX_DISCOVERY_RESULTS);

                            if (found_count <= 0) {
                                printf("No printers found via SSDP or mDNS.\n");
                                printf("Enter the printer IP manually and press Query.\n");
                            } else {
                                printf("Found %d candidate device(s).\n", found_count);
                                if (run_discovery_selection(win, found, found_count, chosen_ip, sizeof(chosen_ip))) {
                                    struct Gadget *disc_ip_gadget = glist;

                                    strncpy(ip_buffer, chosen_ip, sizeof(ip_buffer) - 1);
                                    ip_buffer[sizeof(ip_buffer) - 1] = '\0';

                                    while (disc_ip_gadget && disc_ip_gadget->GadgetID != GAD_IP_STRING) {
                                        disc_ip_gadget = disc_ip_gadget->NextGadget;
                                    }
                                    if (disc_ip_gadget) {
                                        GT_SetGadgetAttrs(disc_ip_gadget, win, NULL,
                                                          GTST_String, (ULONG)ip_buffer,
                                                          TAG_DONE);
                                    }

                                    perform_query_flow_allocated(win, chosen_ip, 0);
                                } else {
                                    printf("Discovery selection cancelled.\n");
                                }
                            }
                        }
                        break;

                        case GAD_PRINT_BUTTON:
                        {
                            GT_RefreshWindow(win, NULL);
                            mintprint_test_page(win);
                        }
                        break;

                        case GAD_SAVE_BUTTON:
                            if (save_driver_config(win))
                                printf("MintPRINT Unit%d saved to ENV: and ENVARC:\n", current_unit_index);
                            else
                                printf("Failed to save MintPRINT Unit%d settings\n", current_unit_index);
                            break;

                        case GAD_EXIT_BUTTON:
                            terminated = TRUE;
                            break;
                    }
                    break;

                case IDCMP_CLOSEWINDOW:
                    terminated = TRUE;
                    break;

                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win);
                    GT_EndRefresh(win, TRUE);
                    /* GT_BeginRefresh/EndRefresh only repaints GadTools
                     * gadgets - the status box is hand-drawn and needs its
                     * own replay here, or it looks emptied out any time
                     * something forces a refresh (e.g. Printer Prefs
                     * opening on top of this window and closing again). */
                    redraw_output_box();
                    mp_draw_marker_strips();
                    mp_draw_sides_hint();
                    mp_draw_printer_icon();
                    break;

                    case IDCMP_MENUPICK:
                    {
                        ULONG code = imsg->Code;
                        while (code != MENUNULL) {
                            UWORD menu_num = MENUNUM(code);
                            UWORD item_num = ITEMNUM(code);
                    
                            if (menu_num == 0) { // File menu
                                switch (item_num) {
                                    case 0: // Save Settings
                                        save_print_mode();
                                        if (save_driver_config(win))
                                            printf("MintPRINT Unit%d saved to ENV: and ENVARC:\n", current_unit_index);
                                        else
                                            printf("Failed to save MintPRINT Unit%d settings\n", current_unit_index);
                                        break;

                                    case 1: // Load Settings
                                        reload_current_unit(win);
                                        break;

                                    case 3: // About MintPRINT...
                                        show_about(win);
                                        break;

                                    case 5: // Quit
                                        terminated = TRUE;
                                        break;
                                }
                            } else if (menu_num == 1) { // Help menu
                                switch (item_num) {
                                    case 0: // MintPrint Settings Help...
                                        mp_launch_help_guide();
                                        break;
                                }
                            }

                            code = MENUNULL; // Only handling one menu item per event
                        }
                    }
                    break;
                    
                    
            }

            imsg = GT_GetIMsg(win->UserPort);
        }
    }

    if (test_print_job.active)
        mp_test_print_cancel(win);

    /* The main window is closing (or the app is quitting) - an orphaned
     * Spooler window left open would keep its UserPort registered with
     * Intuition after this Task exits, which is not safe. Close it the
     * same way its own Close button would. */
    if (g_spool_win)
        mp_spool_win_close();
}

static BOOL mp_open_tcp_stack(void) {
    LONG probe_socket;

    /* Match the minimum version used by the printer driver. Opening the
     * library alone is not quite enough: a stale/incomplete installation can
     * expose bsdsocket.library while still being unable to create sockets. */
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) return FALSE;

    probe_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (probe_socket < 0) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
        return FALSE;
    }

    CloseSocket(probe_socket);
    return TRUE;
}

static void mp_show_tcp_stack_required(void) {
    struct EasyStruct es;

    es.es_StructSize = sizeof(struct EasyStruct);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)"MintPrint Settings";
    es.es_TextFormat = (UBYTE *)
        "MintPRINT needs a running TCP/IP stack.\n\n"
        "bsdsocket.library V4 could not be opened, or it could not\n"
        "create a socket. Start or install Roadshow, AmiTCP, Miami,\n"
        "or another compatible TCP/IP stack, then run MintPRINT again.\n\n"
        "No printer settings or driver files have been changed.";
    es.es_GadgetFormat = (UBYTE *)"Exit";
    EasyRequest(NULL, &es, NULL);
}

// Main function
int main(void) {
    UWORD topborder;

    /* Open libraries with version checks.
     *
     * EXPERIMENTAL: pinned at v37 (AmigaOS 2.04, where gadtools.library was
     * introduced) rather than the v39 (AmigaOS 3.0) this used to require.
     * intuition.library/graphics.library/gadtools.library all existed at
     * v37; the driver-side library opens (dos.library/graphics.library in
     * driver/driver_core.c and driver/command_table.c) already only ever
     * asked for v37. GT_SetGadgetAttrs, the GTCY_ tags, and GetVisualInfo()
     * below are all v36+ gadtools.library API. The one caller-side v39-only
     * call this file makes, ObtainBestPenA() (graphics.library v39,
     * marker-colour ink strip), is separately guarded at its call site -
     * see there.
     *
     * NOT YET PHYSICALLY CONFIRMED on real AmigaOS 2.0/2.04 hardware or
     * emulation - unlike every other AmigaOS-version claim in this codebase,
     * which only gets made after a real test (see README.md's changelog and
     * docs/OS31_SUPPORT.md for that convention). This is exactly that
     * pending test; a v37-class system may still hit some other v38+-only
     * behaviour this audit missed. */
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!IntuitionBase) {
        printf("Failed to open intuition.library\n");
        return 1;
    }

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    if (!GfxBase) {
        printf("Failed to open graphics.library\n");
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    GadToolsBase = OpenLibrary("gadtools.library", 37);
    if (!GadToolsBase) {
        printf("Requires V37 gadtools.library\n");
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    if (!mp_open_tcp_stack()) {
        printf("A working bsdsocket.library V4 TCP/IP stack is required\n");
        mp_show_tcp_stack_required();
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    /* Same topaz.font size (8) GadTools already opens for every gadget in
     * this window via ng_TextAttr - not the separate size-6 variant this
     * used to request, which a real-hardware report tied to WordWorth
     * having been run: RectFill (the status box's border/background)
     * kept drawing fine, only Text() using this font produced nothing,
     * consistent with that specific font variant's glyph data being the
     * one thing broken rather than this window/RastPort in general. */
    font = OpenFont(&Topaz80);
    if (!font) {
        printf("Failed to open Topaz font\n");
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    // Lock the default public screen
    screen = LockPubScreen(NULL);
    if (!screen) {
        printf("Could not lock public screen\n");
        CloseFont(font);
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    // Get visual info
    vi = GetVisualInfo(screen, TAG_DONE);
    if (!vi) {
        printf("Failed to get visual info\n");
        UnlockPubScreen(NULL, screen);
        CloseFont(font);
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    // Calculate top border
    topborder = screen->WBorTop + (screen->Font->ta_YSize + 1);
    g_topborder = topborder;
    /* Cycle label pointers already target process-lifetime static storage.
     * seed_saved_option_labels() populated those arrays above. */
    // Load the same Unit0 profile used by DEVS:Printers/MintPRINT.
    load_driver_config();
    mp_build_spool_options();
    seed_saved_option_labels();

    // Load print mode from ENV:
    load_print_mode();

    // Seed the Unit dropdown's labels from whatever is saved on disk.
    refresh_unit_dropdown(NULL);

    // Create gadgets
    if (!createAllGadgets(&glist, vi, topborder)) {
        printf("Failed to create gadgets\n");
        FreeVisualInfo(vi);
        UnlockPubScreen(NULL, screen);
        CloseFont(font);
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    // Open window
    window = OpenWindowTags(NULL,
        WA_Title, (ULONG)"MintPrint Settings",
        WA_Gadgets, (ULONG)glist,
        WA_AutoAdjust, TRUE,
        WA_Width, 520,
        WA_MinWidth, 520,
        /* Keep the compact historical height. OUTPUT_TOP is positioned so
         * the complete hand-drawn status box remains inside this 314px
         * RastPort, with no unused strip below it. */
        WA_InnerHeight, 314,
        WA_MinHeight, 314,
        WA_DragBar, TRUE,
        WA_DepthGadget, TRUE,
        WA_Activate, TRUE,
        WA_CloseGadget, TRUE,
        WA_SizeGadget, TRUE,
        WA_SimpleRefresh, TRUE,
        WA_NewLookMenus, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | STRINGIDCMP | BUTTONIDCMP | CYCLEIDCMP| IDCMP_MENUPICK,
        WA_PubScreen, (ULONG)screen,
        TAG_DONE);

    if (!window) {
        printf("Failed to open window\n");
        FreeGadgets(glist);
        FreeVisualInfo(vi);
        UnlockPubScreen(NULL, screen);
        CloseFont(font);
        CloseLibrary(SocketBase);
        CloseLibrary(GadToolsBase);
        CloseLibrary((struct Library *)GfxBase);
        CloseLibrary((struct Library *)IntuitionBase);
        return 1;
    }

    /* Draw the status box's empty border immediately, rather than leaving
     * it invisible until the first status line happens to draw it. */
    custom_printf("CLEAR");
    mp_draw_marker_strips();
    mp_draw_sides_hint();

    // Set the initial state of the print mode radio buttons
    struct Gadget *print_mode_gadget = glist;
    while (print_mode_gadget && print_mode_gadget->GadgetID != GAD_PRINT_MODE) {
        print_mode_gadget = print_mode_gadget->NextGadget;
    }
    if (print_mode_gadget) {
        GT_SetGadgetAttrs(print_mode_gadget, window, NULL,
                          GTCY_Active, print_mode,
                          TAG_DONE);
    }

    menu = CreateMenus(menu_template, TAG_DONE);
    if (menu) {
        LayoutMenus(menu, vi,
            GTMN_NewLookMenus, TRUE,           // Enable standard white/grey look
            GTMN_FrontPen, 1,                  // Text pen (usually black)
            GTNM_BackPen, 0,                   // Background pen (usually white)
            TAG_DONE);
        SetMenuStrip(window, menu);
    } else {
        printf("Failed to create menus\n");
    }

    // Refresh window
    GT_RefreshWindow(window, NULL);

    // Offer to install DEVS:Printers/MintPRINT if it is missing.
    check_and_offer_driver_install(window);

    if (load_capability_cache_for_current_endpoint()) {
        apply_cached_capabilities(window);
        printf("Loaded cached printer capabilities\n");
    } else {
        apply_saved_option_state(window);
        /* apply_saved_option_state() itself never logs anything - without
         * this, a startup where the capability cache doesn't match the
         * current host/port/path (deleted, never queried yet, or the
         * endpoint changed) leaves the status box completely silent, which
         * reads as "broken" rather than "nothing cached yet". */
        printf("No cached capabilities for this endpoint - press Query\n");
    }

    /* Refresh live printer state (especially ink/toner levels) on startup
     * when this unit already has a saved endpoint. Reuse exactly the same
     * Query flow as the button so port fallback, capability updates, cache
     * refresh and marker redraw behaviour stay in one place. A new/blank
     * install has an empty ip_buffer and deliberately does nothing here. */
    if (ip_buffer[0]) {
        char startup_ip[64];
        int startup_port = -1;

        if (parse_ip_and_port(ip_buffer, startup_ip,
                              sizeof(startup_ip), &startup_port)) {
            printf("Refreshing saved printer status on startup: %s\n", ip_buffer);
            perform_query_flow_allocated(window, startup_ip, startup_port);
        } else {
            printf("Saved printer address '%s' is invalid - skipping startup Query\n",
                   ip_buffer);
        }
    }

    /* check_and_offer_driver_install() above can pop an EasyRequest on top
     * of this window before the event loop below has even started, so any
     * IDCMP_REFRESHWINDOW that dialog's close generates sits unhandled
     * until process_window_events() gets around to it - by which point
     * the hand-drawn status box (see redraw_output_box()'s comment) may
     * already have been damaged and repainted with nothing in it. One
     * final synchronous repaint here, after all of the above startup
     * activity has settled and immediately before the window is actually
     * shown to the user, doesn't depend on that event ever arriving in
     * time. */
    redraw_output_box();
    mp_draw_marker_strips();
    mp_draw_sides_hint();

    // Process events
    process_window_events(window);

    // Save print mode before exiting
    save_print_mode();

    // Cleanup
    // Ensure operation_in_progress is reset
    operation_in_progress = FALSE;

    // Process any remaining messages in the window's UserPort
    if (window) {
        struct IntuiMessage *imsg;
        while ((imsg = GT_GetIMsg(window->UserPort))) {
            GT_ReplyIMsg(imsg);
        }
    }

    mp_free_sides_hint_images();

    /* Correct GadTools teardown order: first detach menus and close the
     * window, then free gadgets, then release the label backing memory.
     */
    if (window && menu) {
        ClearMenuStrip(window);
    }

    if (window) {
        CloseWindow(window);
        window = NULL;
    }

    if (glist) {
        FreeGadgets(glist);
        glist = NULL;
    }

    /* Cycle gadget labels are static process-lifetime storage. */
    cleanup_dropdown_labels();

    if (menu) {
        FreeMenus(menu);
        menu = NULL;
    }

    /*
     * GadTools VisualInfo belongs to the screen it was obtained from.
     * It must be released while the public-screen lock is still held.
     *
     * The old order did UnlockPubScreen() first and only then called
     * FreeVisualInfo().  That leaves GadTools using screen-related state
     * after our guarantee that the Screen pointer is still valid has gone,
     * and is particularly unfriendly to the classic OS3.1 libraries.
     *
     * Correct lifetime:
     *   FreeGadgets / FreeMenus
     *   FreeVisualInfo
     *   UnlockPubScreen
     */
    if (vi) {
        FreeVisualInfo(vi);
        vi = NULL;
    }

    /* Ink/toner bar pens are held across redraws (see mp_marker_pens[]
     * near mp_draw_marker_strips()) rather than released immediately after
     * drawing, so they must be explicitly freed here too - this is a
     * SHARED public screen, and leaving pens allocated on it past exit
     * would permanently tie up a few of its colour registers. */
    mp_release_marker_pens();

    if (screen) {
        UnlockPubScreen(NULL, screen);
        screen = NULL;
    }

    // Close the font only after GadTools no longer has VisualInfo using it.
    if (font) {
        CloseFont(font);
        font = NULL;
    }

    // Close libraries in reverse order of opening
    mp_clear_printer_icon();
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
    if (GadToolsBase) {
        CloseLibrary(GadToolsBase);
        GadToolsBase = NULL;
    }
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }

    return 0;
}
