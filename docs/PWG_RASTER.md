# MintPRINT PWG Raster backend

`DEVS:Printers/MintPRINT` has five document backends, selected by Unit0's
`ENGINE=` (set from MintPrint Settings' **Printer Engine** control):

- `ENGINE=jpeg` (default) - the original, already-proven JPEG path.
- `ENGINE=postscript` - PostScript Level 2 (`application/postscript`), see
  `docs/POSTSCRIPT_ENGINE.md`.
- `ENGINE=pwg-raster` - streams a PWG Raster (`image/pwg-raster`) document
  instead, via `driver/pwg_writer.c`.
- `ENGINE=pdf` - a minimal single-page PDF (`application/pdf`), see
  `docs/PDF_ENGINE.md`.
- `ENGINE=urf` - Apple Raster (`image/urf`), see `docs/URF_ENGINE.md`.

All paths share the same row source (`mp_job_write_row` in
`driver/driver_core.c`, unchanged) and the same low-memory, one-row-at-a-time
streaming shape - only the encoder and the IPP `document-format` differ.

## Status: implemented, not yet physically test-printed

The encoder was written against the actual PWG/CUPS raster specification
(field layout, sync word, enum values and the row compression algorithm
verified against the public CUPS reference implementation - `cups/raster.h`
and `cups/raster-stream.c` - rather than from memory alone) and the header
byte count was cross-checked to land on exactly 1796 bytes, the well-known
size for this header. That gives good confidence the *bytes on the wire* are
shaped correctly. It has not yet been confirmed against a real printer with
actual paper coming out the other end - that is the next step, and the only
way to be fully sure.

If a PWG Raster test print comes out wrong (garbled image, printer error,
rejected job), the most useful next artifact is `T:MintPRINT-job.pwg`
(kept when `DEBUG=1`) copied off the
Amiga - the first ~1800 bytes are the sync word + page header and are worth
checking against `cups/raster.h`'s `cups_page_header2_t` field-by-field if
something looks off.

## Row compression

PWG/CUPS raster's row compression is a PackBits-style scheme with two halves:
a "repeat run" (control byte 0-127, meaning the next pixel repeated
1-128 times) and a "literal run" (control byte 129-255, meaning 2-128
distinct pixels follow verbatim). This encoder deliberately only ever emits
the first half: a genuine run of identical pixels is compressed normally,
and any pixel that does not extend a run is simply emitted as its own
trivial run of one (byte `0x00` + that one pixel). This is a fully valid
encoding of the format - decoders only care that runs decode correctly, not
that the more space-efficient literal-run encoding was used where it could
have been - and it avoids that encoding's own edge cases (a minimum run
length of 2, and a control-byte formula, `257 - count`, that overflows a
byte for a literal run of exactly 1). Less optimal compression for
detailed/dithered content, in exchange for one less failure-prone code path
in something that cannot be easily debugged from a screenshot.

## Raster type

Pages are written as `srgb_8`: 8-bit sRGB, chunked/interleaved (RGB, RGB,
RGB, ...), 300 DPI - the same RGB reconstruction from `pi_ColorInt`
already used by the JPEG path, at the baseline raster type every
IPP-Everywhere-conformant `image/pwg-raster` printer is required to accept.
Only a single page is supported, matching the existing JPEG path's scope.

## Build/install/test

    make clean
    make driver

Copy `build/driver/MintPRINT` to `DEVS:Printers/MintPRINT` and reboot (or
otherwise ensure the old driver segment is unloaded) before testing.

In MintPrint Settings, set **Printer Engine** to **PWG Raster** and **Save**
for the unit you want to test with (only Unit0 is live for printing - see
"Multiple printers (Units)" in `docs/MINTPRINT_PREFS.md` if testing a
non-Unit0 profile). Then print as usual (MultiView, GraphicDump, etc.).

Before printing:

    Delete T:MintPRINT-driver.log QUIET
    Delete T:MintPRINT-job.pwg QUIET

Expected trace tail on success (compare against the JPEG path's equivalent
in `docs/PRINTER_DEVICE_SPIKE3.md`):

    MintPRINT: PWG begin width/height/scratch <w> <h> <bytes>
    MintPRINT: PWG end rows/expected/failed <h> <h> 0
    MintPRINT: IPP result error/http/status 0 200 0
