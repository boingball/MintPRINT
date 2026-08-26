# MintPRINT PDF backend

`DEVS:Printers/MintPRINT` has five document backends, selected by Unit0's
`ENGINE=` (set from MintPrint Settings' **Printer Engine** control):

- `ENGINE=jpeg` (default) - the original, already-proven JPEG path.
- `ENGINE=postscript` - PostScript Level 2 (`application/postscript`), see
  `docs/POSTSCRIPT_ENGINE.md`.
- `ENGINE=pwg-raster` - PWG Raster (`image/pwg-raster`), see
  `docs/PWG_RASTER.md`.
- `ENGINE=pdf` - a minimal single-page PDF (`application/pdf`), via
  `driver/pdf_writer.c`.
- `ENGINE=urf` - Apple Raster (`image/urf`), see `docs/URF_ENGINE.md`.

## Why PDF, and why it isn't a new image encoder

Most modern network printers are IPP Everywhere or AirPrint certified and
are required to accept PWG Raster, so PDF isn't needed for the common
case. It matters for older or partially-compliant IPP printers - often
office/business multifunction devices whose IPP support was bolted onto an
existing PDF/PostScript print pipeline - that only advertise
`application/pdf` and reject raster formats outright.

PDF's `DCTDecode` filter embeds a raw baseline JPEG bitstream verbatim, so
`pdf_writer.c` does not implement a second pixel encoder at all - it reuses
`jpeg_writer.c` exactly as the JPEG backend does, and just writes a small,
fixed-shape PDF container around the same byte stream: one
Catalog/Pages/Page/XObject/Contents object set, a cross-reference table,
and a trailer. Same row source (`mp_job_write_row` in `driver/driver_core.c`,
unchanged), same low-memory, one-row-at-a-time streaming shape as the
other three backends - a page is never held in memory as a whole.

## The one PDF-specific complication: stream length

A PDF stream's byte length is conventionally declared in its object
dictionary *before* the stream data - awkward for a design that streams
rows straight through to the JPEG encoder and never buffers or seeks back
to patch a field once the final size is known. The fix is standard, valid
PDF: the image object declares `/Length 6 0 R`, an indirect reference to a
separate object written *after* the stream ends, once its actual byte
count is known. Object 6 is nothing but that one integer.

## Status: implemented, not yet physically test-printed

The container structure (object layout, xref table byte offsets, the
deferred-length trick) was written and reviewed carefully, including
catching and fixing one real bug before it shipped: the content stream's
image-placement matrix was initially scaled by the page's pixel
dimensions instead of its point dimensions (`MediaBox` is in points, 1/72
inch; pixel counts there would have drawn the image far larger than the
page). That's the kind of mistake that would only surface as a wrong-sized
or clipped image on real paper, with no error message - worth stating
plainly rather than assuming the fix is correct just because it now looks
right. It has **not** yet been confirmed against a real printer with
actual paper coming out the other end.

If a PDF test print comes out wrong (garbled image, printer error,
rejected job, or a viewer refusing to even open the file), the most
useful next artifact is `T:MintPRINT-job.pdf` (kept when `DEBUG=1`) copied off
the Amiga - it's a plain text+binary
file, readable in any PDF viewer or text editor for the header/object
structure, worth checking object-by-object against this file's layout if
something looks off.

## Build/install/test

    make clean
    make driver

Copy `build/driver/MintPRINT` to `DEVS:Printers/MintPRINT` and reboot (or
otherwise ensure the old driver segment is unloaded) before testing.

In MintPrint Settings, set **Printer Engine** to **PDF** and **Save** for
the unit you want to test with (only Unit0 is live for printing - see
"Multiple printers (Units)" in `docs/MINTPRINT_PREFS.md` if testing a
non-Unit0 profile). Then print as usual (MultiView, GraphicDump, etc.).

Before printing:

    Delete T:MintPRINT-driver.log QUIET
    Delete T:MintPRINT-job.pdf QUIET

Expected trace tail on success (compare against the JPEG/PWG paths'
equivalents in `docs/PRINTER_DEVICE_SPIKE3.md` and `docs/PWG_RASTER.md`):

    MintPRINT: PDF begin width/height/scratch <w> <h> <bytes>
    MintPRINT: PDF end rows/expected/failed <h> <h> 0
    MintPRINT: IPP result error/http/status 0 200 0
