<h1>
  <img src="art/MintPrintSettings.png" width="48" alt="MintPRINT icon">
  MintPRINT
</h1>

![AmigaOS](https://img.shields.io/badge/AmigaOS-2.04%2B-orange)
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
using PWG Raster, Apple Raster, JPEG, PDF or PostScript — no PC print server
required.

MintPRINT includes a real `DEVS:Printers/` printer.device driver and
MintPrint Settings for setup, discovery and test printing.

[Installation](#installing) · [Printer compatibility](docs/PRINTER_COMPATIBILITY.md) · [Release history](CHANGELOG.md)

<img width="525" height="327" alt="image" src="https://github.com/user-attachments/assets/ba4cfd1f-8b0f-4aee-91b9-a6b221712ca5" />


## What's new in 1.3.1

- **Final driver revision 41.17.**
- **Query results now save correctly:** media/tray, colour, quality and scaling
  choices selected by **Query Printer** are synchronised with the Unit config,
  including on the AmigaOS 2.x-compatible event path.
- **Failed RAM-spooled jobs are recoverable:** **View Spooler** shows the
  current RAM/T: job and keeps a failed rendered document available for Retry,
  Copies or Delete. Successful RAM jobs are still removed automatically.

[Full release history and technical details](CHANGELOG.md)

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
  capability-gated one-sided/duplex choices, detects this machine's AmigaOS/
  printer.device generation to pick the matching bundled driver, offers to
  install/update it, and can send a test page. Its **Help** menu opens
  `docs/MintPrintSettings.guide`, an in-app AmigaGuide walkthrough for new
  users. See `docs/MINTPRINT_PREFS.md`.
- **`Install`** - a classic AmigaDOS Installer script, an alternative
  install path to running `MintPrintSettings` directly. Does the same
  AmigaOS-version detection and lets you confirm or override the driver
  choice before copying anything.
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

## Requirements and memory

- Motorola 68000 or later, AmigaOS 2.04+, and a TCP/IP stack providing
  `bsdsocket.library`.
- A network printer supporting IPP and one of the document formats below.
- With RAM/T: spooling, allow roughly **2 MB of free RAM** for a full-page
  print. HDD spooling can make a **1 MB system** practical, provided enough
  contiguous memory remains for the GUI, row buffers, encoder and TCP/IP
  stack; it is not a guarantee for every printer or resolution.
- HDD spool storage is limited by free disk space and normal filesystem
  limits. Retained jobs live in the selected drive's hidden `MPSPOOL` drawer.

## Installing

MintPRINT ships as one drawer (`MintPRINT/`) containing `MintPrintSettings`
and both driver builds under `Drivers/` - `Drivers/MintPRINT-V44/` for
AmigaOS 3.2, 3.5, 3.9 (and later), `Drivers/MintPRINT-OS31/` for the
classic pre-V44 AmigaOS 2.04 through 3.1 build (see `docs/OS31_SUPPORT.md`).

Run `MintPrintSettings` - it detects which driver this machine needs from
workbench.library's version, tells you what it found, and offers to
install/update a missing or out-of-date `DEVS:Printers/MintPRINT` from the
matching `Drivers/` subdrawer. **Reboot after any driver install or
update** - a driver segment already resident in memory will not pick up a
replaced file until then. Then open `Prefs/Printer`, select `MintPRINT`,
and configure your printer's numeric IPv4 address, IPP path, and document
format in MintPrint Settings.

Prefer a classic Amiga install experience instead? Run the `Install`
script at the top of the archive - it offers the same auto-detected driver
choice (with the option to override it) through the standard AmigaOS
Installer.

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
    make driver    # build/driver/MintPRINT (V44+ build)
    make driver31  # build/driver31/MintPRINT (AmigaOS 2.04-3.1 classic build)
    make release   # all three, staged into release/MintPRINT/ ready to distribute
    make clean

`make release` stages one drawer with both driver builds under
`Drivers/MintPRINT-V44/` and `Drivers/MintPRINT-OS31/`, plus the `Install`
script and Aminet readme next to it. See `docs/OS31_SUPPORT.md` for the two
driver builds and `mp_driver_src_path()` in `src/MintPrintSettings.c` for
how the right one gets chosen at runtime. Workbench icons for
`MintPrintSettings` and the release drawer itself are copied in
automatically from `art/` if present there.

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

MintPRINT has multiple printers confirmed working over IPP/AirPrint from
real Amiga hardware. It's still actively developed and not every printer
is confirmed yet, so check the
[printer compatibility page](docs/PRINTER_COMPATIBILITY.md) for your specific
model - see `docs/` for open issues and design history.

## Credits

MintPRINT is by Darren Banfi (boingball), developed with assistance from
Anthropic Claude.

Special thanks to **Andreas Stürmer**, author of
[AmiAirPrint](https://github.com/Andiweli/AmiAirprint), for friendly
cross-project collaboration: sharing compatibility findings and recent fixes,
allowing MintPRINT's implementation to be compared against AmiAirPrint, and
helping identify gaps in classic application and printer compatibility.

## License

[MIT](LICENSE) - Copyright (c) 2026 Darren Banfi (boingball).
