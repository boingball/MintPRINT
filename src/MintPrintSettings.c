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

/* Visible both to AmigaOS's Version command and in the Abo