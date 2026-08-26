<h1>
  <img src="art/MintPrintSettings.png" width="48" alt="MintPRINT icon">
  MintPRINT
</h1>

![AmigaOS](https://img.shields.io/badge/AmigaOS-3.0%2B-orange)
![CPU](https://img.shields.io/badge/CPU-m68k-blue)
![Printing](https://img.shields.io/badge/Printing-IPP%20%2F%20AirPrint-0078D4)
![Formats](https://img.shields.io/badge/Formats-PWG%20%7C%20JPEG%20%7C%20PDF%20%7C%20PostScript-purple)
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

## What's new in 1.2.1

MintPRINT 1.2.1 bumps to **driver revision 30**, adding a new engine:

- **Apple Raster (`ENGINE=urf`) engine.** A new backend for printers that
  advertise `image/urf` but none of MintPRINT's other formats (JPEG,
  PostScript, PWG Raster, PDF) - reported for the OKI B412 in
  [issue #60](https://github.com/boingball/MintPRINT/issues/60). See
  `docs/URF_ENGINE.md`. Implemented and unit-tested, not yet physically
  test-printed.

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

MintPRINT is now a real, working app: version **1.2.1** GUI with **driver
revision 30**, with multiple printers confirmed fully working over IPP/AirPrint
from real Amiga hardware. It's still actively developed and not every printer
is confirmed yet, so check the
[printer compatibility page](docs/PRINTER_COMPATIBILITY.md) for your specific
model - see `docs/` for open issues and design history.

## License

[MIT](LICENSE) - Copyright (c) 2026 Darren Banfi (boingball).