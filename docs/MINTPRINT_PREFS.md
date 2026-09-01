# MintPrint Settings

MintPrint Settings (`src/MintPrintSettings.c`, formerly `IPP-Test16.c` /
"MintPRINT Preferences") is the setup and test front-end for
`DEVS:Printers/MintPRINT`.

Startup behaviour:

- Load `ENV:MintPRINT/Unit0` first (the Unit dropdown starts on Unit0).
- Fall back to `ENVARC:MintPRINT/Unit0`.
- Saved media, colour, quality and scaling values are shown before a query.
- If no saved value exists, capability controls show `Not Detected` and are
  ghosted until Query Printer succeeds.
- If `DEVS:Printers/MintPRINT` is missing, offer to install it (see
  "Driver install helper" below).

Layout:

- Unit sits at the top - which saved printer profile is being viewed/edited.
- Query Printer sits beside Printer IPv4.
- Discover sits directly below Query and searches the LAN for printers.
- Printer Engine offers JPEG, PostScript, PWG Raster, and PDF.
- Sides defaults to One-sided and only offers capability-confirmed duplex
  modes after Query Printer.
- Save sits beside Exit.
- Test Print prints the built-in test page through `printer.device`.

## Multiple printers (Units)

People with more than one network printer can keep a separate saved profile
per printer:

    ENV:MintPRINT/Unit0, Unit1, Unit2, ... (up to Unit7)

The **Unit** dropdown at the top of the window lists all eight slots, each
labelled from what is actually saved on disk: `Unit1 - Brother HL-L2350DW`
once a make/model is known and saved, plain `Unit1` if the slot has a saved
profile but no make/model yet, or `Unit1 (empty)` if nothing has been saved
there. Picking a different unit reloads the whole form - IP/host, path,
engine, media, cached capabilities, everything - from that unit's saved
files, exactly like **File > Reload Driver Settings** does for the current
one.

**Only Unit0 is what `DEVS:Printers/MintPRINT` actually reads at print
time** - the driver has no concept of "which unit" it was opened as, so
Unit1+ are switchable *saved profiles*, not simultaneously-active printers.
**Activate**, next to the Unit dropdown, is how a different printer becomes
the one that actually prints: it copies the selected unit's saved
`ENV:`/`ENVARC:` config (and cached capabilities, if any) over Unit0's, then
switches the dropdown back to Unit0 so the window reflects what is now
live. Unit0's own previous settings are overwritten by this - if they are
worth keeping, save them to an empty unit slot first. Activate on Unit0
itself is a no-op (it is already active); on a unit with nothing saved yet
it just reports that there is nothing to copy.

`ENGINE=jpeg`, `ENGINE=postscript`, `ENGINE=pwg-raster`, `ENGINE=urf`, and
`ENGINE=pdf` are persisted by the preferences program and all five are real
driver backends:
`DEVS:Printers/MintPRINT` reads Unit0's `ENGINE=` and produces a JPEG, a
PostScript (`application/postscript`), PWG Raster (`image/pwg-raster`), Apple
Raster (`image/urf`), or a PDF (`application/pdf`) document accordingly. See
`docs/POSTSCRIPT_ENGINE.md`,
`docs/PWG_RASTER.md`, and `docs/PDF_ENGINE.md` for how each encoder works and
what has and hasn't been physically test-printed yet.

The driver reloads Unit0 at the start of every graphics print. Replacing the
printer driver binary itself still requires a reboot before testing it.

## Duplex

Query Printer reads `sides-supported` and `document-format-supported`. The
**Sides** selector is enabled only when the selected engine is PWG Raster, the
printer accepts PWG Raster, and the requested duplex binding is advertised.
MintPRINT then submits one multi-page PWG document with one Print-Job, which
also supports printers that report `multiple-document-jobs-supported=false`.
It also caches `pwg-raster-document-sheet-back` so reverse-side pixels and PWG
header transforms match the printer's native coordinate system. Other engines
remain safely one-sided. See `docs/DUPLEX_PRINTING.md`.

