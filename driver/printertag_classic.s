/*
 * MintPRINT classic printer.device segment tag for AmigaOS 3.1.
 *
 * This deliberately uses the original pre-V44 PrinterExtendedData layout.
 * In particular it does NOT set PPCF_EXTENDED and does NOT append a
 * ped_TagList/ped_DoPreferences/ped_CallErrHook tail.
 *
 * That means printer.device V40 (AmigaOS 3.1) never sees the later
 * PRTA_NoIO / PRTA_8BitGuns interface used by the normal MintPRINT driver.
 *
 * The matching classic_render_shim.c expands the classic printer.device
 * 4-bit Y/M/C/B intensity values to the 8-bit values expected by MintPRINT's
 * existing document encoders.
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

_start:
        /* MOVEQ #0,D0 ; RTS -- standard printer driver run-alert stub. */
        .byte   0x70,0x00,0x4e,0x75

        /*
         * Version 35 is the canonical classic graphics-driver ABI used by
         * Commodore's own example drivers; anything below V44 uses the
         * original PED layout.
         */
        .word   35
        .word   1

_PEDData:
        .long   printerName
        .long   _Init
        .long   _Expunge
        .long   _TextDriverOpen
        .long   _TextDriverClose

        /* PrinterClass = PPC_COLORGFX (NO PPCF_EXTENDED). */
        .byte   0x03
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
        .long   0                 /* ped_8BitChars: system default */
        .long   0                 /* ped_PrintMode */
        .long   _ConvFunc         /* suppress primitive I/O and capture PRT: */

        /* STOP HERE: V44 extended PED fields must not exist in this build. */

printerName:
        .asciz  "MintPRINT"
        .even

/*
 * Same update marker convention as the main driver.  This is kept primarily
 * for diagnostics; the OS3.1 release is packaged separately so Settings sees
 * the correct PROGDIR:MintPRINT binary for that release.
 *
 */
mp_driver_revision_marker:
        .asciz  "MPDRVREV:34"
        .even

/* Human-readable marker useful when inspecting a built driver. */
mp_driver_abi_marker:
        .asciz  "MPDRVABI:OS31"
        .even
