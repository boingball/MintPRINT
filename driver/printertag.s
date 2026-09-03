/*
 * MintPRINT printer.device driver segment tag.
 *
 * The AmigaDOS loader supplies ps_NextSegment before the bytes below.
 * The first four bytes here are ps_runAlert (MOVEQ #0,D0 / RTS), followed
 * by the printer segment version/revision and PrinterExtendedData.
 *
 * Numeric constants used here are from devices/prtbase.h:
 *   PPC_COLORGFX  = 0x03
 *   PPCF_EXTENDED = 0x04
 *   PCC_YMCB      = 0x04
 */

        .section .text
        .even
        .globl  _start
        .globl  _PEDData

        .extern _Init
        .extern _Expunge
        .extern _TextDriverOpen
        .extern _TextDriverClose
        .extern _CommandTable
        .extern _TextDoSpecial
        .extern _Render
        .extern _MintPRINTCompatRender
        .extern _ConvFunc
        .extern _DriverTags

_start:
        /* MOVEQ #0,D0 ; RTS -- safe if somebody tries to execute driver */
        .byte   0x70,0x00,0x4e,0x75

        /* PrinterSegment version/revision. Version must stay 44 - it is
         * printer.device's own ABI marker for the V44 extended PED tags
         * this driver uses, not a MintPRINT project number. Revision is
         * unused by this project (there is no reliable fixed byte offset
         * to read it back at from the raw file on disk - see
         * mp_driver_version_marker below for why, and for this project's
         * own version/revision marker). */
        .word   44
        .word   1

_PEDData:
        .long   printerName
        .long   _Init
        .long   _Expunge
        .long   _TextDriverOpen
        .long   _TextDriverClose

        /* PrinterClass = PPC_COLORGFX | PPCF_EXTENDED */
        .byte   0x07
        .byte   0x04              /* ColorClass = PCC_YMCB */
        .byte   136               /* MaxColumns */
        .byte   0                 /* NumCharSets */
        .word   1                 /* NumRows: one raster row per cycle */
        .long   4096              /* MaxXDots */
        .long   6144              /* MaxYDots */
        .word   300               /* XDotsInch */
        .word   300               /* YDotsInch */
        .long   _CommandTable
        .long   _TextDoSpecial
        .long   _MintPRINTCompatRender
        .long   30                /* timeout seconds */
        .long   0                 /* ped_8BitChars: use system default */
        .long   0                 /* ped_PrintMode */
        .long   _ConvFunc         /* capture PRT:/CMD_WRITE characters */

        /* V44 extended fields */
        .long   _DriverTags
        .long   0                 /* ped_DoPreferences */
        .long   0                 /* ped_CallErrHook */

printerName:
        .asciz  "MintPRINT"
        .even

/*
 * This project's own driver build version, for MintPrint Settings' own
 * update-detection and for AmigaOS's own "Version" command / Workbench
 * Information requester - NOT read by printer.device itself.
 *
 * The compiled driver FILE on disk is a standard AmigaDOS hunk-format load
 * module (HUNK_HEADER followed by HUNK_CODE/HUNK_DATA/... hunks, same as
 * any other linked Amiga program - "-nostartfiles" only omits the C
 * runtime startup code, it does not change the container format), not a
 * raw blob starting at _start. There is no reliable FIXED BYTE OFFSET for
 * anything in that raw file: the hunk header's own size varies (resident
 * library name list, hunk count/sizes), so a byte offset that happens to
 * land on this version word in one build can land somewhere else
 * entirely in the next. A scannable ASCII marker sidesteps that
 * completely - a standard "$VER:" string is exactly that trick, so this
 * doubles as both the update-detection marker (previously the ad-hoc
 * "MPDRVREV:" prefix) and something real Amiga tooling already knows how
 * to read.
 *
 * version.revision, not a flat build counter - see driver_core.c's
 * MP_DRIVER_REV (version) / MP_DRIVER_SUBREV (revision). Never a bare
 * number on its own, so it can't be misread as printer.device's own
 * fixed V44 PrinterSegment ABI marker above. As of 1.2.2, the revision
 * half is what moves for an ordinary rebuild - 41.1, 41.2, 41.3, and so
 * on - the same way a real Amiga library keeps one version number
 * across many small revisions; the version half only bumps for
 * something that warrants a new version number outright.
 */
mp_driver_version_marker:
        .asciz  "$VER: MintPRINT 41.17 (03.09.2026)"
        .even
