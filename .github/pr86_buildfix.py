from pathlib import Path

p = Path('src/MintPrintSettings.c')
s = p.read_text()
old = '''    if (!c) return -1;\n    mp_config_defaults(&cfg);\n    mp_test_suite_copy_string(cfg.host, sizeof(cfg.host), "127.0.0.1");'''
new = '''    if (!c) return -1;\n    /* This helper is linked into MintPrintSettings, not the printer driver.\n     * Do not depend on driver/config.c merely for mp_config_defaults(): that\n     * module also owns driver-only Render compatibility state and is not part\n     * of the GUI link. The serializer below only consumes the endpoint and\n     * job-template fields populated here, so a zeroed local config is the\n     * correct minimal starting point. */\n    memset(&cfg, 0, sizeof(cfg));\n    mp_test_suite_copy_string(cfg.host, sizeof(cfg.host), "127.0.0.1");'''
if s.count(old) != 1:
    raise SystemExit(f'expected one anchor, found {s.count(old)}')
p.write_text(s.replace(old, new, 1))
print('patched MintPrintSettings config initialization')
