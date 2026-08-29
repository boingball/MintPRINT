# MintPRINT runtime configuration

MintPRINT reads its printer endpoint at the start of every graphics print job.
The live profile is:

    ENV:MintPRINT/Unit0

If that file is absent, the driver falls back to:

    ENVARC:MintPRINT/Unit0

If neither exists, the driver defaults to an empty host and simply does
nothing (query or print) rather than falling back to any hardcoded
address - an early build defaulted to a specific test printer's address,
which meant an unconfigured Unit0 could silently send traffic to
whatever device happened to be at that address on someone else's
network.

The driver itself has no concept of multiple units - it only ever reads
`Unit0`. MintPrint Settings' Unit dropdown (see `docs/MINTPRINT_PREFS.md`)
manages `Unit1`, `Unit2`, ... as switchable saved profiles for people with
more than one printer, and its **Activate** button is how one of those
becomes the live `Unit0` this driver reads.

## Unit0 format

The file is plain text (`HOST=` below is a placeholder - MintPrint
Settings always fills in a real printer address before saving):

    HOST=192.168.1.100
    PORT=80
    PATH=/ipp/print
    DEBUG=0
    SIDES=
    SPOOL=RAM
    PWG_SHEET_BACK=normal

`DEBUG=0` is the default: no `T:MintPRINT-gui.log` or
`T:MintPRINT-driver.log` is written, and the temporary rendered job is removed
after submission (including a failed submission). `DEBUG=1` enables both logs
and keeps `T:MintPRINT-job.jpg`, `.pwg`, or `.pdf` for diagnosis.

For compatibility, existing `KEEPJOB=0`/`KEEPJOB=1` profiles are still read as
Debug Off/On. MintPrint Settings writes `DEBUG=` when the profile is next saved.

`SIDES=` accepts `one-sided`, `two-sided-long-edge`, or
`two-sided-short-edge`. MintPrint Settings displays One-sided by default and
only offers duplex values confirmed by Query Printer. For a printer that does
not advertise duplex, Settings saves an empty `SIDES=` value so the driver
omits the optional IPP attribute; absence still means one-sided. Old profiles
without a `SIDES=` line preserve the historical request shape. See
`docs/DUPLEX_PRINTING.md`.

`SPOOL=` selects where job files (the rendered JPEG/PWG Raster/PDF/
PostScript/URF document, plus its captured-text-mode equivalent) are
written before submission. `RAM` (the default) is MintPRINT's original,
unconfigurable behaviour: files go under `T:`, which is normally assigned
to `RAM:` on a stock system. Any other value is used as a literal device
prefix (e.g. `DH0:`), so job files spool to a real hard drive instead -
useful on memory-tight systems (see `docs/OS31_SUPPORT.md`'s memory
preflight check) where even `T:`'s usual RAM: backing is scarce. MintPrint
Settings' Spooler gadget lists `RAM` plus one entry per `DHn:`-named
device it finds mounted, and writes this value when saved.

`PWG_SHEET_BACK=` records the printer's
`pwg-raster-document-sheet-back` capability (`normal`, `rotated`, `flipped`,
or `manual-tumble`). Query Printer writes it automatically. It controls the
PWG reverse-side coordinate transform only; old profiles default to `normal`.

Settings are reloaded for each new graphics print, so changing Unit0 does not
require unloading the driver. Replacing `DEVS:Printers/MintPRINT` itself still
requires a reboot before testing a new driver binary.

`config-Unit0.example` shows the same format for manual/scripted setup
without running MintPrint Settings first - replace its placeholder `HOST=`
with your printer's real address before copying it to the ENV:/ENVARC:
locations; copied verbatim, it configures nothing your printer actually
uses.
