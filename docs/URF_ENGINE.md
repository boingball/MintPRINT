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
offset 8-11:  page count, big-endian u32 - written once, with the first page
offset 12-43: 32-byte page header - repeated per page
offset 44...: compressed rows, then another page header + rows per
              subsequent page (duplex)
```

Page header (32 bytes), all multi-byte fields big-endian:

```text
byte 0:      cupsBitsPerPixel   (24 - 8-bit sRGB, chunked RGB)
byte 1:      colorspace         (1 - sRGB)
byte 2:      duplex/tumble mode (1 = simplex, 2 = duplex-tumble
             [two-sided-short-edge], 3 = duplex-no-tumble [two-sided-long-edge])
byte 3:      print quality      (0 - unspecified)
byte 4:      media type         (0 - unspecified)
byte 5:      media position     (0 - auto)
bytes 6-11:  reserved, zero
bytes 12-15: width in pixels
bytes 16-19: height in pixels
bytes 20-23: resolution (dpi - one figure used for both axes)
bytes 24-31: reserved, zero
```

Unlike PWG Raster's page header, Apple Raster's compact 32-byte header has
no CrossFeedTransform/FeedTransform-equivalent fields for describing a
pre-mirrored backside - only the single duplex/tumble byte above, which
doesn't distinguish front from back. There is therefore nothing for a
sender to pre-flip: `driver/urf_writer.c` streams every page's rows in the
same natural top-to-bottom, left-to-right order regardless of side,
trusting the printer's own duplex mechanism to orient the backside
correctly from the duplex/tumble hint alone. **This is the main assumption
worth checking on an actual physical duplex print** - if a backside comes
out upside-down or mirrored, that is where to look first (compare against
`mp_pwg_backside_transform()` in `pwg_writer.c`, which PWG Raster needs
precisely because its page header *does* have those transform fields).

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

## Duplex and strip-printing accumulation

Unlike PWG Raster's per-page header, Apple Raster declares its total page
count once, up front, in the 12-byte file header - before any page's raster
data exists. `driver/urf_writer.c` handles that the same way
`driver_core.c` already handles PWG's post-hoc height patch for
strip-printed pages: a duplex job writes an honest "unknown/streaming"
placeholder (`0xffffffff`, the same sentinel CUPS itself uses) for the page
count when the first page begins, then `DriverClose()` patches in the true
total - via `mp_spool_job_patch()`, the same primitive PWG's own
`mp_page_finalize()` already uses - once the last page is known, just
before closing the file.

`ENGINE=urf` duplex is otherwise wired exactly like PWG Raster's: pages are
appended to one open file across several `Render(0)/.../Render(4)` cycles
within the same `DriverOpen()`/`DriverClose()` bracket
(`mp_duplex_requested()` gates this for both engines identically), and the
whole multi-page file is submitted as a single IPP Print-Job once complete.
`sides=` in MintPrint Settings offers Long edge/Short edge whenever the
printer advertises them **and** the selected engine's own document format
(`image/urf` here) - see `mp_duplex_transport_supported()`.

**Strip-printing accumulation is also implemented for URF**, using the same
growable-declared-height contract PWG Raster's `mp_pwg_grow()` already
offers: `mp_urf_grow()` raises a page's declared height as more
`SPECIAL_NOFORMFEED` bands arrive (e.g. Wordworth's output splitting one
physical page into several same-width bands), and `mp_page_finalize()`
patches the page header's height field with the true total once the whole
page is known - see `MP_URF_HEIGHT_FIELD_OFFSET`. Tiny leading/auxiliary
NOFORMFEED bands (positioning artifacts some applications emit) are
discarded the same way for both engines too
(`mp_is_tiny_leading_auxiliary_band()`/`mp_is_tiny_auxiliary_band()`).

This was **not** true in URF's first duplex build (driver revision 31): a
NOFORMFEED band under `ENGINE=urf` finalized immediately as its own page,
same as JPEG/PDF/PostScript. Combined with duplex, that turned one
strip-printed physical page into dozens of bogus single-band "duplex
pages" queued into one file - confirmed on a real Brother MFC-J6930DW
duplex test, where 62 pages got queued instead of the true handful, and
the printer rejected the job (`server-error-job-canceled`). Driver
revision 32 fixes this by extending the same accumulator PWG Raster
already had to also cover URF - see `mp_engine_supports_strip_accumulation()`
in `driver_core.c`.

## Status: confirmed physically printing

The byte layout was written and cross-checked against the CUPS reference
implementation field-by-field, and the row compression is the identical,
already-proven algorithm `pwg_writer.c` uses. Driver revision 30 has since
printed a full A4 (2480x3508 @ 300dpi) test page correctly on a **Brother
MFC-J6930DW** (`ENGINE=urf` is not that printer's default - it also
advertises PWG Raster - but was selected explicitly to exercise this
engine); the driver log showed `URF end rows/expected/failed 3508 3508 0`
and `IPP result error/http/status 0 200 0`, and the retained
`T:MintPRINT-job.urf` decodes byte-for-byte to the expected header fields
and 3508 valid rows with no corruption. See
[issue #60](https://github.com/boingball/MintPRINT/issues/60) for the
**OKI B412** report that motivated this engine - that printer's own
confirmation is still pending, since it wasn't the hardware used for the
test above.

That confirmed test was one-sided. Duplex support (see above) is new; its
first real test (Brother MFC-J6930DW, driver revision 31) surfaced the
strip-printing accumulation gap described above rather than confirming
duplex itself - the document was strip-printed, so the missing accumulator
turned it into dozens of bogus single-band "duplex pages" and the printer
canceled the job before any paper came out. That gap is now fixed (driver
revision 32); duplex itself - including the "no backside row reversal"
assumption - is still not yet physically confirmed. A backside page that
prints upside-down or mirrored, while the front side, page count and
overall page geometry are otherwise correct, points there first.

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

Expected trace tail on a one-sided success (compare against the PWG path's
equivalent in `docs/PWG_RASTER.md`):

    MintPRINT: URF begin width/height/scratch <w> <h> <bytes>
    MintPRINT: URF end rows/expected/failed <h> <h> 0
    MintPRINT: IPP result error/http/status 0 200 0

For duplex, select **Long edge** or **Short edge** under **Sides** (only
enabled once a Query has confirmed the printer advertises it for
`image/urf`), then print a multi-page document. Expect one `URF begin`/
`URF end` pair and one `Duplex page queued pages/rows/bytes` line per
page, followed by a single submission once the whole document is queued:

    MintPRINT: Duplex page queued pages/rows/bytes 1 <h> <bytes>
    MintPRINT: Duplex page queued pages/rows/bytes 2 <h> <bytes>
    MintPRINT: IPP duplex Print-Job error/http/status 0 200 0

`make test-urf` runs the host-side encoder tests (`tests/test_urf_writer.c`)
covering the file/page header byte layout, the row compression, the
multi-page duplex file/page-header sequencing, and the grow-then-patch
strip-accumulation contract (`mp_urf_grow()` plus a post-hoc height-field
patch), without needing AmigaOS hardware.