## DPI compatibility option

Query normally fills the **DPI** cycle from the printer's reported
`printer-resolution-supported` and PWG Raster resolution attributes. Some
printers under-report this list: the Canon TS8300 series advertises only 600
DPI but has been physically confirmed to accept and correctly print 300-DPI
PWG Raster jobs.

When PWG Raster or Apple Raster (URF) is selected and a printer reports
resolutions but omits 300 DPI, Settings adds **`300* dpi`** to the cycle. The asterisk means
"compatibility option not reported by the printer". It is deliberately not
written into the capability cache and is not automatically selected for a new
unsaved profile. A saved `RESOLUTION=300` remains selected, allowing a known
working compatibility choice to survive Query and restart. On a URF printer
that advertises only `RS600`, 300 DPI is experimental and can still be rejected
by the printer's raster parser. Other document engines continue to show only
their reported resolution choices.

## LAN printer discovery

Clicking **Discover** runs two passes, each taking about 5 seconds:

1. **SSDP**: a single `M-SEARCH` multicast to `239.255.255.250:1900`,
   catching printers/print servers that answer UPnP discovery.
2. **mDNS**: a DNS PTR query for `_ipp._tcp.local` sent to
   `224.0.0.251:5353` with the "unicast response" bit set, so replies come
   straight back to MintPrint Settings without needing to join the
   multicast group. This is the mechanism most current printers actually
   use to advertise IPP/AirPrint, so it is the pass that matters most in
   practice - SSDP is a bonus for devices that also happen to answer it.

SSDP remains address-only. The mDNS pass now parses the IPP service's DNS-SD
PTR/SRV/TXT records as well: the advertised SRV port and TXT `rp=` resource
path are retained, with `/ipp/print` and port 631 as conservative fallbacks.
If the first PTR response omits the detail records, Settings asks the service
instance directly for SRV and TXT before the discovery window closes. This
matters for older AirPrint printers that do not use the most common endpoint.

Results appear in a small selection window. Picking one and choosing
**Use Selected** fills in Printer IPv4, applies any advertised IPP path/port,
and runs the same capability query as the **Query** button. The fetched
media/colour/quality/scaling values and document formats still come from the
IPP query; discovery only supplies the endpoint needed to reach it.

This is a best-effort LAN scan, not a guarantee: a printer that answers
neither SSDP nor mDNS, or that sits behind a router blocking multicast, will
not appear. If nothing is found, enter the IP manually and use **Query** as
before.

## Make and model

Query Printer now also requests `printer-make-and-model` and logs it (e.g.
`Printer: Brother HL-L2350DW series`). A successful **Save** writes it into
the current unit's file as `MODEL=...`, which is what lets the Unit
dropdown show `Unit0 - Brother HL-L2350DW series` instead of a bare
`Unit0`. Until a unit has been queried and saved at least once, its
dropdown entry just shows the unit number (or `(empty)` if nothing has
been saved there at all).

## Document format reporting

Query Printer now also requests `document-format-supported` and logs the
printer's full advertised list (e.g. `image/jpeg`, `image/pwg-raster`,
`application/pdf`, ...) to the output area. This is informational: the driver
only implements four of those itself (JPEG, PostScript, PWG Raster, and PDF, selected
by `Printer Engine`); anything else in the list is just what the printer
also happens to accept from other clients. If **Save** is pressed with
`Printer Engine` set to a format the most recent query did not see
advertised, a warning is logged (Save still succeeds - this is a
heads-up, not a hard block). If the printer's advertised list contains
**none** of the four formats MintPRINT can produce, a requester points at
filing a GitHub issue with `windows_ipp_probe.py` output attached, since
that printer is not supported yet.

Query also requests the PWG JPEG size and dimension attributes. If a printer
advertises `image/jpeg` but reports none of them, Settings labels that as a
warning: JPEG remains selectable because omission is not proof of failure,
but the printer may silently discard direct JPEG jobs. PostScript is suggested
when the printer advertises it.

