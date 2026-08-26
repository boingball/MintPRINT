<h1>
  <img src="art/MintPrintSettings.png" width="48" alt="MintPRINT icon">
  MintPRINT
</h1>

![AmigaOS](https://img.shields.io/badge/AmigaOS-3.0%2B-orange)
![CPU](https://img.shields.io/badge/CPU-m68k-blue)
![Printing](https://img.shields.io/badge/Printing-IPP%20%2F%20AirPrint-0078D4)
![Formats](https://img.shields.io/badge/Formats-PWG%20%7C%20URF%20%7C%20JPEG%20%7C%20PDF%20%7C%20PostScript-purple)
![Discovery](https://img.shields.io/badge/Discovery-mDNS%20%2B%20SSDP-green)
![License](https://img.shields.io/badge/License-MIT-lightgrey)

[![Aminet](https://img.shields.io/badge/Download-Aminet-005CA9)](https://aminet.net/package/driver/print/MintPRINT)
[![Support](https://img.shields.io/badge/Support-Buy%20Me%20a%20Coffee-FFDD00?logo=buymeacoffee&logoColor=000000)](https://buymeacoffee.com/boingball)

![GitHub stars](https://img.shields.io/github/stars/boingball/MintPRINT)
![GitHub last commit](https://img.shields.io/github/last-commit/boingball/MintPRINT)

**Modern network printing for classic Amigas.**

Print directly from normal Amiga applications to modern IPP/AirPrint printers
using PWG Raster, JPEG, PDF or PostScript — no PC print server required.

# MintPRINT

IPP/AirPrint printing for classic AmigaOS - print to modern network
printers (JPEG, PostScript, PWG Raster, or PDF; no driver-specific software
on the printer side) straight from Amiga applications, via a real `DEVS:Printers/`
printer.device driver plus a GUI setup tool.

## What's new in 1.2.2

MintPRINT 1.2.2 switches driver version tracking to a real AmigaOS `$VER:`
string, **driver revision 41.1** - no driver behaviour change, so no
version-number bump for the driver's own build count.

- **Real `$VER:` driver versioning.** `DEVS:Printers/MintPRINT` now embeds
  a standard `$VER: MintPRINT 41.1 (dd.mm.yyyy)` string, readable by
  AmigaOS's own `Version` command and Workbench's Information requester -
  the same convention MintPrint Settings' own `$VER:` line already used.
  This replaces the earlier ad-hoc `MPDRVREV:<n>` marker, which existed
  only for MintPrint Settings' own bundled-vs-installed update check and
  wasn't understood by anything else. The driver's own version now has two
  parts, matching real Amiga version.revision pairs (e.g. Workbench 3.9's
  `47.102`): `MP_DRIVER_REV` (the version half, `driver_core.c`) and
  `MP_DRIVER_SUBREV` (the revision half) - so "driver revision" is never
  just a bare integer that could be misread as printer.device's own fixed
  V44 PrinterSegment ABI marker in the same file. Going forward the
  revision half is what moves for an ordinary driver rebuild - 41.1, 41.2,
  41.3, and so on, the same way a real Amiga library keeps one version
  number across many small revisions - and the version half only bumps for
  something that warrants a new version number outright. MintPrint
  Settings still automatically offers to update an out-of-date installed
  driver at startup (About and Test Print show the same version), now
  parsing and comparing the real `$VER:` version.revision pair instead of
  the old flat counter.

## What's new in 1.2.1

MintPRINT 1.2.1 currently carries **driver revision 41**, adding a new engine
and the following Wordsworth strip-printing fixes:

- **PRT: text/URF and cross-engine margin fixes** (driver rev 41, not yet
  physically re-tested). Two bugs found reviewing revisions 35-40 before
  merge: `PRT:`/`CMD_WRITE` plain-text printing had no `ENGINE=urf` case in
  `command_table.c`'s own engine dispatch, so it silently fell through to
  JPEG and submitted `image/jpeg` - which fails outright on a URF-only
  printer such as the OKI B412 (the whole reason this engine exists).
  Separately, the rev 37-39 top-margin restoration computes and writes
  leading whitespace for *any* `SPECIAL_NOFORMFEED` job regardless of
  engine, but the helper it called (`mp_job_reserve_page()`/
  `mp_job_pad_page()`) only understood PWG and URF, silently writing into
  the unopened PWG encoder instead of whichever encoder a JPEG, PDF or
  PostScript job actually used. Both now dispatch across all five engines.
- **Wordsworth page borders** (driver rev 39). The driver now processes and
  accumulates pre-dump `aIND`, `aNEL` and literal line-feed movement using
  printer.device's 1/216-inch VMI, then emits the corresponding leading white
  raster rows. A rev 37 trace had proved that
  Wordsworth clears margins and sends only a 70-line form length, not its
  separate Print Borders values through the margin commands; rev 38 therefore
  added a form-length/physical-media fallback which restores the documented
  0.50-inch top border (150 rows at 300 DPI) when no real vertical movement or
  explicit `aSTBM` placement appears. The media-height finalizer supplies the
  remaining bottom border.
- **Complete legacy layout diagnostics** (driver rev 40). Debug logs now
  record every printer layout command from `aVERP0` through `aCAM`, both
  parameter slots, live line/VMI/CRLF state, and every raster band's complete
  `IODRPReq` source/destination geometry. This exposes any placement signal
  outside the commands already observed without changing rev 39's output.
- **Wordsworth logical page boundaries** (driver rev 36). A terminal short
  strip now ends the application's printable page before the physical-media
  padding, preventing the first 443 rows of the next page from being pulled
  onto the previous sheet.

- **Apple Raster (`ENGINE=urf`) engine.** A new backend for printers that
  advertise `image/urf` but none of MintPRINT's other formats (JPEG,
  PostScript, PWG Raster, PDF) - reported for the OKI B412 in
  [issue #60](https://github.com/boingball/MintPRINT/issues/60). See
  `docs/URF_ENGINE.md`. One-sided printing confirmed physically (Brother
  MFC-J6930DW, driver rev 30); the OKI B412 report that motivated it is
  still pending its own confirmation.
- **URF duplex** (driver rev 31, three bugs found and fixed by rev 34).
  `ENGINE=urf` now supports two-sided printing the same way PWG Raster
  does - one multi-page stream, submitted once from `DriverClose()` -
  using Apple Raster's own duplex/tumble page header byte. Unlike PWG
  Raster, no backside row/column reversal is performed (Apple Raster's
  page header has no equivalent transform fields); see
  `docs/DUPLEX_PRINTING.md`. Its first three real tests (Brother
  MFC-J6930DW) each hit a real bug rather than confirming duplex itself,
  every one rejected by the printer before any paper came out: first, URF
  had no strip-printing band accumulator, so a strip-printed document
  turned into dozens of bogus single-band "duplex pages" (62 instead of a
  handful) - fixed in rev 32 by extending PWG Raster's own accumulator to
  cover URF; then, once fixed, the two resulting real pages came out at
  different heights (front content naturally overshot the media-derived
  target while the back only got padded to it) - fixed in rev 33 by
  flooring every later page's target at the tallest page the job has
  finalized so far; then, with both pages byte-perfect and an exactly
  matching height, the printer still rejected the job - the duplex/tumble
  byte itself was wrong (1/2/3 for no-duplex/short/long instead of the
  correct 0/1/2, caught by cross-checking a second, independent real-world
  URF reverse-engineering against an HP DesignJet T230) - fixed in rev 34.
  A fourth test on rev 34 hit the same strip-accumulator page-boundary bug
  PWG Raster hit (see the strip-printing fixes below); once revs 35-36
  landed, **two-sided long-edge duplex over `ENGINE=urf` is physically
  confirmed** on this same Brother MFC-J6930DW.
- **Strip-printing page-boundary fixes** (driver revs 35-36). Found
  testing PWG Raster one-sided on the same printer, not URF or duplex
  specifically: a two-page Wordworth document printed with no IPP error at
  all, but visibly corrupted - the page boundary the accumulator chose fell
  in the middle of real content (confirmed by decoding and rendering the
  job file: a character cut off at the top of the second sheet). The
  rev-35 accumulator split at the 3505-row physical A4 boundary, but a real
  retest proved Wordworth's page cycle ends earlier at 3062 rows (thirty
  100-row bands plus a 62-row terminal remainder); rev 35 therefore still
  stole 443 rows from page 2. Rev 36 recognises that short terminal band as
  the logical printable-page boundary, including when it arrives as a narrow
  auxiliary dump, finalises/pads the physical page there, and retains the
  media-height split as a fallback for applications without that signal.
  Affects PWG Raster and URF identically, one-sided or duplex. Revs 37-39
  then restored the top margin that pagination fix had exposed as missing
  (see the Wordsworth page borders bullet above); **a physical retest on
  rev 40 confirmed Wordsworth's PWG Raster page boundaries and margins now
  print correctly**.

## What's new in 1.2.0

MintPRINT 1.2.0 is a substantial driver update, with **driver revision 29**.

- **Much faster JPEG output on classic 68k CPUs.** The JPEG encoder now has
  fast paths for constant 8x8 blocks and complete interior MCUs, avoiding a
  large amount of repeated DCT, quantisation, bounds checking and RGB lookup
  work. This also speeds PostScript jobs because the PostScript backend embeds
  MintPRINT's JPEG output.
- **Plain-text `PRT:` printing.** MintPRINT now handles printer.device
  `CMD_WRITE`/`PRT:` character output as well as graphics dumps. Plain text has
  been tested with AmigaDOS `Type ... TO PRT:` and MultiView, including tabs,
  automatic line wrapping and form-feed page breaks.
- **PostScript Fit respects the printer's printable area.** When a PostScript
  printer reports hardware media margins, `Fit` now keeps the complete page
  inside that imageable rectangle instead of placing borders/content into an
  edge area the printer cannot physically mark. `Fill` keeps its existing
  full-sheet cover/crop behaviour.
- **No regression to the existing graphics path.** The normal MintPrint
  Settings test page and PWG Raster graphics output were re-tested alongside
  the new text path.

PWG Raster remains the preferred engine when a printer supports it: it is much
cheaper to encode on a classic Amiga than JPEG/PDF/PostScript and is therefore
still selected by default where available.

## What's here

- **`driver/`** - `DEVS:Printers/MintPRINT`, the printer.device driver.
  Handles both graphics raster callbacks and plain-text `PRT:`/`CMD_WRITE`
  output, converts the result into streamed JPEG, PostScript, PWG Raster,
  PDF, or Apple Raster (URF) documents, and submits them to the printer's
  IPP `Print-Job` endpoint.
  See `docs/PRINTER_DEVICE_SPIKE.md` (and its follow-ups) for how it was built,
  and `docs/PWG_RASTER.md`/`docs/DRIVER_SPOOL_PROCESS.md` for two of the most
  significant pieces of its design.
- **`src/MintPrintSettings.c`** - MintPrint Settings, the GUI setup/test
  front-end. Discovers printers on the LAN (SSDP + mDNS), queries IPP
  capabilities, supports multiple saved printer profiles (Unit0-7), offers
  capability-gated one-sided/duplex choices, offers to install/update the
  driver, and can send a test page. Its **Help** menu opens
  `docs/MintPrintSettings.guide`, an in-app AmigaGuide walkthrough for new
  users. See `docs/MINTPRINT_PREFS.md`.
- **`windows_ipp_probe.py`** - a small Windows-runnable diagnostic script
  for isolating printer-side vs Amiga-side IPP issues without needing
  Amiga-specific tooling. Useful when reporting a printer MintPRINT
  doesn't work with (see Reporting a problem below).
- **`docs/`** - design notes and build logs for the driver and GUI,
  written as the project went rather than after the fact. The
  **[printer compatibility page](docs/PRINTER_COMPATIBILITY.md)** records
  confirmed hardware, AmigaOS/TCP stack combinations and required settings.
- **`Archive/`, `Binarys/`, `Tools/`** - earlier test programs and
  experiments kept for reference; not part of the current driver/GUI.

## Installing

Run `MintPrintSettings` - it detects a missing or out-of-date
`DEVS:Printers/MintPRINT` and offers to install/update it (copying from
next to itself). **Reboot after any driver install or update** - a driver
segment already resident in memory will not pick up a replaced file until
then. Then open `Prefs/Printer`, select `MintPRINT`, and configure your
printer's IP/host, IPP path, and document format in MintPrint Settings.

## Supported document formats

`image/jpeg`, `application/postscript`, `image/pwg-raster`,
`application/pdf`, and `image/urf` (Apple Raster).
Any IPP Everywhere or AirPrint-certified printer
(most network printers from roughly the last decade) 
is required to accept PWG Raster, so most printers should already
work with that alone. PostScript and PDF cover older or partially-compliant
IPP printers whose network support fronts an existing office-printer
interpreter and which reject raster formats. Apple Raster (URF) covers the
rarer case of a printer that accepts neither - see
`docs/URF_ENGINE.md`.

## Building

Requires `m68k-amigaos-gcc` (Bebbo's cross-toolchain) on `PATH`, or set
`CROSS=` to a different prefix.

    make gui       # MintPrintSettings
    make driver    # build/driver/MintPRINT
    make release   # both, staged into release/MintPRINT/ ready to distribute
    make clean

`make release` does not generate Workbench icons - add
`MintPrintSettings.info` / `MintPRINT.info` inside `release/MintPRINT/`,
and a drawer icon matching the folder's name in its parent directory,
before distributing.

## Reporting a problem

If MintPrint Settings' Query reports that your printer doesn't support any
format MintPRINT can produce, or printing otherwise fails, please
[open an issue](https://github.com/boingball/MintPRINT/issues) and attach
the output of `windows_ipp_probe.py` run against your printer from a
Windows PC on the same network:

    python windows_ipp_probe.py http://<printer-ip>:631/ipp/print --all

`--all` requests every printer attribute and includes a full parsed
attribute dump, which gives much better debugging detail for the issue.

Use the report template on the
[printer compatibility page](docs/PRINTER_COMPATIBILITY.md) so the result can
be added with its AmigaOS version, TCP/IP stack, engine and exact print options.

## Status

MintPRINT is now a real, working app: version **1.2.2** GUI with **driver
revision 41.1**, with multiple printers confirmed fully working over IPP/AirPrint
from real Amiga hardware. It's still actively developed and not every printer
is confirmed yet, so check the
[printer compatibility page](docs/PRINTER_COMPATIBILITY.md) for your specific
model - see `docs/` for open issues and design history.

## License

[MIT](LICENSE) - Copyright (c) 2026 Darren Banfi (boingball).
