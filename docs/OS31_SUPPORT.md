# MintPRINT on AmigaOS 3.1

MintPRINT now has a separate **classic printer.device build** intended for
AmigaOS 3.1 / printer.device V40.

This is deliberately a separate driver binary rather than trying to make the
normal V44+ driver lie about the ABI it uses.

## Why a second driver is needed

The normal MintPRINT driver uses the extended V44 printer-driver interface:

- `PPCF_EXTENDED`
- `PRTA_NoIO`
- `PRTA_8BitGuns`
- the V44 extended fields at the end of `PrinterExtendedData`

Those interfaces post-date AmigaOS 3.1.

The OS3.1 build instead uses the original pre-V44 PrinterExtendedData layout
and advertises only `PPC_COLORGFX`.  There is no extended tag list.

## Colour input

The V44 build requests 8-bit gun values from printer.device.

Classic printer.device supplies its traditional 4-bit Y/M/C/B intensity
values.  `driver/classic_render_shim.c` expands each component from 0..15 to
0..255 (`n * 17`) before handing the row to the normal MintPRINT renderer.

From that point onward the JPEG, PostScript, PWG Raster, PDF, spool and IPP code is shared
with the normal driver.

## Network requirement

AmigaOS 3.1 does not itself provide the TCP/IP socket environment MintPRINT
needs.

The machine therefore needs a working `bsdsocket.library`-compatible TCP/IP
stack such as Roadshow, AmiTCP, Miami, or another compatible stack.

MintPrint Settings requires `bsdsocket.library` V4 and verifies that it can
create a socket before opening the main window. If that check fails it shows a
requester and exits without installing or changing the driver. The driver
performs the same check during `Init()`, before printer.device can begin sending
raster rows.

## Build

    make clean
    make driver31

The result is:

    build/driver31/MintPRINT

Install that file as:

    DEVS:Printers/MintPRINT

Then select MintPRINT in Printer Preferences and reboot before the first test.

## Release bundle

For release builds use:

    make release

This stages a single distributable drawer:

    release/MintPRINT/

containing `MintPrintSettings` and both driver builds under `Drivers/`:

- `Drivers/MintPRINT-V44/MintPRINT` - V44+ driver for AmigaOS 3.2, 3.5, 3.9
  and later.
- `Drivers/MintPRINT-OS31/MintPRINT` - classic pre-V44 driver for AmigaOS
  3.0/3.1.

`mp_needs_os31_driver()` in `src/MintPrintSettings.c` checks
`SysBase->LibNode.lib_Version` at runtime and `mp_driver_src_path()` picks
whichever `Drivers/MintPRINT-<variant>/MintPRINT` matches, so there is no
longer a separate bundle to choose before downloading - one archive covers
every supported AmigaOS release, and MintPrint Settings tells the user
which driver it picked (and why) before installing it. The `Install`
script at the repository root offers the same auto-detect-then-confirm
flow for anyone installing without running MintPrintSettings first.

## Important: test status

This compatibility layer is structurally based on the documented classic
printer.device ABI, but it must be considered **experimental until it has
actually produced a physical/test print on an OS3.1 system**.

Before public release, test at minimum:

1. MintPrint Settings -> Test Print on OS3.1.
2. MultiView or GraphicDump graphics printing.
3. JPEG engine.
4. PWG Raster and PDF where the target printer advertises them.
5. Reboot/install/update cycle.
6. A real bsdsocket.library stack used by OS3.1 (not only WinUAE networking).

If the OS3.1 printer transport opens Parallel/Serial even though MintPRINT
never calls `PD->pd_PWrite`, that is the first compatibility point to inspect.
Unlike the V44 build, classic printer.device has no `PRTA_NoIO` tag.
