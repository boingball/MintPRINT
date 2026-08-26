# MintPRINT printer compatibility

This page records real MintPRINT hardware results: the printer, AmigaOS
version, TCP/IP stack, document engine and the settings needed to reproduce a
working print. It deliberately distinguishes physical output from an IPP job
that merely reports success.

Last reviewed: **26 August 2026**

## Status key

| Status | Meaning |
|---|---|
| ✅ Working | Physical output has been confirmed from MintPRINT. |
| 🟡 Partial | Some jobs print, but an application, orientation, query or scaling problem remains. |
| 🧪 Testing | A likely fix or new engine exists but has not produced a confirmed MintPRINT print yet. |
| ❌ Not working | The tested MintPRINT engine does not produce physical output. |

An HTTP 200 response, IPP `successful-ok`, completed job state or advertised
document format is **not** enough to mark a printer working. Some firmware
accepts a job and silently discards it.

## Compatibility summary

| Printer and machine | Status | AmigaOS | TCP/IP stack | Confirmed engine/result | Required or known settings |
|---|---|---|---|---|---|
| **Brother MFC-J6930DW** | ✅ Working | 3.2.3 | Roadshow | PWG Raster; URF also confirmed (driver rev 30) | Port `631`; path `/ipp/print`; `300 dpi`; A4; tray `auto`; scaling `auto`; quality `draft`; colour |
| **Brother HL-L2350DW** on A500 PiStorm, Wi-Fi | ✅ Working | 3.2.3 | Roadshow | Original Aminet release reported as working perfectly; exact engine was not recorded | No printer-specific override reported |
| **Brother HL-L2350DW** on A4000, CSMkII 060/50 and Ariadne-II, wired | ✅ Working | 3.2.3 | Roadshow | Multi-page printing fully fixed - confirmed in issue #8 after a beta build, and fully in released 1.1.0 | 1.1.0; scaling `auto` |
| **Canon TS8360** (IPP identifies it as **TS8300 series**) | ✅ Working | 3.2.3 | Not reported | PWG Raster text and colour pictures physically confirmed; JPEG pictures also work | Port `631`; path `/ipp/print`; PWG Raster; **`300* dpi` compatibility mode**; A4; source `auto`; scaling as required. Printer advertises only 600 DPI but accepts 300 DPI |
| **HP OfficeJet 8014e** on A4000, 68060, Wi-Fi | ✅ Working | 3.2.3 | Not reported | Detection/Query and Test Print fully fixed - confirmed in issue #30 after a beta build, and fully in released 1.1.0 | Port `631`; path `/ipp/print`; advertises both JPEG and PWG Raster - a fresh add now defaults to PWG Raster |
| **Samsung C480W / C48x Series** | 🟡 Partial (PostScript only) | 3.9 Boing Bag 2; Kickstart 3.1 | Not reported | JPEG is silently discarded; PWG Raster and PDF are rejected. PostScript engine confirmed physically printing, but is slow since this printer only accepts PostScript | Port `631`; path `/ipp/print`; `300 dpi`; A4; tray `tray-1`; normal quality; scaling `auto`; allow 3–4 minutes for PostScript |
| **OKI B412** | 🧪 Testing | Not reported | Not reported | Advertises only `application/octet-stream, application/vnd.hp-PCL, image/urf` - none of MintPRINT's four existing engines. Motivated the new `ENGINE=urf` Apple Raster backend ([docs/URF_ENGINE.md](URF_ENGINE.md)); not yet physically test-printed | Port `631`; path `/ipp/print`; Engine `urf` (auto-selected after Query, since no other MintPRINT engine is advertised) |

“Not recorded” is intentional. Do not assume Roadshow, AmiTCP or Miami from
the presence of `bsdsocket.library`; reports should name the actual stack and
version.

## Printer details

### Brother MFC-J6930DW

Confirmed working configuration:

```text
Engine:      PWG Raster
Port:        631
IPP path:    /ipp/print
DPI:         300
Media:       iso_a4_210x297mm
Tray/source: auto
Scaling:     auto
Quality:     draft
Print mode:  color
```

The recorded driver run completed the PWG document and received a successful
HTTP/IPP response. The AmigaOS release and TCP/IP stack were not written into
the test record and still need adding.

