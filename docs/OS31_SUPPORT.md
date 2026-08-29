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

## Memory preflight check

`mp_job_begin()` (`driver/driver_core.c`) now checks `AvailMem(MEMF_LARGEST |
MEMF_PUBLIC)` against the combined size of the per-row RGB buffer and the
active encoder's scratch buffer *before* calling `AllocMem()` for either one,
and fails the page immediately (`PDERR_BUFFERMEMORY`, logged as "Job begin
rejected: insufficient memory needed/free/width" when debug logging is on)
if the largest free block can't satisfy both. Both buffers scale with page
width, not width x height - MintPRINT streams rows to the spool file rather
than buffering a whole page in memory - so the actual requirement is modest
(low hundreds of KB at 300dpi for a portrait page) but still needs to exist
as one contiguous free block, which matters on a fragmented, memory-tight
system. This replaces silently attempting the allocation and failing deep
into a print with a single clear rejection up front.

This check only covers the small per-row/scratch buffers, which always
live in RAM regardless of configuration. The *encoded job file itself*
(the full rendered JPEG/PWG Raster/PDF/PostScript/URF document, which can
run to several MB) is a separate concern, covered by the Spooler option
below.

## Spooler: RAM vs. hard drive

MintPRINT has always spooled its rendered job file to `T:` before
submitting it over IPP. On a stock system `T:` is assigned to `RAM:`, so
that file - potentially several MB for a full-page raster - was always
competing with everything else for the same scarce RAM on a memory-tight
system like the one above. MintPrint Settings' Spooler gadget now offers
an alternative: any `DHn:`-named hard drive device it finds mounted
(`RAM` stays the default - see `SPOOL=` in `docs/MINTPRINT_CONFIG.md`).
Choosing a hard drive moves that entire file off RAM and onto disk,
independent of the small preflight-checked buffers above - the two
address different parts of the same low-memory problem.

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
`mp_os_version()` (workbench.library's version, not exec.library's - see
that function's comment for why: AmigaOS 3.9 and similar software-only OS
updates can leave exec.library's own version at whatever the Kickstart ROM
shipped with) at runtime, and `mp_driver_src_path()` picks whichever
`Drivers/MintPRINT-<variant>/MintPRINT` matches, so there is no
longer a separate bundle to choose before downloading - one archive covers
every supported AmigaOS release, and MintPrint Settings tells the user
which driver it picked (and why) before installing it. The `Install`
script at the repository root offers the same auto-detect-then-confirm
flow for anyone installing without running MintPrintSettings first.

## AmigaOS 2.0/2.04 (experimental, unconfirmed)

The classic driver's own printer.device interface (pre-V44
`PrinterExtendedData`, `PPC_COLORGFX` only) is the same interface that's
existed since well before AmigaOS 3.1, and `driver/driver_core.c` and
`driver/command_table.c` already only ever `OpenLibrary()` `dos.library` and
`graphics.library` at v37 - AmigaOS 2.04, where `gadtools.library` (and this
driver's own minimum) was introduced. So the driver side of this build has
never actually required 3.0/3.1 specifically.

**MintPrint Settings** (the GUI) did, though: it pinned
`intuition.library`/`graphics.library`/`gadtools.library` at v39 (AmigaOS
3.0) even though nothing else in it needed more than v37, until that pin was
lowered as an experiment. The one real v39-only call it made,
`ObtainPen()`/`ObtainBestPenA()` (graphics.library's shared-pen allocator,
used only for the printer-status ink/toner strip's marker-colour fill), is
now skipped on a sub-v39 `graphics.library` - that strip just shows no fill
on such a system rather than calling an entry point that doesn't exist.

**First physical test on real AmigaOS 2.04 (v37)**: MintPrint Settings built
and ran, and the OS31 driver installed and completed a Test Print - after
several minutes, on a system with roughly 2MB of RAM (the practical minimum
observed so far; see the memory preflight check below). The printed page was
not centered - content was pushed toward the left edge rather than centred on
the page, the same symptom class as the pre-fix V44 DUMPRPORT geometry quirk
described in `mp_job_write_row()` (`driver/driver_core.c`) and in
`mintprint_test_page()`'s own history (`src/MintPrintSettings.c`). That
comment's fix (the `g_recenter_clamped_page` clamp, keyed off how far
printer.device's reported page width diverges from the configured media's
expected width) is shared, unmodified code between both driver builds, so it
is already compiled into the OS31/2.04 build too - it just may not be
tripping the same way on this older printer.device's own reported geometry.
Root-causing this precisely needs the actual numbers from a debug-enabled
run (`Debug: on` in MintPrint Settings, then `T:MintPRINT-driver.log` off the
test machine) - in particular the "Render begin width/height", "Clamping
oversized page width", and "Row xpos printer/used/scaled" lines - rather than
guessing at a fix that might only patch this one case and regress the
already-confirmed V44 behaviour.

Building and running MintPrint Settings itself successfully on a v37 system
is confirmed; the driver binary's own behaviour there is the second,
separate question, now underway per the above.

## Test status

**Physically confirmed on a real AmigaOS 3.1 system**: MintPrint Settings
runs, detects this build's driver as the correct choice, installs it,
discovers/queries a live network printer over IPP, and a Test Print
completes successfully with real paper output.

Still worth confirming as testing continues:

1. MultiView or GraphicDump graphics printing (beyond Test Print).
2. JPEG engine specifically (confirm which engine the successful test used
   if it wasn't JPEG).
3. PWG Raster and PDF where the target printer advertises them.
4. A second reboot/install/update cycle (updating an already-installed
   driver, not just a first install).
5. A real bsdsocket.library stack other than the one already tested.

If the OS3.1 printer transport opens Parallel/Serial even though MintPRINT
never calls `PD->pd_PWrite`, that is the first compatibility point to inspect.
Unlike the V44 build, classic printer.device has no `PRTA_NoIO` tag.
