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
byte 2:      duplex/tumble mode (0 = no duplex, 1 = duplex, short side
             [two-sided-short-edge], 2 = duplex, long side [two-sided-long-edge])
byte 3:      print quality      (0 - unspecified)
byte 4:      media type         (0 - unspecified)
byte 5:      media position     (0 - auto)
bytes 6-11:  reserved, zero
bytes 12-15: width in pixels
bytes 16-19: height in pixels
bytes 20-23: resolution (dpi - one figure used for both axes)
bytes 24-31: reserved, zero
```

The byte 2 (duplex/tumble) values above were corrected after real-hardware
testing (see "Duplex and strip-printing accumulation" below) - the
original CUPS-source-derived implementation used 1/2/3 instead of 0/1/2,
which was independently caught against a second, unrelated source: a
published from-scratch reverse-engineering of the on-the-wire format
against a real HP DesignJet T230. Every other field's offset and size in
this table was confirmed correct against that same source.

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
revision 32 fixed this by extending the same accumulator PWG Raster
already had to also cover URF - see `mp_engine_supports_strip_accumulation()`
in `driver_core.c`.

**A second, related bug surfaced immediately after** (same Brother test,
driver revision 32): the accumulator now correctly built two real pages,
but they came out different heights - the front page's real content
naturally overshot the media-derived target (its bands didn't divide
evenly into it), while the back page fell short and only got padded up to
the plain target, leaving front and back at different heights. A physical
duplex sheet's two sides have to share one height, and the printer
rejected the job again for it (confirmed by decoding the retained
`T:MintPRINT-job.urf`: page 1 = 3562 rows, page 2 = 3505 rows, both
otherwise byte-perfect). Driver revision 33 fixes this with
`g_duplex_max_page_height` - the tallest page any duplex job has
finalized at so far, used as an extra floor on every later page's target
alongside the media-derived one. This only ever raises a page's padding,
never truncates real content, and is a no-op whenever pages already
matched - see the comment at its declaration in `driver_core.c`. It
converges same-sized pages within a duplex job onto one consistent
height; a document whose page sizes actively grow through the job (a
later page genuinely needing more content than any page before it) can
still end up with a final mismatched pair, since an earlier, already-
queued page can never be enlarged after the fact once submitted.

**A third bug was found once the first two were fixed** (same Brother
test, driver revision 33): with the accumulator and page-height matching
both now correct - two pages, byte-perfect, both exactly 3562 rows - the
printer *still* rejected the job with the identical
`server-error-job-canceled`, immediately and synchronously (confirmed via
`ipp_client.c`'s `rc=-16` path, which only fires when the printer's own
IPP response to the submission itself carries an error status - this
isn't a later async rasterization failure). The cause was the duplex/tumble
byte values themselves: the original implementation, derived from a CUPS
source paraphrase, used 1 = no duplex, 2 = short side, 3 = long side.
Cross-checking against a second, independent, real-world source (see
"Format layout" above) showed the actual on-the-wire values are 0 = no
duplex, 1 = short side, 2 = long side - so a `two-sided-long-edge` job was
sending byte value 3, which isn't a defined value in that second source at
all. Driver revision 34 fixes the enum in `mp_urf_write_page_header()`.
Notably, the one-sided test that confirmed URF's row/header layout back at
driver revision 30 used the old scheme's "simplex" value (1), which under
the corrected enum actually means "duplex, short side" - it likely worked
anyway because that job's IPP-level `sides=one-sided` never told the
printer's duplexer to engage at all, so the page-embedded hint went
unchecked; the stricter duplex path evidently does check it.

**A fourth test, on driver revision 34, ruled out the duplex/tumble byte
as the (whole) explanation.** The retained `T:MintPRINT-job.urf` was
independently decoded byte-for-byte: 2 pages, both exactly 2478x3562,
300dpi, `duplex=2` (the corrected "long side" value, matching the job's
`sides=two-sided-long-edge`), every row's PackBits data decodes cleanly,
and the file ends exactly at the last row of page 2 with zero trailing or
missing bytes - about as byte-perfect as a duplex URF stream can be. The
printer still rejected it with the identical `server-error-job-canceled`,
immediately and synchronously, exactly as it did for the byte-*imperfect*
files in the first three tests. That repetition across four structurally
very different files is itself informative: it suggests the printer's
rejection may not be reacting to the URF stream's content at all, and
could instead be a blanket "no duplex over `image/urf`" policy on this
particular printer/firmware, unrelated to anything MintPRINT controls in
the raster stream.

MintPRINT's own IPP client only records the numeric IPP status, not the
`status-message` text a compliant printer is supposed to return alongside
it - so the driver log alone can't say *why* the printer canceled the job,
only that it did. `windows_ipp_probe.py` (v2.2+) can now get that text
directly: `--print-file` accepts a `--sides` value, which reproduces the
exact IPP request MintPRINT's own duplex path sends (including the
`sides` Job Template attribute), and the report now prints any
`status-message`/`job-state-reasons` the printer sends back. Run it
against the retained `T:MintPRINT-job.urf` from a PC on the same network:

    python windows_ipp_probe.py http://<printer-ip>:631/ipp/print \
        --print-file job.urf --mime image/urf --sides two-sided-long-edge

If the printer's own error text says something like "format not
supported for two-sided printing" or similar, that confirms the "no URF
duplex on this printer" theory above and this isn't a MintPRINT bug to
chase further on the Brother; if it names something else (an unexpected
attribute, a media/resolution mismatch), that reopens the search with an
actual reason to go on instead of a bare status code.

**A fifth, unrelated-to-duplex bug was found testing PWG Raster on the same
Brother printer** (driver revision 34): a genuine two-page Wordworth
document (real letter text, graphics, its own 0.5in top / 1.0in bottom
margins) printed one-sided with no IPP error at all, but came out visibly
corrupted - decoding and rendering the retained job file to PNG showed a
character graphic cut off at the very top of the second physical page, and
the same graphic's fragments recurring further down. The physical page
boundary MintPRINT chose had landed in the middle of real content instead
of at Wordworth's own page break.

Root cause: this accumulator only checks whether the target page height has
been *reached* once a whole `SPECIAL_NOFORMFEED` band has already been
accepted and written - never whether accepting that band would *overshoot*
it. When the page was already close to, but under, target and the next
~100-row band arrived, the accumulator took the entire band anyway, letting
the page run up to ~100 rows past its true boundary. Since Wordworth streams
its *entire* multi-page document as one unbroken sequence of same-width
NOFORMFEED bands with no other page-break signal, those overshoot rows are
never spare padding - they are always the start of whatever comes next
(here, the next page's own top margin and content), silently welded onto
the bottom of the page that just filled up. This affects PWG Raster and URF
identically, strip-printed one-sided or duplex - it long predates this URF
duplex work (unchanged accumulator logic; see the driver rev 32 fix above,
which only extended the same pre-existing behaviour to URF) and simply
hadn't surfaced on a document whose real per-page content didn't divide
evenly into the target height.

Driver revision 35 fixes this: a continuation band that would overshoot
`g_page_target_height` now only has its first `g_split_at_row` rows applied
to the page being finished (closing it out at exactly the target, so no
padding is needed either); the remaining rows start a fresh page instead,
via `mp_begin_split_page()` (mirrors `Render()` case 0's own "begin new
page" path). Rows already arrive one at a time through `Render()` case 1
(`mp_job_write_row()`), so the split happens mid-band, at the exact row the
target is reached, before that row is written to either encoder - no row is
ever dropped or duplicated. Not yet re-tested on real hardware; validated
so far only by manual review (no cross-compiler in this environment) and by
hand-tracing the exact band sequence from the real driver log that exposed
the bug, which the fix reproduces cleanly: page 1 finalizes at exactly 3505
rows (the target, not 3562) and the 57 previously-stolen rows correctly
become page 2's own leading content instead.

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
first three real tests (Brother MFC-J6930DW) each surfaced a real bug
rather than confirming duplex itself - every time the printer canceled the
job before any paper came out: first the missing strip-printing
accumulator (driver revision 31, fixed in 32), then the front/back
page-height mismatch it exposed once fixed (driver revision 32, fixed in
33), then the duplex/tumble byte enum itself being wrong (driver revision
33, fixed in 34 - see "Format layout" and "Duplex and strip-printing
accumulation" above). A fourth test on revision 34, with a file confirmed
byte-perfect end to end, hit the identical rejection anyway - see that
section for why this now points at a possible printer-side "no duplex
over `image/urf`" limitation rather than a remaining MintPRINT bug, and
the `windows_ipp_probe.py --print-file --sides` diagnostic added to get
the printer's own error text instead of guessing further from a bare
status code. Whether URF duplex specifically can complete and print - and,
separately, whether the "no backside row reversal" assumption holds - is
still not physically confirmed on any printer; PWG Raster duplex, by
contrast, *has* since been confirmed working on this same Brother printer
(`IPP duplex Print-Job error/http/status 0 200 0`), while testing turned up
the fifth, accumulator-overshoot bug described above (driver revision 35).

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
