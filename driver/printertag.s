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
         * MP_DRIVER_REV_MARKER below for why, and for this project's own
         * build-counter marker). */
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
        .long   _Render
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
 * This project's own driver build-counter, for MintPrint Settings' own
 * update-detection - NOT read by printer.device.
 *
 * The compiled driver FILE on disk is a standard AmigaDOS hunk-format load
 * module (HUNK_HEADER followed by HUNK_CODE/HUNK_DATA/... hunks, same as
 * any other linked Amiga program - "-nostartfiles" only omits the C
 * runtime startup code, it does not change the container format), not a
 * raw blob starting at _start. There is no reliable FIXED BYTE OFFSET for
 * anything in that raw file: the hunk header's own size varies (resident
 * library name list, hunk count/sizes), so a byte offset that happens to
 * land on this revision word in one build can land somewhere else
 * entirely in the next. A scannable ASCII marker sidesteps that
 * completely - it only needs to appear ANYWHERE in the file's bytes, the
 * same approach AmigaOS's own "Version" command uses for "$VER:" strings.
 * Bump the trailing number whenever a driver rebuild actually changes
 * behaviour - a config-only or GUI-only change does not need a bump.
 *
 */
mp_driver_revision_marker:
        .asciz  "MPDRVREV:33"
        .even
