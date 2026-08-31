# MintPRINT release history

[Back to MintPRINT](README.md) · [Aminet readme](Aminet/MintPRINT.readme)

Detailed release notes, newest first, consolidated from the project front
page and Aminet readme. Older entries describe the behaviour and testing at
that point in development; later entries may supersede them. For current
hardware and application test results, see
[printer compatibility](docs/PRINTER_COMPATIBILITY.md).

Keep the full version-by-version history here. The front page and Aminet
readme should carry only a short summary of the latest version, with a link
to this page; replace that summary when preparing the next release.

[Unreleased](#unreleased) | [1.3.0](#130) | [1.2.7](#127) | [1.2.6](#126) | [1.2.5](#125) | [1.2.4](#124) | [1.2.3](#123) | [1.2.2](#122) | [1.2.1](#121) | [1.2.0](#120) | [1.1.0](#110) | [1.0.3b](#103b) | [1.0.3](#103) | [1.0.2](#102) | [1.0.0](#100)

## Unreleased

- **Strict Apple Raster page-header compatibility (driver 41.14).** The URF
  header now uses CUPS's canonical simplex/short-edge/long-edge values
  `1/2/3` and writes the selected Draft/Normal/High quality as `3/4/5`
  (defaulting to Normal), instead of the unofficial `0/1/2` mode mapping
  and unspecified quality `0`. This targets the HP Color LaserJet
  M255/M256 parser failure reported in
  [issue #81](https://github.com/boingball/MintPRINT/issues/81): that printer
  advertises `PQ3-4-5` and returned `PARSER / Not Implemented` for the old
  one-sided header. Byte-exact host regressions cover all modes and quality
  values. The Windows probe also recognizes URF as a valid multi-page duplex
  transport. Physical output on the reporting HP remains to be confirmed.
- **Graphics form-feed/reset page boundaries (driver 41.13).** A processed
  form-feed or `aRIS` reset now finalizes any pending
  `SPECIAL_NOFORMFEED` graphics page before the following page begins. This
  closes the gap where a short FinalWriter/Wordsworth page that had not
  reached a media or short-strip heuristic could absorb the next page. The
  existing engine finalizer is used, preserving duplex streams, Wordsworth
  margin reconstruction and FinalWriter's stable variable-width canvas;
  redundant boundaries and orphan tiny control bands create no blank job.
  Page-finalization failures are latched and later dumps refused until the
  driver is closed and reopened. Inspired by Andreas Stürmer's equivalent
  graphics-boundary handling in AmiAirPrint 1.2, shared with permission.
- **Test Print page placement with Media set to auto.** Removed redundant
  printer.device centring from the full-page JPEG/PWG/PDF/URF test request.
  An OS 2.04 capture showed it adding 809 blank columns before the image,
  producing a 3287x3508 raster for an A4-sized request. Explicit destination
  dimensions and aspect handling remain, as do saved media settings, the
  separate PostScript test layout and application-driven printing. A fresh
  OS 2.04 capture and physical print are still required to validate the fix.
- **More accurate Test Print completion text.** Release the printer device
  before reporting completion, allowing pending/duplex submission to finish.
  Preserve reported I/O errors, but no longer claim successful delivery just
  because printer.device returned zero: the OS 2.04 fake-IP capture returned
  zero despite a driver-side IPP timeout. The message now directs users to
  printer output, retained job status or the Debug driver log.

## 1.3.0

MintPRINT 1.3.0 is the spooler and compatibility milestone. The GUI now runs
on AmigaOS 2.04-era systems as well as later releases, and retained jobs can
be managed from the new Spooler window. The driver revision is **41.12**.

- **Disk-backed spooler management.** Choose RAM/T: for the traditional
  behaviour, or a mounted hard drive for a hidden `MPSPOOL` directory. Enable
  **Keep Jobs (HDD)** to retain completed and failed jobs with unique names,
  live status sidecars, retry, reprint, copies, delete and printer
  reassignment from **View Spooler**.
- **Lower memory requirement with HDD spooling.** With RAM/T: spooling, allow
  roughly 2 MB of free RAM for a full-page print. Moving the rendered document
  to an HDD can make a 1 MB system practical, although the GUI, row buffer,
  encoder scratch and TCP/IP stack still require contiguous memory. Retained
  spool storage is limited by free HDD space and normal filesystem file-size
  limits, not by RAM.
- **Strip-printing page buffering across all engines.**
  JPEG, PDF and PostScript now buffer `SPECIAL_NOFORMFEED` bands on disk until
  the complete page height is known, preventing one line per sheet and
  sideways-looking pages. PWG Raster and Apple Raster retain their existing
  accumulation paths.
- **AmigaOS 2.x compatibility.** The classic pre-V44 build and settings GUI
  now use the older library interface required by AmigaOS 2.04/2.1-era
  systems. The classic driver remains the correct choice through AmigaOS 3.1;
  V44+ systems use the extended build.
- **Older Workbench graphics improved.** Printer and duplex artwork is kept
  readable on constrained 16-colour screens, with grayscale-style pen
  selection instead of garish palette colours.
- **FinalWriter 97 variable-width strips (driver 41.12).** Bands cropped to
  different widths now share a stable page canvas and retain correct logical
  page boundaries. A retained-job test produced two complete pages without a
  rendering failure; physical FinalWriter output is not yet confirmed.
  WordWorth 7's fixed-width strip path was regression-tested. See the
  [application compatibility notes](docs/PRINTER_COMPATIBILITY.md).
- **Workbench launch stack increased to 256 KiB** for discovery, printer
  selection and OS 2.x use. Use the supplied application icon.
- **Smoother printer artwork on RTG screens.** Neutral dithering is restricted
  to classic planar displays; RTG bitmap detection uses the effective bitmap
  attributes rather than trusting the legacy depth field.
- **Help viewer fallback for AmigaOS 2.x.** Help uses MultiView when installed,
  otherwise the standalone AmigaGuide viewer, and reports missing viewers or
  guide files instead of silently failing.

## 1.2.7

MintPRINT 1.2.7 is a driver bug fix: **`PRT:`/CLI text printing now honours
real duplex.** Driver revision **41.4**.

- **Fixed two-sided printing of plain text.** Printing via `PRT:`/`CMD_WRITE`
  (e.g. `type file >PRT:` or AmigaDOS `Type ... TO PRT:`) with `Sides` set to
  two-sided-long-edge or two-sided-short-edge still produced a stack of
  separate one-sided sheets instead of a real duplex print - see
  [issue #68](https://github.com/boingball/MintPRINT/issues/68). The text
  path (`driver/command_table.c`) submitted one independent IPP job per page
  with `sides=` hardcoded to `one-sided`, a stopgap the code had carried
  since text printing was first added, explicitly pending real-hardware
  duplex coverage. `mp_text_print_document()` now detects a duplex request
  on the PWG Raster engine and accumulates the whole document into one
  multi-page job (mirroring the graphics path's existing duplex handling in
  `driver_core.c`) instead of one job per page, submitting once with the
  real `sides=` attribute; backside pages that need PWG's reverse feed order
  are buffered and replayed back-to-front the same way the graphics duplex
  path already does. Non-duplex text jobs and non-PWG engines are
  unaffected. **Confirmed fixed on real hardware** by the issue #68
  reporter.

## 1.2.6

MintPRINT 1.2.6 is a driver bug fix: **Monochrome now actually prints in
black and white.** Driver revision **41.3**.

- **Fixed monochrome jobs printing in colour.** `Printer Engine`'s colour
  mode (`COLOR=` in the saved config) was only ever forwarded to the
  printer as the IPP `print-color-mode` job attribute - a *request* the
  printer's firmware can honour or ignore. The actual raster/JPEG pixel
  data sent alongside it stayed full colour regardless, across all five
  engines (JPEG, PostScript, PWG Raster, PDF, URF), since none of them had
  any grayscale handling at all. On hardware that doesn't strictly honour
  the hint, a "monochrome" job printed in colour - confirmed on real
  hardware. `mp_job_write_row()` (`driver/driver_core.c`) now desaturates
  the row itself (integer ITU-R BT.601 luma, no floating point) whenever
  `COLOR=` is `monochrome`, `auto-monochrome`, `bi-level`,
  `process-bi-level` or `process-monochrome` - the PWG5100.3/RFC 8011
  keywords that mean "black ink only" - before it reaches any encoder, so
  the output is black and white regardless of whether the printer itself
  respects the IPP hint.

## 1.2.5

MintPRINT 1.2.5 is a maintenance release: safer defaults, a driver that no
longer hangs on an unresponsive printer, correct AmigaOS 3.9 detection, and
several smaller GUI fixes and cleanups found during an external audit and
real-hardware use. Driver revision **41.2**.

- **No more indefinite driver hangs on a dead/unresponsive printer.**
  `driver/ipp_client.c`'s `connect()`/`recv()`/`send()` were plain blocking
  calls with no timeout - a printer that accepted a connection or a job and
  then went silent (see
  [issue #66](https://github.com/boingball/MintPRINT/issues/66), a Samsung
  C480W hanging on a valid `image/urf` job) could hold the spool process,
  and the application printing through it, forever. All three are now
  timeout-bounded (8s connect, 20s read/write). A timeout also gets its own
  result codes (`-17`/`-18`) and a plain-English debug log line, distinct
  from an ordinary response failure.
- **Fresh installs no longer probe a hardcoded LAN address.** Both
  MintPrint Settings and the driver's own config defaults used to fall
  back to `192.168.0.51:80` when nothing was configured yet, so an unset
  Unit0 could still send a startup Query or even a print job to some other
  device on the network. Both now default to empty and simply do nothing
  until a real address is set.
- **Correct AmigaOS 3.9 detection.** Driver/GUI selection used to read
  `exec.library`'s version, which stays at whatever the Kickstart ROM
  shipped with on a software-only OS update - a real 3.9 system with a 3.1
  ROM read back as "3.1" and got the wrong (classic pre-V44) driver build.
  Now reads `workbench.library`'s version instead (falling back to
  exec.library only if that can't be opened at all), in both MintPrint
  Settings and the `Install` script.
- **Atomic driver install/update.** Copying a new driver into
  `DEVS:Printers/MintPRINT` used to truncate the destination file
  immediately on open; an allocation failure, short write, or full disk
  partway through could destroy a previously-working driver with no way
  back. It now copies to a temp file, verifies the full size landed, then
  swaps it in via a rename-old-aside/rename-new-in/delete-old dance -
  restoring the previous file if the final rename itself fails.
- **Help menu now actually opens the guide.** "MintPrint Settings Help..."
  silently did nothing: it built its Multiview command line with the
  literal string `PROGDIR:MintPrintSettings.guide`, but `PROGDIR:` is a
  local assign scoped to the process that has it, not inherited by the new
  process `SystemTags()` spawns to run Multiview. Now resolves `PROGDIR:`
  to a real path before spawning.
- **Live printer status next to the ink/toner strips.** Query already
  requested and decoded `printer-state` but never showed it anywhere;
  `printer-state-reasons` wasn't even requested. Now shown as a short
  word/phrase - Ready, Busy, Stopped, or a specific problem (Jam, Door
  Open, Toner Empty, Out of Paper, Supply Low) - right on the "Ink/Toner:"
  row.
- **Faster duplex-hint art loading.** The 32x32 duplex icons
  (`single.iff`/`longside.iff`/`shortside.iff`) are decoded by a separate
  translation unit (`iff-loader.c`) whose debug `printf()`/`puts()` calls
  bypassed MintPrint Settings' own Workbench-safe output redirect and hit
  real stdio instead - likely forcing a hidden console open on a
  Workbench launch, reported as sluggish loading on real hardware.
  Removed. The per-pixel nearest-screen-pen colour match for that same
  icon is also now cached instead of recomputed on every redraw.
- **GUI stack reduced from 384 KiB to 128 KiB.** The 256 KiB IPP response
  buffers behind that figure are heap allocations now, not stack; `__stack`,
  the `$STACK:` cookie, and the Workbench icon's `do_StackSize` all move
  together.
- Relabeled "Printer IP/Host" to "Printer IPv4" - both the GUI and driver
  only ever resolve it with `inet_addr()`, so a hostname was never
  actually supported.
- Fixed a `return;` in the Query button's event-loop handler that closed
  the entire Settings window on an invalid address instead of just
  reporting the error.
- Fixed garbled AmigaGuide help text - `@wordwrap`/`@smartwrap` were
  reflowing hand-formatted, already-wrapped paragraphs and splicing their
  hanging-indent whitespace into the middle of lines. Removed; every node
  was already wrapped to fit the declared `@width`.
- Added `make check` (the existing test suite plus HTTP/IPP-enum/
  PostScript coverage it skipped) and a GitHub Actions workflow that runs
  it on every push/PR.

## 1.2.4

MintPRINT 1.2.4 is mainly a packaging/build change: one distributable
drawer instead of two, with automatic AmigaOS-version detection choosing
the right driver, plus a real-hardware ink/toner colour fix found while
testing it. Driver behaviour is unchanged (still driver revision 41.1).

- **One MintPRINT drawer for every supported AmigaOS release.** The
  separate `MintPRINT` / `MintPRINT-OS31` release bundles are merged into a
  single `MintPRINT/` drawer, with both compiled drivers staged under
  `Drivers/MintPRINT-V44/` (AmigaOS 3.2, 3.5, 3.9+) and
  `Drivers/MintPRINT-OS31/` (AmigaOS 2.04 through 3.1 classic) - see
  `docs/OS31_SUPPORT.md`. There is no longer a "which archive do I
  download" question.
- **Automatic driver selection.** MintPrint Settings detects the AmigaOS
  version at startup (`mp_os_version()` / `mp_needs_os31_driver()` /
  `mp_driver_src_path()` in `src/MintPrintSettings.c`) and installs or offers
  to update the matching `Drivers/MintPRINT-<variant>/MintPRINT`.
  This release initially used `exec.library`; 1.2.5 changed detection to
  `workbench.library` so software-only OS updates such as AmigaOS 3.9 are
  identified correctly. The detected AmigaOS
  version and chosen driver are shown in the install/update prompts and the
  About box (`mp_describe_amiga_os()`), so the choice is never a silent
  guess.
- **New `Install` script.** A classic AmigaDOS Installer script at the
  repository root offers the same auto-detect-then-confirm flow (asking
  "what AmigaOS/printer.device generation is this?" with the detected
  answer pre-selected) for anyone who prefers that install path over
  running MintPrintSettings first.
- **Ink/toner bars now use the printer's actual reported colour, and keep
  it.** Two bugs, found back to back on real hardware. First,
  `ObtainBestPenA()` only ever reuses whatever pen already on screen was
  closest to the requested colour, and on a screen with few free colour
  registers (e.g. a default 32-colour Workbench) with no true
  cyan/magenta already present, several markers snapped to the same wrong
  pen (both cyan and magenta bars turned yellow) - fixed by trying
  `ObtainPen()` first, which only succeeds by setting a genuinely free pen
  to the exact requested RGB. That fix then exposed a second, worse bug:
  each marker released its pen immediately after drawing, so the very
  next marker's `ObtainPen()` could reclaim that same shared colour
  register and repaint it - instantly recolouring every bar already drawn
  with that pen index, not just the new one (visible as all four bars
  correctly coloured for an instant, then collapsing to whichever colour
  was requested last). Pens are now held for as long as their colour needs
  to stay on screen and released only just before the next redraw, or at
  shutdown.
- `make release` now builds both drivers and stages the single combined
  bundle in one step; the old separate `make release31`/`make release-all`
  targets are gone since there is only one bundle to build.
- Shortened OS-detection text in the install/update and About requesters so
  a long driver path does not make the requester wider than a low-resolution
  Amiga screen.

## 1.2.3

MintPRINT 1.2.3 brings back printer ink/toner status in MintPrint Settings'
Query flow while keeping the compact 520px-wide main window. Driver behaviour
is unchanged (still driver revision 41.1).

- **Ink/toner level display.** Query now also requests the IPP Printer MIB
  `marker-names`/`marker-colors`/`marker-types`/`marker-levels`/
  `marker-low-levels`/`marker-high-levels` attributes (RFC 3805 /
  PWG5100.13). Up to six reported markers are shown in a compact 2x3 panel
  inside the existing window, using the spare area beside IPP Path / Printer
  Engine / Debug and above Media. Each marker gets a short label, percentage
  and coloured level bar using the printer-reported `#RRGGBB` or named colour
  via `ObtainBestPenA()`. Unknown levels remain visibly empty rather than
  being hidden.
- **Compact settings layout.** The main GadTools controls were rearranged to
  use the available 520px width more efficiently: Unit spans the top row,
  Discover sits beside Printer IPv4, Query beside Printer Model, and DPI /
  Sides share the lower option rows. The status/output area remains unchanged.

## 1.2.2

MintPRINT 1.2.2 switches driver version tracking to a real AmigaOS `$VER:`
string, **driver revision 41.1** - no driver behaviour change, so no
version-number bump for the driver's own build count.

- **Real `$VER:` driver versioning.** `DEVS:Printers/MintPRINT` now embeds
  a standard `$VER: MintPRINT 41.1 (dd.mm.yyyy)` string, readable by
  AmigaOS's own `Version` command and Workbench's Information requester -
  the same convention MintPrint Settings' own `$VER:` line already used.
  This replaces the earlier ad-hoc `MPDRVREV:<n>` marker, which existed
  only for MintPrint Settings' own bundled-vs-installed update check and
  wasn't understood by anything else. The driver's own version now has two
  parts, matching real Amiga version.revision pairs (e.g. Workbench 3.9's
  `47.102`): `MP_DRIVER_REV` (the version half, `driver_core.c`) and
  `MP_DRIVER_SUBREV` (the revision half) - so "driver revision" is never
  just a bare integer that could be misread as printer.device's own fixed
  V44 PrinterSegment ABI marker in the same file. Going forward the
  revision half is what moves for an ordinary driver rebuild - 41.1, 41.2,
  41.3, and so on, the same way a real Amiga library keeps one version
  number across many small revisions - and the version half only bumps for
  something that warrants a new version number outright. MintPrint
  Settings still automatically offers to update an out-of-date installed
  driver at startup (About and Test Print show the same version), now
  parsing and comparing the real `$VER:` version.revision pair instead of
  the old flat counter.

## 1.2.1

MintPRINT 1.2.1 currently carries **driver revision 41**, adding a new engine
and the following Wordsworth strip-printing fixes:

- **PRT: text/URF and cross-engine margin fixes** (driver rev 41, not yet
  physically re-tested). Two bugs found reviewing revisions 35-40 before
  merge: `PRT:`/`CMD_WRITE` plain-text printing had no `ENGINE=urf` case in
  `command_table.c`'s own engine dispatch, so it silently fell through to
  JPEG and submitted `image/jpeg` - which fails outright on a URF-only
  printer such as the OKI B412 (the whole reason this engine exists).
  Separately, the rev 37-39 top-margin restoration computes and writes
  leading whitespace for *any* `SPECIAL_NOFORMFEED` job regardless of
  engine, but the helper it called (`mp_job_reserve_page()`/
  `mp_job_pad_page()`) only understood PWG and URF, silently writing into
  the unopened PWG encoder instead of whichever encoder a JPEG, PDF or
  PostScript job actually used. Both now dispatch across all five engines.
- **Wordsworth page borders** (driver rev 39). The driver now processes and
  accumulates pre-dump `aIND`, `aNEL` and literal line-feed movement using
  printer.device's 1/216-inch VMI, then emits the corresponding leading white
  raster rows. A rev 37 trace had proved that
  Wordsworth clears margins and sends only a 70-line form length, not its
  separate Print Borders values through the margin commands; rev 38 therefore
  added a form-length/physical-media fallback which restores the documented
  0.50-inch top border (150 rows at 300 DPI) when no real vertical movement or
  explicit `aSTBM` placement appears. The media-height finalizer supplies the
  remaining bottom border.
- **Complete legacy layout diagnostics** (driver rev 40). Debug logs now
  record every printer layout command from `aVERP0` through `aCAM`, both
  parameter slots, live line/VMI/CRLF state, and every raster band's complete
  `IODRPReq` source/destination geometry. This exposes any placement signal
  outside the commands already observed without changing rev 39's output.
- **Wordsworth logical page boundaries** (driver rev 36). A terminal short
  strip now ends the application's printable page before the physical-media
  padding, preventing the first 443 rows of the next page from being pulled
  onto the previous sheet.

- **Apple Raster (`ENGINE=urf`) engine.** A new backend for printers that
  advertise `image/urf` but none of MintPRINT's other formats (JPEG,
  PostScript, PWG Raster, PDF) - reported for the OKI B412 in
  [issue #60](https://github.com/boingball/MintPRINT/issues/60). See
  `docs/URF_ENGINE.md`. One-sided printing confirmed physically (Brother
  MFC-J6930DW, driver rev 30); the OKI B412 report that motivated it is
  still pending its own confirmation.
- **URF duplex** (driver rev 31, three bugs found and fixed by rev 34).
  `ENGINE=urf` now supports two-sided printing the same way PWG Raster
  does - one multi-page stream, submitted once from `DriverClose()` -
  using Apple Raster's own duplex/tumble page header byte. Unlike PWG
  Raster, no backside row/column reversal is performed (Apple Raster's
  page header has no equivalent transform fields); see
  `docs/DUPLEX_PRINTING.md`. Its first three real tests (Brother
  MFC-J6930DW) each hit a real bug rather than confirming duplex itself,
  every one rejected by the printer before any paper came out: first, URF
  had no strip-printing band accumulator, so a strip-printed document
  turned into dozens of bogus single-band "duplex pages" (62 instead of a
  handful) - fixed in rev 32 by extending PWG Raster's own accumulator to
  cover URF; then, once fixed, the two resulting real pages came out at
  different heights (front content naturally overshot the media-derived
  target while the back only got padded to it) - fixed in rev 33 by
  flooring every later page's target at the tallest page the job has
  finalized so far; then, with both pages byte-perfect and an exactly
  matching height, the printer still rejected the job. Revision 34 changed
  CUPS's 1/2/3 mode mapping to an unofficial 0/1/2 mapping after comparison
  with an HP DesignJet reverse-engineering report. That Brother later accepted
  duplex, but driver 41.14 supersedes the mapping after a strict HP Color
  LaserJet rejected simplex value 0; current builds again follow CUPS's 1/2/3.
  A fourth test on rev 34 hit the same strip-accumulator page-boundary bug
  PWG Raster hit (see the strip-printing fixes below); once revs 35-36
  landed, **two-sided long-edge duplex over `ENGINE=urf` is physically
  confirmed** on this same Brother MFC-J6930DW.
- **Strip-printing page-boundary fixes** (driver revs 35-36). Found
  testing PWG Raster one-sided on the same printer, not URF or duplex
  specifically: a two-page Wordworth document printed with no IPP error at
  all, but visibly corrupted - the page boundary the accumulator chose fell
  in the middle of real content (confirmed by decoding and rendering the
  job file: a character cut off at the top of the second sheet). The
  rev-35 accumulator split at the 3505-row physical A4 boundary, but a real
  retest proved Wordworth's page cycle ends earlier at 3062 rows (thirty
  100-row bands plus a 62-row terminal remainder); rev 35 therefore still
  stole 443 rows from page 2. Rev 36 recognises that short terminal band as
  the logical printable-page boundary, including when it arrives as a narrow
  auxiliary dump, finalises/pads the physical page there, and retains the
  media-height split as a fallback for applications without that signal.
  Affects PWG Raster and URF identically, one-sided or duplex. Revs 37-39
  then restored the top margin that pagination fix had exposed as missing
  (see the Wordsworth page borders bullet above); **a physical retest on
  rev 40 confirmed Wordsworth's PWG Raster page boundaries and margins now
  print correctly**.

## 1.2.0

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
- Fixed the `300*` DPI compatibility option so it appears immediately after
  the first Query on PWG Raster printers that omit 300 DPI from their
  advertised resolution list.
- Fixed IPP enum parsing for `print-quality` and `printer-state`, and reject
  failed Get-Printer-Attributes HTTP/IPP responses instead of accepting them
  as capability data.
- Added built-in AmigaGuide help, available from MintPrint Settings' Help menu
  and included in the release bundles.

PWG Raster remains the preferred engine when a printer supports it: it is much
cheaper to encode on a classic Amiga than JPEG/PDF/PostScript and is therefore
still selected by default where available.

## 1.1.0

PostScript engine, faster JPEG, and more reliable wireless setup.

- Added a fourth output engine, PostScript, for printers (e.g.
  Samsung C480W) that advertise IPP JPEG support but silently
  discard the job; it reuses the existing JPEG encoder internally,
  so it benefits from the speed-up below too
- Query/Discover now wait long enough for Wi-Fi printers that
  power down their radio between jobs (HP OfficeJet/Envy series
  confirmed) to wake up, instead of giving up after one attempt
- Fixed printer.device occasionally reporting an oversized page
  width for a single-shot page (observed producing a page ~30%
  wider than the configured media); the fix now correctly tells
  a genuinely oversized page apart from a legitimate landscape
  one, and covers the JPEG and PDF engines as well as PWG Raster
- Fixed the test page's Wi-Fi-driven-oversized-page fix leaving a
  blank left margin and clipping the right edge on some hardware
- PWG Raster (the cheapest engine to encode) is now the default
  whenever a printer advertises it, instead of always defaulting
  to JPEG
- Replaced the JPEG encoder's page-width DCT with a much faster
  algorithm (fewer than a tenth of the multiplications for the
  same result), significantly speeding up JPEG and PostScript
  jobs on real 68k hardware
- Test Print now runs asynchronously so the window stays
  responsive while printing, and uses the current Workbench
  screen's own palette instead of a fixed one, fixing a
  toner-wasting grey background on at least one laser printer

## 1.0.3b

Page-boundary fixes and clearer setup options.

- Fixed multi-page PWG jobs being combined into one oversized
  raster when applications left SPECIAL_NOFORMFEED set; complete
  media-sized pages and narrow end-of-page auxiliary dumps now
  provide safe physical-page boundaries
- Preserved media-height padding for genuine WordWorth strip
  printing, preventing short portrait documents being rotated or
  expanded while allowing multi-page documents to print every page
- Added a printer-capability-backed DPI selector and renamed it DPI
- Replaced the debug JPEG option with a simple Debug on/off setting
- Added an early TCP-stack check so the driver fails safely when no
  bsdsocket.library-compatible stack is available

## 1.0.3

WordWorth support and more reliable printer connections.

- Fixed strip-printed pages (WordWorth, and likely explains
  ArtEffect's long-standing "many near-blank pages" behaviour
  too) submitting as several separate, massively oversized
  pages instead of one correct one - the driver now honours
  SPECIAL_NOFORMFEED and assembles the bands into a single
  document before sending it
- Fixed MintPrint Settings' status box going blank and staying
  that way after certain other applications have run
- Fixed Query being able to freeze the whole program with no
  way out but a reboot, if the printer didn't respond -
  connection attempts are now properly bounded and time out
  cleanly instead
- Fixed a bug where MintPrint Settings' own Get-Printer-
  Attributes request could be rejected by some printers
  (reported against a Canon TS8300), leaving Query showing no
  capabilities at all even though the printer itself was fine
- Query now prefers the standard IPP port (631) over port 80
  when discovering or querying a printer
- Query no longer accepts an all-empty capability response as
  a successful result

## 1.0.2

Bug fixes from real-hardware testing reports.

- Fixed an IPP response parsing bug (a printer's interim "100
  Continue" response was mistaken for the final one) that could
  report a print job as failed even though it printed, and could
  leave MintPrint Settings' Query showing Media/Scaling/Quality/
  Print Mode as undetected on printers that do support them
- MintPrint Settings' Query no longer accepts an incomplete
  response from the printer as a successful result
- Scaling now defaults to "auto" after a Query when the printer
  offers it, for better cross-printer compatibility
- Added a safety guard against a rare runaway "many near-blank
  pages" print storm triggered by some third-party applications
- Driver auto-update detection now also offers to update a
  driver installed before version tracking was added
- About box shows the installed and bundled driver revision
- Various MintPrint Settings layout and text-truncation fixes

## 1.0.0

Initial Aminet release.

- DEVS:Printers/MintPRINT driver
- Separate AmigaOS 3.1 classic driver build
- JPEG, PostScript, PWG Raster and PDF document backends
- MintPrint Settings GUI
- LAN printer discovery
- IPP capability query
- Up to eight saved printer profiles
- Built-in printer.device test page
- Automatic driver install/update