## Driver install helper

MintPRINT now ships as a single drawer holding `MintPrintSettings` plus
*both* compiled driver builds under `PROGDIR:Drivers/`:

- `Drivers/MintPRINT-V44/MintPRINT` - AmigaOS 3.2, 3.5, 3.9 (V44+
  printer.device).
- `Drivers/MintPRINT-OS31/MintPRINT` - AmigaOS 3.0, 3.1 (classic pre-V44
  printer.device). See `docs/OS31_SUPPORT.md` for why this second build
  exists at all.

On startup, `mp_needs_os31_driver()` (`src/MintPrintSettings.c`) checks
`mp_os_version()`'s reading of `workbench.library`'s version (falling back
to `SysBase->LibNode.lib_Version` - exec.library/Kickstart - only if
workbench.library can't be opened at all). workbench.library, not
exec.library, is what indicates whether this machine's printer.device
understands the V44 tags: AmigaOS 3.9 and other software-only OS updates
layered on an existing Kickstart ROM commonly leave exec.library's own
version at whatever the ROM shipped with, while workbench.library and the
rest of `LIBS:` get bumped to V44+. `mp_driver_src_path()` then picks the
matching drawer above as the bundled driver source for the rest of this
flow, and `mp_describe_amiga_os()` turns that same version into a friendly
label (e.g. "AmigaOS 3.1") shown in the install/update prompts and the
About box, so the user can see what was detected and why a given driver
was chosen.

If `DEVS:Printers/MintPRINT` does not exist:

1. If the detected driver's bundled copy is also missing (i.e. its
   `Drivers/MintPRINT-<variant>/` drawer isn't present), a note is logged
   and startup continues normally (nothing to offer to install).
2. Otherwise the user is asked whether to install it, with the detected
   AmigaOS version and chosen driver variant named in the prompt. On
   confirmation the driver is copied to `DEVS:Printers/MintPRINT`.
3. On success, the user is asked whether to open Printer preferences now.
   Confirming launches `SYS:Prefs/Printer` (`SystemTags(..., SYS_Asynch,
   TRUE, ...)`) so they can select **MintPRINT** as their printer driver and
   save.

If `DEVS:Printers/MintPRINT` already exists, the same detection picks which
bundled copy to compare against it, and the existing newer-version check
(comparing `$VER: MintPRINT` strings) decides whether to offer an update.

This only ever offers to *install* the driver; selecting it in Printer
preferences and saving remains a manual step the user must do themselves; is
not something Preferences can do automatically.

A standalone `Install` script (classic AmigaDOS Installer format) at the
repository root does the equivalent auto-detect-then-choose flow for users
who prefer that install path over running MintPrintSettings first; see the
comments at the top of that script.

## Capability cache

After a successful **Query Printer**, MintPRINT writes the detected printer
capabilities for whichever unit is currently selected to:

    ENV:MintPRINT/UnitN.cache
    ENVARC:MintPRINT/UnitN.cache

(`N` is the selected unit's number - `Unit0.cache`, `Unit1.cache`, ...) The
cache contains the available media/tray mappings, colour modes, quality
levels, scaling and sides choices, detected document formats and other IPP
values. Each unit's own config file remains what stores its selected
defaults.

When a unit is loaded (at startup, via the Unit dropdown, or File > Reload),
its cache is used only if its HOST, PORT and PATH match that unit's current
endpoint. A successful new query replaces both of that unit's cache files,
so the UI always uses the newest detected capability set.

The status/output area now starts below the Test Print / Save / Exit row and the
preferences window is taller so those controls no longer overlap the log box.

## Help menu

**Help > MintPrint Settings Help...** opens `MintPrintSettings.guide` (an
AmigaGuide document, shipped next to the program in the release bundle) in
Multiview. It covers the same ground as this document, written for someone
using the program for the first time: discovery, Query, engines, the DPI
compatibility option, duplex, Units, the driver install helper, Test Print,
and reporting a problem. See `docs/MintPrintSettings.guide` for the source.
