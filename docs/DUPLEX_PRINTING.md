# Duplex printing

MintPRINT keeps duplex **off by default**. After **Query Printer**, the
**Sides** selector is enabled only when:

- `sides-supported` includes `two-sided-long-edge` and/or
  `two-sided-short-edge`;
- `document-format-supported` includes the selected engine's own document
  format (`image/pwg-raster` for **PWG Raster**, `image/urf` for **Apple
  Raster**); and
- **Printer Engine** is set to **PWG Raster** or **Apple Raster**.

If those conditions are not met, MintPRINT leaves the selector at
**One-sided** and ghosted. JPEG is a single-image format, and MintPRINT's
current PDF encoder produces one PDF per page, so neither can safely represent
one duplex job yet.

MintPRINT does not require IPP multi-document Jobs. The Brother MFC-J6930DW,
for example, supports duplex, Create-Job and Send-Document but explicitly
reports `multiple-document-jobs-supported=false`. Its supported route is one
multi-page PWG Raster document in one ordinary Print-Job.

## Sides choices

| MintPrint Settings | IPP value | Typical binding |
| --- | --- | --- |
| One-sided | `one-sided` | Print only on sheet fronts |
| Long edge | `two-sided-long-edge` | Book-style portrait pages |
| Short edge | `two-sided-short-edge` | Calendar/notepad-style portrait pages |

## Driver behaviour

The one-sided path is unchanged: every completed Amiga page uses the existing
IPP Print-Job submission. This preserves the already-tested Wordsworth,
ArtEffect and strip-printing behaviour.

With a duplex value selected, the driver instead opens one multi-page
stream in the selected engine's format, appends one page header and raster
body per completed Amiga page, and submits the complete file once from
`DriverClose()` using the existing IPP Print-Job operation with `sides=...`.
`mp_duplex_requested()` in `driver_core.c` is the single switch controlling
this for both engines.

### PWG Raster

1. opens one PWG Raster `RaS2` stream;
2. appends one 1796-byte PWG page header and raster body per completed Amiga
   page; and
3. submits the complete multi-page file once from `DriverClose()`.

The file contains only one `RaS2` sync word. Every following header/body pair
is another page in that same document.

Query also stores `pwg-raster-document-sheet-back`. That capability tells the
driver which coordinate system the printer expects for reverse-side PWG
bitmaps. MintPRINT writes the matching PWG `Duplex`, `Tumble`,
`CrossFeedTransform` and `FeedTransform` header fields. When a reverse page
must be flipped (the Brother MFC-J6930DW reports `rotated`, for example), its
already-compressed PWG rows are temporarily spooled to `T:` and replayed in
the required order. This avoids a page-sized RAM buffer and avoids always
storing three raw bytes per pixel; actual temporary size remains
content-dependent. The temporary file is removed after the page is encoded.
If the printer omits this conditionally required capability, MintPRINT uses
the standard `normal` coordinate system.

### Apple Raster (URF)

See `docs/URF_ENGINE.md` for the full format details. In short:

1. opens one Apple Raster `UNIRAST` stream, writing an "unknown/streaming"
   placeholder page count in its one file-level header;
2. appends one 32-byte page header (carrying that page's duplex/tumble
   byte) and raster body per completed Amiga page;
3. once the true page count is known, `DriverClose()` patches it into the
   placeholder before closing the file; and
4. submits the complete multi-page file once, same as PWG Raster.

**Unlike PWG Raster, no backside row/column reversal is performed.** Apple
Raster's compact page header has no sheet-back-transform-equivalent fields
(no `pwg-raster-document-sheet-back` counterpart is queried or used) - every
page, front or back, streams in the same natural row order, and the
printer's own duplex mechanism is trusted to orient the backside correctly
from the duplex/tumble byte alone. This is the main thing worth checking
carefully on a real duplex test print with `ENGINE=urf`.

## Testing a printer

1. Install the PR build and reboot so the new driver segment is loaded.
2. Open MintPrint Settings, select the printer and press **Query**.
3. Select **PWG Raster** or **Apple Raster** and confirm that **Sides**
   offers only the duplex modes reported by the printer for that engine's
   document format.
4. Save **Long edge** and print a four-page portrait document.
   Expected: two sheets, with pages 1/2 and 3/4 paired.
5. Repeat with **One-sided**. Expected: four one-sided sheets and no change to
   page size, orientation or application behaviour.
6. If supported, test **Short edge** and confirm the reverse-side
   orientation is correct. For **Apple Raster** specifically, this is the
   step most worth scrutinizing - see the note above.

Enable **Debug** before a failed test and attach `T:MintPRINT-driver.log`
and the retained job file (`T:MintPRINT-job.pwg` or `T:MintPRINT-job.urf`)
to the bug report. The log records each queued page, backside transforms
(PWG Raster) or their deliberate absence (Apple Raster), and the final
Print-Job result.
