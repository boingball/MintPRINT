# MintPRINT printer compatibility

This page records real MintPRINT hardware results: the printer, AmigaOS
version, TCP/IP stack, document engine and the settings needed to reproduce a
working print. It deliberately distinguishes physical output from an IPP job
that merely reports success.

Last reviewed: **21 August 2026**

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
| **Brother MFC-J6930DW** | ✅ Working | Not recorded | Not recorded | PWG Raster | Port `631`; path `/ipp/print`; `300 dpi`; A4; tray `auto`; scaling `auto`; quality `draft`; colour |
| **Brother HL-L2350DW** on A500 PiStorm, Wi-Fi | ✅ Working | 3.2.3 | Roadshow | Original Aminet release reported as working perfectly; exact engine was not recorded | No printer-specific override reported |
| **Brother HL-L2350DW** on A4000, CSMkII 060/50 and Ariadne-II, wired | 🟡 Partial | 3.2.3 | Roadshow | A simple AmigaWriter document now prints perfectly with MintPRINT 1.0.3 and scaling `auto` | An unexpected Brother direct-print error sheet follows some jobs; MintPRINT Test Print is still enlarged/cropped |
| **Canon TS8360** (IPP identifies it as **TS8300 series**) | 🟡 Partial | 3.2.3 | Not reported | PWG Raster and JPEG print colour pictures correctly in portrait and landscape; text/application cases remain incomplete | Port `631`; path `/ipp/print`; printer reports `600 dpi`, A4, source `auto`, quality `draft`, scaling `auto`; Query fix in PR #16 awaits confirmation |
| **Samsung C480W / C48x Series** | ❌ Current release / 🧪 PostScript build | 3.9 Boing Bag 2; Kickstart 3.1 | Not reported | JPEG is silently discarded; PWG Raster and PDF are rejected. External one-shot PostScript printed; MintPRINT PostScript PR #17 awaits confirmation | Port `631`; path `/ipp/print`; `300 dpi`; A4; tray `tray-1`; normal quality; scaling `auto`; allow 3–4 minutes for PostScript |

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

### Brother HL-L2350DW

Two systems were reported against the same printer in
[issue #8](https://github.com/boingball/MintPRINT/issues/8):

- **A500 PiStorm, AmigaOS 3.2.3, Roadshow over Wi-Fi:** the original Aminet
  release discovered the printer immediately and printed documents correctly.
  The exact engine and MintPRINT job settings were not included in the report.
- **A4000, CSMkII 060/50, Ariadne-II wired, AmigaOS 3.2.3, Roadshow:**
  MintPRINT 1.0.3 with scaling `auto` printed a simple AmigaWriter document
  perfectly. This confirms the earlier multi-band application rendering fault
  is substantially fixed. Two separate defects remain:
  - some jobs are followed by a second sheet containing the Brother-generated
    Swedish error `-data som inte stöds för direktutskrift: 3000` (“-data not
    supported for direct printing: 3000”); and
  - MintPrint Settings' Test Print still prints only the middle portion of an
    enlarged image filling the sheet.

  The exact result is recorded in
  [issue #8](https://github.com/boingball/MintPRINT/issues/8#issuecomment-5371708419).
  `fit` and `auto-fit` had previously corrected text size but split the document
  over multiple vertically-centred pages, so `auto` remains the best setting
  for AmigaWriter while the two remaining bugs are investigated.

The Roadshow result confirms the TCP stack is viable; the A4000 problem is in
rendering/page handling rather than basic IP connectivity.

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

Confirmed with MintPRINT 1.0.3:

- A JPG picture prints in colour using either PWG Raster or JPEG.
- The picture prints correctly in both portrait and landscape.
- PWG Raster portrait text fills the page, but was monochrome.

Still failing or awaiting retest:

- PWG Raster landscape text produced a blank page.
- JPEG text produced a blank page in both orientations.
- Query could hang and leave Media, Scaling, Quality and Print Mode disabled.
  [PR #16](https://github.com/boingball/MintPRINT/pull/16) contains the current
  Canon response-parser fix and needs confirmation on this printer.

Do not describe the Canon as fully working yet. It is usable for colour-image
printing, but the application/text and Query paths are not confirmed fixed.

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
| PostScript / `application/postscript` | A one-shot external IPP Print-Job physically printed and incremented the page counter. MintPRINT's PostScript engine in [PR #17](https://github.com/boingball/MintPRINT/pull/17) is awaiting hardware confirmation. |

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

## AmigaOS and TCP/IP stack status

| Environment | Status |
|---|---|
| AmigaOS 3.2.3 + Roadshow, A500 PiStorm Wi-Fi | ✅ Confirmed end-to-end with Brother HL-L2350DW |
| AmigaOS 3.2.3 + Roadshow, A4000/Ariadne-II wired | 🟡 AmigaWriter printing now confirmed with scaling `auto`; an extra direct-print error sheet and oversized Test Print remain |
| AmigaOS 3.9 BB2, TCP stack not reported | 🟡 IPP transport reaches Samsung C480W; no released MintPRINT engine currently prints on it |
| AmigaOS 3.1 classic driver | 🧪 Structurally implemented but no physical OS3.1 print is recorded yet |
| AmiTCP | 🧪 Expected through compatible `bsdsocket.library`; no named hardware report yet |
| Miami | 🧪 Expected through compatible `bsdsocket.library`; no named hardware report yet |

MintPRINT requires a working `bsdsocket.library`-compatible TCP/IP stack.
MintPrint Settings checks for `bsdsocket.library` V4 and a usable socket before
opening. Roadshow, AmiTCP and Miami are supported targets, but only Roadshow has
a named community hardware result so far.

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
   baseline than Wordworth, ArtEffect or other strip-printing applications.
8. Enable Debug only while diagnosing. Attach `T:MintPRINT-driver.log` and the
   retained `T:MintPRINT-job.*` file to the report.

### Wordworth 7 and ArtEffect

Use MintPRINT 1.0.3 or newer so strip printing via
`SPECIAL_NOFORMFEED` is assembled into one page. The confirmed Amiga Printer
Preferences shown in [issue #9](https://github.com/boingball/MintPRINT/issues/9)
use:

```text
Printer Driver: MintPRINT
Print Method:   Normal
Paper Type:     Sheet Feeder
Density:        7
Borders:        Left 0.00 in, Right 0.00 in, Top 0.50 in, Bottom 1.00 in
```

Application compatibility is separate from printer compatibility. A printer
that works from MultiView can still expose an application-specific page-band
or orientation bug.

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
