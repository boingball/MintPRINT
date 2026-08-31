#!/usr/bin/env python3
"""Host-test real FF/reset and strip-wrapper callbacks without an Amiga SDK.

Extract complete top-level functions, not rewritten copies of their logic.
The C harness mocks logging, text allocation, Render and page finalisation;
the existing writer/geometry tests cover those separate layers. This is not
an emulator or proof of printer.device callback ordering on real hardware.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    # These C sources use unindented function headers/closing braces and
    # indented nested blocks. Fail closed if a refactor changes that shape.
    pattern = (
        r"^(?:static )?(?:LONG PRT_STDARGS|VOID|void|BOOL) "
        + re.escape(name)
        + r"\([^;]*?\)\n\{\n.*?^\}"
    )
    matches = re.findall(pattern, source, flags=re.M | re.S)
    if len(matches) != 1:
        raise RuntimeError(f"Expected exactly one complete function: {name}")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()

    config = (ROOT / "driver/config.c").read_text()
    core = (ROOT / "driver/driver_core.c").read_text()
    text = (ROOT / "driver/command_table.c").read_text()
    start = config.index("static ULONG g_render_compat_special")
    end = config.index("static ULONG mp_cfg_len", start)
    declarations = config[start:end]
    functions = []
    for source, names in (
        (config, ("mp_render_compat_finish_page", "mp_render_compat_reset",
                  "MintPRINTCompatEndPage", "mp_render_compat_add_rows",
                  "MintPRINTCompatRender")),
        (core, ("MintPRINTGraphicsFormFeed", "MintPRINTNoteVerticalAdvance",
                "MintPRINTResetVerticalAdvances", "DoSpecial")),
        (text, ("ConvFunc", "TextDoSpecial")),
    ):
        functions.extend(function(source, name) for name in names)

    # Verify the failure latch's lifecycle in the real entry points too.
    opened = core.split(
        "int PRT_STDARGS DriverOpen(", 1)[1].split("VOID PRT_STDARGS DriverClose", 1)[0]
    assert "g_graphics_boundary_failed = FALSE;" in opened
    render = function(core, "Render")
    assert re.search(r"if \(g_graphics_boundary_failed\) \{[^}]*return PDERR_CANCEL;",
                     render)

    harness = (ROOT / "tests/graphics_boundary_harness.c").read_text()
    harness = harness.replace("/* CONFIG_STATE */", declarations)
    harness = harness.replace("/* REAL_CALLBACKS */", "\n\n".join(functions))
    flags = ["-std=c89", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="mintprint-boundary-") as directory:
        source = Path(directory) / "boundary.c"
        binary = Path(directory) / "boundary-test"
        source.write_text(harness)
        subprocess.run(shlex.split(args.cc) + flags + ["-I", str(ROOT / "driver"),
                       str(source), str(ROOT / "driver/media_size.c"),
                       "-o", str(binary)], check=True)
        env = os.environ.copy()
        if args.sanitize:
            env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        subprocess.run([str(binary)], check=True, env=env)


if __name__ == "__main__":
    main()
