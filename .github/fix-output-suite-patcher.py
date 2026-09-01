from pathlib import Path

p = Path('.github/apply-output-suite.py')
s = p.read_text()
old = '''    if count != 1:
        raise SystemExit(f"{label}: expected 1 anchor, found {count}")
    p.write_text(s.replace(old, new, 1))'''
new = '''    if count != 1:
        if label == "config capture source guard" and count == 0:
            alt_old = "    Close(fh);\\n\\n    /* Config loading already"
            guard = (
                "    Close(fh);\\n\\n"
                "    /* CAPTURE_* is never a persisted Unit0 feature. Even a hand-edited\\n"
                "     * ENV:/ENVARC: file containing those keys cannot disable printing.\\n"
                "     * Only the dedicated T: test-suite override may enable it. */\\n"
                "    if (source != MP_CONFIG_SOURCE_TEST) {\\n"
                "        cfg->capture_only = FALSE;\\n"
                "        cfg->capture_path[0] = 0;\\n"
                "    } else if (cfg->capture_only) {\\n"
                "        cfg->debug = TRUE;\\n"
                "        mp_cfg_copy(cfg->spool, sizeof(cfg->spool), \\\"RAM\\\");\\n"
                "        cfg->spool_keep = FALSE;\\n"
                "    }\\n\\n"
                "    /* Config loading already"
            )
            if s.count(alt_old) != 1:
                raise SystemExit("config capture alternate tail anchor failed")
            s = s.replace(alt_old, guard, 1)
            ps_old = "    if (mp_cfg_starts(cfg->engine, \\\"postscript\\\") &&\\n"
            ps_new = "    if (!cfg->capture_only &&\\n        mp_cfg_starts(cfg->engine, \\\"postscript\\\") &&\\n"
            if s.count(ps_old) != 1:
                raise SystemExit("capture PostScript query guard anchor failed")
            s = s.replace(ps_old, ps_new, 1)
            p.write_text(s)
            return
        raise SystemExit(f"{label}: expected 1 anchor, found {count}")
    p.write_text(s.replace(old, new, 1))'''
if s.count(old) != 1:
    raise SystemExit('replace_once helper anchor failed')
p.write_text(s.replace(old, new, 1))
print('patcher adjusted for current config.c tail')