**URF (Apple Raster) also confirmed**, driver revision 30: this printer
advertises both `image/pwg-raster` and `image/urf`, so it doubled as the
first real-hardware confirmation for the new `ENGINE=urf` backend
(`docs/URF_ENGINE.md`), added for the OKI B412 report
([issue #60](https://github.com/boingball/MintPRINT/issues/60)). A full
A4 (2480x3508 @ 300dpi) test page printed correctly with `ENGINE=urf`
explicitly selected; the driver log showed `URF end rows/expected/failed
3508 3508 0` and a successful IPP result, and the retained
`T:MintPRINT-job.urf` decodes cleanly to the expected header and all 3508
rows. `pwg-sheet-back=rotated`; scaling `auto`; quality `high`; color;
source `auto`.

This printer also advertises `two-sided-long-edge`/`two-sided-short-edge`,
making it the printer that first tested URF duplex - so far across three
attempts, every one rejected by the printer before any paper came out,
each exposing a different real bug:

1. **Driver revision 31.** The test document was strip-printed
   (`SPECIAL_NOFORMFEED`), and URF's duplex implementation at that revision
   had no strip-printing accumulator (PWG-only at the time), so every small
   band became its own bogus single-band "duplex page" - the driver log
   showed 62 pages queued for what should have been a handful. Rejected
   with `IPP duplex Print-Job error/http/status -16 200 1288`
   (`server-error-job-canceled`). Fixed in driver revision 32 by extending
   the same strip-printing accumulator PWG Raster already had to also
   cover URF.
2. **Driver revision 32.** The accumulator now correctly built two real
   pages, but decoding the retained `T:MintPRINT-job.urf` showed them at
   different heights - page 1 (front) 3562 rows, page 2 (back) 3505 rows,
   both otherwise byte-perfect - and the printer rejected the job again
   with the identical IPP status. A physical duplex sheet's two sides have
   to share one height; real strip-printed content naturally overshot the
   media-derived target on the front while the back only got padded to
   the plain target. Fixed in driver revision 33 by flooring every later
   page's target at the tallest page the duplex job has finalized so far
   (`g_duplex_max_page_height`), converging same-sized pages onto one
   height without ever truncating real content.
3. **Driver revision 33.** Both pages now came out byte-perfect and an
   exactly matching 3562 rows each - and the printer *still* rejected the
   job with the identical IPP status, immediately and synchronously. The
   cause was the duplex/tumble byte itself (page header offset 2): the
   original CUPS-source-derived implementation used 1/2/3 for no-duplex/
   short-side/long-side; cross-checking against a second, independent,
   real-world URF reverse-engineering (against an HP DesignJet T230) found
   the correct values are 0/1/2 - meaning this printer was being sent byte
   value 3 for a `two-sided-long-edge` job, which isn't a defined value in
   that second source at all. Fixed in driver revision 34 - see
   `docs/URF_ENGINE.md`'s "Format layout" section for the corrected table
   and citation.

See `docs/URF_ENGINE.md` for all three fixes in detail. Not yet re-tested;
still also unconfirmed once retested is whether Apple Raster's lack of a
backside-transform mechanism (unlike PWG's `pwg-sheet-back=rotated` above)
produces a correctly-oriented backside on this specific printer's duplex
unit.

**A fourth, separate bug was found testing PWG Raster on this same
printer** (driver revision 34), unrelated to duplex or to URF specifically:
a genuine two-page Wordworth document (real letter text and graphics, with
its own 0.5in top / 1.0in bottom page margins) printed one-sided with no
IPP error at all, but the output was visibly corrupted - decoding the
retained job file to an image showed a graphic cut off at the very top of
the second physical sheet, with fragments of it recurring further down.
`IPP duplex Print-Job error/http/status 0 200 0` on a retest confirmed PWG
Raster duplex itself now works end to end on this printer, but reproduced
the identical visible corruption on both sides. Root cause: the
strip-printing accumulator only checked whether a page had *reached* its
target height after accepting a whole `SPECIAL_NOFORMFEED` band, never
whether accepting that band would *overshoot* it - so up to ~100 rows that
belonged to the next page (including, in this case, its own top margin)
were silently welded onto the bottom of the page that had just filled up.
This affects PWG Raster and URF identically, one-sided or duplex, and
predates this URF duplex work entirely (the accumulator logic itself was
unchanged; URF only inherited it in driver revision 32 above). Revision 35
added a media-height split, but its real retest proved Wordworth's logical
page ends at a short 62-row remainder after 3062 printable rows, before the
3505-row physical A4 boundary; rev 35 consequently still stole 443 rows
from page 2. Revision 36 recognises that terminal short band for both normal
and narrow auxiliary dumps, pads/finalises the sheet at the logical boundary,
and keeps the media-height split as a fallback - see `docs/URF_ENGINE.md`'s
"Duplex and strip-printing accumulation" section for the full mechanism.
The rev 36 physical retest then showed that the 443 missing physical rows were
all appended at the bottom: pagination was correct, but the configured
0.50-inch top border was lost. Revision 37 added support and diagnostics for
printer.device's standard `aSTBM` command, but its real trace proved Wordsworth
does not send that command: it clears margins, sets a 70-line A4 form, then
sends only the 3062-row printable raster. Revision 38 recognises that matching
form-length/physical-media strip signature and restores the documented
0.50-inch top border (150 rows at 300 DPI); final media padding continues to
supply the remaining bottom border. Revision 39 also processes pre-dump
`aIND`, `aNEL`, and literal LF movement at the active VMI as the stronger,
genuine vertical-position signal; the rev 38 form-length reconstruction now
acts only as a fallback when no such movement or explicit `aSTBM` margin is
present.
Revision 40 adds full diagnostics without changing that placement behaviour:
every layout command from `aVERP0` through `aCAM` is logged with parameters and
live line/VMI state, along with each band's `IODRPReq` source and destination
geometry. This is intended to find any undocumented Wordsworth placement clue
before treating the form-length reconstruction as permanent behaviour.

### Brother HL-L2350DW

Two systems were reported against the same printer in
[issue #8](https://github.com/boingball/MintPRINT/issues/8):

- **A500 PiStorm, AmigaOS 3.2.3, Roadshow over Wi-Fi:** the original Aminet
  release discovered the printer immediately and printed documents correctly.
  The exact engine and MintPRINT job settings were not included in the report.
- **A4000, CSMkII 060/50, Ariadne-II wired, AmigaOS 3.2.3, Roadshow:**
  A simple one-page AmigaWriter document printed correctly with MintPRINT
  1.0.3 and scaling `auto`, but a later three-page document exposed page
  handling problems:
  - with 1.0.3, all three document pages printed, but each was followed by a
    Brother-generated error sheet containing the Swedish text
    `-data som inte stöds för direktutskrift: 3000` (“-data not supported for
    direct printing: 3000”); the third document page was also greatly enlarged
    and only its central text printed;
  - with the 1.0.3a/main-line revision-15 test build, the extra error sheets
    disappeared, but only document page 2 printed and its page break moved two
    rows earlier.

  These were page-boundary regressions in the multi-band accumulator. A beta
  build fixing the page-boundary handling was sent for testing and confirmed
  working in [issue #8](https://github.com/boingball/MintPRINT/issues/8); the
  fix shipped fully in the 1.1.0 release, and multi-page wired printing on
  this machine is now confirmed working end to end.

The Roadshow result confirms the TCP stack is viable, and the A4000
rendering/page-handling problem is now resolved as of 1.1.0.

### Canon TS8360 / TS8300 series

Evidence and ongoing work are recorded in
[issue #5](https://github.com/boingball/MintPRINT/issues/5) and
[issue #6](https://github.com/boingball/MintPRINT/issues/6).

The printer reports:

```text
Port/path:       631 /ipp/print
Formats:         image/jpeg, image/urf, image/pwg-raster
PWG type:        srgb_8, sgray_8
DPI:             600
Media default:   iso_a4_210x297mm
Source default:  auto
Quality default: draft
Scaling default: auto
Colour default:  color
```

Confirmed after the Canon fixes merged in PR #26:

- A JPG picture prints in colour using either PWG Raster or JPEG.
- The picture prints correctly in both portrait and landscape.
- PWG Raster text prints as laid out, including the application's top margin.
- Query completes through the Canon's interim/chunked HTTP response.
- Tiny `NOFORMFEED` bands no longer become separate jobs, and their vertical
  extent is retained as page whitespace.

The printer advertises only 600 DPI through IPP, but its 600-DPI AmigaWriter
raster has poor font rendering. Manually setting `RESOLUTION=300` produced a
fully correct physical page. Settings therefore offers **`300* dpi`** for PWG
Raster; `*` identifies an unreported compatibility resolution rather than a
capability claimed by the printer.

### HP OfficeJet 8014e

Full evidence is in
[issue #30](https://github.com/boingball/MintPRINT/issues/30). The tested
system was an A4000 with a 68060 (128MB Fast RAM plus a 128MB Zorro III
card), AmigaOS 3.2.3, connected to the printer over Wi-Fi.

The printer's IPP capability response is unremarkable - it advertises both
`image/jpeg` and `image/pwg-raster` (not `application/pdf`), answers on
port `631` at `/ipp/print`, and reports `iso_a4_210x297mm` as its default
media. Two problems specific to this report, both since fixed:

- **Detection/Query.** The printer was found by Discover (SSDP/mDNS, which
  only needs a UDP reply) but a direct Query, and the follow-up query after
  Discover, both timed out. HP OfficeJet/Envy-series printers let their
  Wi-Fi radio drop into a power-save state between jobs; the radio wakes for
  broadcast/multicast discovery traffic, but the first real TCP connect
  afterwards can be slow enough to blow past the driver's connect timeout.
  Fixed by giving a connect timeout on the primary port a second attempt
  before moving on, and raising the timeout itself from 5 to 8 seconds.
- **Test Print.** After the Query fix, Test Print appeared to hang - the
  driver log showed a JPEG encode targeting `3287x3508` instead of the
  correct `2480x3508` for A4 (the same oversized-DUMPRPORT-page-width
  behaviour documented for the Settings test page elsewhere in this repo,
  here hitting the JPEG engine since this printer's driver defaulted to
  JPEG despite also supporting PWG Raster). ~30% more pixels than necessary,
  through the JPEG encoder's original direct-matrix DCT, was slow enough on
  real 68k hardware to look hung rather than merely slow. Addressed by
  extending the page-width clamp to the JPEG/PDF engines (previously
  PWG-only), replacing the JPEG DCT with a much faster algorithm, and
  making PWG Raster - unaffected by the JPEG-specific slowness in the first
  place - the default engine whenever a printer advertises it.

A beta build with these fixes was sent for testing and confirmed working
against this printer's actual hardware in issue #30; Detection/Query and
Test Print are now fully fixed in the released 1.1.0.

### Samsung C480W / C48x Series

Full evidence is in
[issue #15](https://github.com/boingball/MintPRINT/issues/15). The tested
system was AmigaOS 3.9 Boing Bag 2 with Kickstart 3.1; the TCP/IP stack was not
reported.

| Engine/format | Result |
|---|---|
| JPEG / `image/jpeg` | Printer advertises it, Validate-Job accepts it and the job completes, but no page is produced and the billing counter does not increment. |
| PWG Raster / `image/pwg-raster` | Rejected with `client-error-document-format-not-supported`. |
| PDF / `application/pdf` | Rejected with `client-error-document-format-not-supported`. |
| PostScript / `application/postscript` | Confirmed physically printing and incrementing the page counter with MintPRINT's PostScript engine ([PR #17](https://github.com/boingball/MintPRINT/pull/17)). This printer is PostScript-only, so printing through it is slow. |

Reported/default settings:

```text
Port:        631
IPP path:    /ipp/print
DPI:         300
Media:       iso_a4_210x297mm
Tray/source: tray-1
Scaling:     auto
Quality:     normal
Print mode:  color
Sides:       one-sided
```

The printer can take **three to four minutes** to produce a PostScript page.
Do not declare failure after a short wait. Its IPP job byte/impression counters
also remain zero for jobs that physically print, so the device billing counter
or actual paper is the reliable test.

### OKI B412

Reported in [issue #60](https://github.com/boingball/MintPRINT/issues/60)
via `windows_ipp_probe.py` output. The printer reports:

```text
Port/path:       631 /ipp/print
Formats:         application/octet-stream, application/vnd.hp-PCL, image/urf
Media default:   iso_a4_210x297mm
Source default:  tray-1
Sides supported: one-sided, two-sided-long-edge, two-sided-short-edge
Colour supported: no (monochrome only)
```

None of MintPRINT's engines at the time (JPEG, PostScript, PWG Raster, PDF)
matched any of the three advertised formats - this printer is Apple Raster
(URF) only among the formats MintPRINT can produce. `application/vnd.hp-PCL`
was also considered but not pursued, since Apple Raster reuses the row
compression already proven by the PWG Raster backend rather than requiring a
new page-description language. See `docs/URF_ENGINE.md` for the resulting
`ENGINE=urf` backend, added specifically for this report.

The `ENGINE=urf` backend itself is now confirmed physically printing (see
the Brother MFC-J6930DW entry above), but not yet on this specific
printer - still needs an OKI B412 test report to close out issue #60.

This printer also advertises duplex sides (`two-sided-long-edge`,
`two-sided-short-edge`). MintPRINT duplex originally required
`ENGINE=pwg-raster`, which this printer does not advertise - but `ENGINE=urf`
now supports duplex too (driver revision 34, see `docs/DUPLEX_PRINTING.md`
and `docs/URF_ENGINE.md`), so duplex is expected to be reachable here once
this printer's own URF printing is confirmed, one-sided first.

## AmigaOS and TCP/IP stack status

| Environment | Status |
|---|---|
| AmigaOS 3.2.3 + Roadshow, A500 PiStorm Wi-Fi | ✅ Confirmed end-to-end with Brother HL-L2350DW |
| AmigaOS 3.2.3 + Roadshow, A4000/Ariadne-II wired | ✅ Confirmed end-to-end, including multi-page output, with Brother HL-L2350DW as of 1.1.0 |
| AmigaOS 3.9 BB2, TCP stack not reported | 🟡 IPP transport reaches Samsung C480W; no released MintPRINT engine currently prints on it |
| AmigaOS 3.1 classic driver | 🧪 Structurally implemented but no physical OS3.1 print is recorded yet |
| AmiTCP | 🧪 Expected through compatible `bsdsocket.library`; no named hardware report yet |
| Miami | 🧪 Expected through compatible `bsdsocket.library`; no named hardware report yet |

MintPRINT requires a working `bsdsocket.library`-compatible TCP/IP stack.
MintPrint Settings checks for `bsdsocket.library` V4 and a usable socket before
opening. Roadshow, AmiTCP and Miami are supported targets, but only Roadshow has
a named community hardware result so far.

### AmigaOS 3.2.3 Printer Preferences

In the standard AmigaOS Printer Preferences editor, select **MintPRINT** as the
Printer Type. For the normal OS 3.5+/3.2 driver, that is the only setting in
that particular editor that MintPRINT itself requires. Do not confuse it with
the separate **Graphics Printer Preferences** editor described below.

The normal driver declares `PRTA_NoIO`, so Printer Port, device name and Device
Unit are not used. MintPRINT sends over IPP using the host, port and path saved
by MintPrint Settings instead. Print Pitch, Print Spacing, Print Quality, Paper
Type, Paper Format, Paper Length and the character margins are legacy text
printer preferences; MintPRINT's current graphics-focused driver does not
implement the text command table that would apply them. They may be left at
their AmigaOS defaults.

An application can still read standard system preferences while laying out its
own page, so record any non-default application Print Setup separately. This is
why the Wordworth and ArtEffect settings below still matter even though the OS
Printer Preferences values do not configure MintPRINT's IPP output.

This does not apply unchanged to the separate AmigaOS 3.1 classic build, which
cannot use the V44 `PRTA_NoIO` tag and still needs dedicated hardware testing.

### AmigaOS Graphics Printer Preferences

These settings **do affect graphics printing**. AmigaOS `printer.device`
applies them while converting an application's bitmap into the raster rows
delivered to MintPRINT. An application can override them by supplying its own
`PRD_DUMPRPORT` dimensions and special flags. The controls and their effects
are described in the
[AmigaOS Workbench printer manual](https://wiki.amigaos.net/wiki/AmigaOS_Manual%3A_Workbench_Printers#PrinterGfx_Preferences_Editor).

Confirmed AmigaOS 3.2.3 test-machine baseline:

```text
Dithering:       Ordered
Scaling:         Fraction
Image:           Positive
Aspect:          Horizontal (portrait)
Shade:           Black & White
Threshold:       7
Density:         1
Smoothing:       Off
Center Picture:  Off
Color correction: Off
Colors:          4096
Left Edge:       0
Limits Type:     Ignore
```

These are the settings on the confirmed test machine and they still produce
colour through MintPRINT. Although the AmigaOS manual describes `Shade` as the
default colour-mode choice, applications can override PrinterGfx defaults in
their graphics-dump request; do not require users to change `Black & White` to
`Color` when colour output is already working. If one particular application
unexpectedly prints monochrome, its own Print Setup and the PrinterGfx Shade
setting are both worth checking. `Horizontal` means portrait and `Vertical`
means landscape. `Limits=Ignore` leaves the requested print size under
application control and avoids an additional hidden width/height constraint.

PrinterGfx Density is not MintPRINT's IPP resolution selector. MintPRINT's DPI
comes from MintPrint Settings; keep the OS density at `1` as the baseline and
record application-specific density overrides such as those required by
Wordworth and ArtEffect.

## Application compatibility

Application compatibility is tracked separately from printer compatibility.
Older Amiga applications can exercise `printer.device` in very different ways:
some submit a complete raster, while others use many `SPECIAL_NOFORMFEED`
graphics dumps to assemble one physical page.

| Application | Status | Confirmed environment | Result | Required application setup |
|---|---|---|---|---|
| **Wordworth 7** | ✅ Working | AmigaOS 3.2.3, Roadshow, Brother MFC-J6930DW, PWG Raster, driver revision 16 | Portrait and three-page documents print with the correct orientation and page boundaries; the former trailing blank sheet is gone | Select `MintPRINT`, `Normal`, `Sheet Feeder`, Density `7`; borders Left `0.00 in`, Right `0.00 in`, Top `0.50 in`, Bottom `1.00 in` |
| **ArtEffect 2** | ✅ Working | Same revision-16 PWG Raster environment | Confirmed still printing after the Wordworth and multi-page boundary fixes | Density `4`; Brightness, Contrast and Gamma `0`; working image size `188x176 mm`; both dimensions must remain smaller than the selected paper |
| **DPaint V** | ❌ Not working | Same revision-15 test machine | Printing crashes DPaint with Software Failure `#8000000A` | No working setup confirmed; capture `T:MintPRINT-driver.log` from the failed attempt |
| **MultiView** | ✅ Working | AmigaOS 3.2.3, same revision-15 test environment; OS Printer Preferences left at defaults apart from selecting MintPRINT | Prints successfully using the active MintPRINT preferences | Select **Print**; MultiView provides no application-specific print settings |
| **GfxDump** | ✅ Working | AmigaOS 3.2.3, same revision-15 test environment | The OS tool sends its graphics dump directly through `printer.device` to MintPRINT and prints successfully | Select MintPRINT in OS Printer Preferences; no application-specific setup |
| **Directory Opus 4.16** | ❌ Not supported yet | AmigaOS 3.2.3, same revision-15 test environment | The Print button opens and closes MintPRINT but produces no raster page and no IPP job | Requires a future Amiga text-line renderer fed by `ped_ConvFunc()` characters from the `CMD_WRITE` path |
| **AmigaWriter** | ✅ Working | AmigaOS 3.2.3, Roadshow, Brother HL-L2350DW, 1.1.0 | Multi-page documents print correctly with the page-boundary fix confirmed via a beta build in issue #8 and shipped fully in 1.1.0 | Tested with the defaults after adding the printer (`Scaling=auto`) |
| **MintPrint Settings Test Print** | 🟡 Partial | Brother HL-L2350DW report | The centre of the test image remains enlarged and cropped with `Scaling=auto` | No working override confirmed yet |

### Wordworth 7 Print Setup

Use driver revision **40** or newer for the current diagnostic strip-printing
path.
Revision 16 first preserved Wordworth's strip printing as one media-sized PWG
page and prevented trailing narrow graphics dumps from becoming a second IPP
job. Revision 36 additionally separates logical pages before physical media
padding; revision 39 restores pre-dump vertical movement when supplied and
otherwise retains revision 38's documented Wordsworth form-length fallback.

Set Wordworth 7's **Print Setup** window to:

These values were originally recorded in
[issue #9](https://github.com/boingball/MintPRINT/issues/9) and confirmed again
with the revision-16 physical print tests.

```text
Printer Driver: MintPRINT
Print Method:   Normal
Paper Type:     Sheet Feeder
Density:        7

Print Borders:
  Left:         0.00 in
  Right:        0.00 in
  Top:          0.50 in
  Bottom:       1.00 in
```

The confirmed matching MintPRINT configuration was:

```text
Engine:         PWG Raster
DPI:            300
Media:          iso_a4_210x297mm
Tray/source:    auto
Scaling:        auto
Quality:        high
Print mode:     color
```

The physical revision-15 test produced one `2478x3505`, 300-DPI portrait PWG
page. ArtEffect 2 was retested afterwards and continued to print correctly.

### ArtEffect 2 Print settings

ArtEffect 2 must be given an output size **smaller than the selected physical
page in both dimensions**. Using a size that reaches or exceeds the page size
can prevent the print from working. The confirmed A4 settings were:

```text
Density:        4
Brightness:     0
Contrast:       0
Gamma:          0
Width:          188 mm
Height:         176 mm
```

Density `7` did not work in the reported test; Density `4` produced the
confirmed print. Treat `188x176 mm` as a known-working A4 starting point rather
than automatically expanding an image to `210x297 mm`.

### DPaint V

DPaint V currently crashes when printing through MintPRINT with AmigaOS
Software Failure `#8000000A`. This is the same signature as the historical
DPaint failure, not a newly identified Wordworth revision-15 regression.

DPaint can invoke `printer.device` from an Exec Task rather than a normal DOS
Process. MintPRINT already moves its DOS, file and network work into a spool
Process, but the reproduced crash shows that the DPaint Task path is not yet
fully isolated. Stock PostScript printing from DPaint worked in an earlier
comparison, so this entry remains a MintPRINT driver compatibility defect rather
than an application setting problem.

The next report should include `T:MintPRINT-driver.log` from the exact failed
attempt and note whether the crash happens immediately after selecting Print,
during disk or network activity, or after a progress requester. A partial log is
useful: its last completed callback will identify whether the remaining fault is
in driver open, first render, spooling or close/submit teardown.

### Directory Opus 4.16

Directory Opus 4.16's Print button currently produces no output. The
revision-15 trace shows MintPRINT loading successfully and repeating:

```text
Render pre-master special/maxX/maxY 256 4096 6144
Config ...
engine=pwg-raster
Open
Close
Expunge
```

There is no `Render begin`, raster-row callback, document encoder start or IPP
submission. `special=256` is the normal Density 1 flag, not an error. This means
the button is not submitting a graphics dump to MintPRINT; its behaviour is
consistent with the legacy alphanumeric `PRT:`/`CMD_WRITE` route.

MintPRINT is currently graphics-focused: all entries in its text command table
are marked unsupported and `DoSpecial()` emits no printer bytes. DOpus 4.16 is
therefore **not supported yet**. Supporting this function requires a future
Amiga text-line renderer fed by characters intercepted through
`ped_ConvFunc()` from the `CMD_WRITE` path, then passed to the existing document
engines. It is not a printer, IPP, PWG or application-settings fault.

## Baseline setup and troubleshooting

1. Install the correct package: the classic build for AmigaOS 3.1, or the
   normal build for AmigaOS 3.2/3.5/3.9 and newer printer.device versions.
2. Reboot after installing or updating the driver. If the old revision remains
   loaded, fully power-cycle the Amiga.
3. Query the printer before saving so MintPRINT learns its actual formats,
   media, DPI, scaling, quality and colour options.
4. Prefer port `631` and the printer's reported IPP path. `/ipp/print` is the
   confirmed path for every printer currently listed here.
5. Start with `Scaling=auto` when the printer advertises it. It is the best
   current cross-printer baseline, not a universal guarantee.
6. Select an engine the printer advertises, but treat JPEG without reported
   JPEG constraints as suspicious rather than proven.
7. Test from MintPrint Settings or MultiView first. They provide a simpler
   baseline than Wordworth, ArtEffect or other strip-printing applications;
   then check the application table above for any required setup.
8. Enable Debug only while diagnosing. Attach `T:MintPRINT-driver.log` and the
   retained `T:MintPRINT-job.*` file to the report.

## Add or update a printer report

Open a [GitHub issue](https://github.com/boingball/MintPRINT/issues) and include
the following. Unknown fields should say `not reported` rather than being
guessed.

```text
Printer make/model:
Printer firmware (if known):
MintPRINT version and driver revision:
Amiga model/accelerator/network card:
AmigaOS and Kickstart version:
TCP/IP stack and version:
Wired or Wi-Fi:
IPP port and path:
Engine:
DPI:
Media and tray/source:
Scaling:
Quality:
Colour/print mode:
Application used for the test:
Application version:
Application Print Setup:
Portrait/landscape:
Physical result:
```

Also attach:

- `T:MintPRINT-driver.log` from the test;
- the retained `T:MintPRINT-job.jpg`, `.pwg`, `.pdf` or `.ps` when Debug was
  enabled; and
- `windows_ipp_probe.py --all --validate-mintprint` output from a computer on
  the same network.

The page should only be updated to ✅ Working after physical output has been
confirmed.
