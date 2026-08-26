# MintPRINT Apple Raster (URF) backend

`DEVS:Printers/MintPRINT` has five document backends, selected by Unit0's
`ENGINE=` (set from MintPrint Settings' **Printer Engine** control):

- `ENGINE=jpeg` (default) - the original, already-proven JPEG path.
- `ENGINE=postscript` - PostScript Level 2 (`application/postscript`), see
  `docs/POSTSCRIPT_ENGINE.md`.
- `ENGINE=pwg-raster` - PWG Raster (`image/pwg-raster`), see
  `docs/PWG_RASTER.md`.
- `ENGINE=pdf` - a minimal single-page PDF (`application/pdf`), see
  `docs/PDF_ENGINE.md`.
- `ENGINE=urf` - streams an Apple Raster (`image/urf`) document instead,
  via `driver/urf_writer.c`.

## Why URF, and why it exists

Apple Raster (also called URF, its MIME subtype and de-facto file
extension) is the raster format AirPrint/driverless printers use as their
own alternative to PWG Raster. Most printers that support one also support
the other, so URF hasn't mattered to MintPRINT so far - `ENGINE=pwg-raster`
already covers that ground. It matters for a printer that advertises **only**
URF among MintPRINT's usable formats: [issue #60](https://github.com/boingball/MintPRINT/issues/60)
reported exactly this for an **OKI B412**, whose `document-format-supported`
is `application/octet-stream, application/vnd.hp-PCL, image/urf` - none of
MintPRINT's four existing engines (JPEG, PostScript, PWG Raster, PDF).

## Format layout

Verified against the CUPS reference implementation (`cups/raster.h`'s
`CUPS_RASTER_SYNCapple` constant and the Apple-mode write path in
`cups/raster-stream.c`) rather than from memory alone - the same standard
`pwg_writer.c` already holds itself to, for the same reason: a byte-layout
mistake here would only surface as garbled physical output with no useful
error message.

```text
offset 0-3:   sync word "UNIR"        (CUPS_RASTER_SYNCapple)
offset 4-7:   "AST" + 0x00            (together: the well-known "UNIRAST\0" magic)
offset 8-11:  page count, big-endian u32 (always 1 - see below)
offset 12-43: 32-byte page header
offset 44...: compressed rows
```

Page header (32 bytes), all multi-byte fields big-endian:

```text
byte 0:      cupsBitsPerPixel   (24 - 8-bit sRGB, chunked RGB)
byte 1:      colorspace         (1 - sRGB)
byte 2:      duplex/tumble mode (1 - simplex; this encoder is single-page only)
byte 3:      print quality      (0 - unspecified)
byte 4:      media type         (0 - unspecified)
byte 5:      media position     (0 - auto)
bytes 6-11:  reserved, zero
bytes 12-15: width in pixels
bytes 16-19: height in pixels
bytes 20-23: resolution (dpi - one figure used for both axes)
bytes 24-31: reserved, zero
```

Row compression is CUPS's shared PackBits-style scheme - the same one
`cups_raster_write()` uses for both its PWG and Apple output modes, byte
for byte. This encoder therefore reuses exactly the "repeat-run-only"
subset `pwg_writer.c` already proved out: a 1-byte "this line's data
appears once" prefix (always `0x00`), then only the "repeat run" half
(control byte 0-127, meaning "next pixel repeated (byte+1) times", 1-128
pixels) - every matching run of identical pixels compresses normally, and
any pixel that does not extend a run is simply emitted as its own trivial
run of one. See `docs/PWG_RASTER.md` for why the format's other, more
failure-prone "literal run" half (control byte 128-255, with its own
minimum-run-length and byte-overflow edge cases) is never needed for a
fully valid, decodable stream.

## Single page only

Unlike PWG Raster's per-page header, Apple Raster declares its total page
count once, up front, in the 12-byte file header - before any page's raster
data exists. That's incompatible with PWG Raster's "grow the page as more
strip-printed bands arrive" accumulator (`mp_pwg_grow()`) and its duplex
"queue several pages, then submit" flow, both of which only know the true
final page count after the fact. `driver/urf_writer.c` therefore starts at
the same single-page scope PDF and PostScript already have in this
codebase (see `driver/driver_core.c`'s single-band engine handling) - no
duplex, no strip-printing accumulation - always writing a page count of 1.
Duplex printing still requires `ENGINE=pwg-raster`, as before.

## Status: implemented, not yet physically test-printed

The byte layout was written and cross-checked against the CUPS reference
implementation field-by-field, and the row compression is the identical,
already-proven algorithm `pwg_writer.c` uses. That gives good confidence
the *bytes on the wire* are shaped correctly. It has not yet been confirmed
against a real printer with actual paper coming out the other end - see
[issue #60](https://github.com/boingball/MintPRINT/issues/60) (OKI B412)
for the report that motivated this engine.

If a URF test print comes out wrong (garbled image, printer error, rejected
job), the most useful next artifact is `T:MintPRINT-job.urf` (kept when
`DEBUG=1`) copied off the Amiga - the first 44 bytes are the file header
plus page header and are worth checking against this file's layout above if
something looks off.

## Build/install/test

    make clean
    make driver

Copy `build/driver/MintPRINT` to `DEVS:Printers/MintPRINT` and reboot (or
otherwise ensure the old driver segment is unloaded) before testing.

In MintPrint Settings, set **Printer Engine** to **Apple Raster** and
**Save** for the unit you want to test with (only Unit0 is live for
printing - see "Multiple printers (Units)" in `docs/MINTPRINT_PREFS.md` if
testing a non-Unit0 profile). A printer that advertises only `image/urf`
(no JPEG/PostScript/PWG Raster/PDF) is auto-selected to this engine after a
Query. Then print as usual (MultiView, GraphicDump, etc.).

Before printing:

    Delete T:MintPRINT-driver.log QUIET
    Delete T:MintPRINT-job.urf QUIET

Expected trace tail on success (compare against the PWG path's equivalent
in `docs/PWG_RASTER.md`):

    MintPRINT: URF begin width/height/scratch <w> <h> <bytes>
    MintPRINT: URF end rows/expected/failed <h> <h> 0
    MintPRINT: IPP result error/http/status 0 200 0

`make test-urf` runs the host-side encoder tests (`tests/test_urf_writer.c`)
covering the file/page header byte layout and the row compression, without
needing AmigaOS hardware.
