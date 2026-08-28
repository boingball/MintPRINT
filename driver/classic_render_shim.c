/*
 * MintPRINT classic printer.device Render entry point.
 *
 * driver_core.c is compiled as MintPRINT_RenderCore for the pre-V44 build,
 * leaving this file to export the _Render symbol required by the classic
 * PrinterExtendedData ABI.
 *
 * Earlier versions copied every raster row into a second allocation merely
 * to expand printer.device's 4-bit Y/M/C/B guns to 8 bits. On memory-tight
 * AmigaOS 2.04 that allocation returned PDERR_BUFFERMEMORY (7) before row
 * zero, so printer.device closed the dump with ct=7 and nothing printed.
 *
 * The shared renderer now expands classic nibbles as it consumes them when
 * built with MINTPRINT_CLASSIC_GUNS. This wrapper therefore has no allocation,
 * no row copy and no additional failure path.
 */

#include <exec/types.h>
#include <devices/printer.h>
#include <devices/prtbase.h>

LONG PRT_STDARGS MintPRINT_RenderCore(LONG ct, LONG x, LONG y,
                                      LONG status, ...);

LONG PRT_STDARGS Render(LONG ct, LONG x, LONG y, LONG status, ...)
{
    return MintPRINT_RenderCore(ct, x, y, status);
}
