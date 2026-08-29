#ifndef MINTPRINT_CONFIG_H
#define MINTPRINT_CONFIG_H

#include <exec/types.h>

#define MP_CONFIG_HOST_MAX 64
#define MP_CONFIG_PATH_MAX 96
#define MP_CONFIG_OPTION_MAX 64
#define MP_CONFIG_ENGINE_MAX 16

#define MP_CONFIG_SOURCE_DEFAULTS 0
#define MP_CONFIG_SOURCE_ENV      1
#define MP_CONFIG_SOURCE_ENVARC   2

/* Drawer name job files spool into on a real hard drive Spooler location
 * (never used for "RAM" - that keeps spooling flat under T: exactly as
 * before). MintPrint Settings creates it hidden, with no .info icon, the
 * first time a device is saved as SPOOL= (mp_ensure_hidden_spool_dir() in
 * src/MintPrintSettings.c) - the driver itself only ever builds paths
 * into it, never creates it. */
#define MP_SPOOL_DIR_NAME "MPSPOOL"

struct MPConfig {
    char host[MP_CONFIG_HOST_MAX];
    UWORD port;
    char path[MP_CONFIG_PATH_MAX];
    BOOL debug;
    UWORD resolution; /* capture DPI: 300 or 600, see RESOLUTION= */
    char engine[MP_CONFIG_ENGINE_MAX];
    char media[MP_CONFIG_OPTION_MAX];
    char source[MP_CONFIG_OPTION_MAX];
    char color[MP_CONFIG_OPTION_MAX];
    char quality[MP_CONFIG_OPTION_MAX];
    char scaling[MP_CONFIG_OPTION_MAX];
    char sides[MP_CONFIG_OPTION_MAX];
    char pwg_sheet_back[MP_CONFIG_OPTION_MAX];
    /* Where job files spool: "RAM" (default - whatever T: is assigned to,
     * normally RAM: on a stock system) or a real device name such as
     * "DH0:" for memory-tight systems where even T:'s usual RAM: backing
     * is scarce. See SPOOL= below and driver_core.c's mp_build_spool_paths(). */
    char spool[MP_CONFIG_OPTION_MAX];
    /* Keep every hard-drive-spooled job under a unique timestamped name
     * (rather than the fixed one job file reused/overwritten by every
     * page) for the Spooler management window to list, inspect, retry or
     * reprint later. Only meaningful when spool[] names a real device -
     * RAM keeps its original single-fixed-name behaviour regardless of
     * this flag (see driver_core.c's mp_job_begin()). See SPOOL_KEEP=
     * below. */
    BOOL spool_keep;
    /* IPP media-*-margin values, in hundredths of a millimetre. */
    ULONG margin_left_100mm;
    ULONG margin_right_100mm;
    ULONG margin_top_100mm;
    ULONG margin_bottom_100mm;
};

void mp_config_defaults(struct MPConfig *cfg);
LONG mp_config_load(struct MPConfig *cfg);

#endif
