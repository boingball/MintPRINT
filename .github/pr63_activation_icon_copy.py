from pathlib import Path

p = Path('src/MintPrintSettings.c')
s = p.read_text(encoding='utf-8')
anchor = '                                    mp_copy_file((CONST_STRPTR)src_cache_envarc, (CONST_STRPTR)dst_cache_envarc);\n'
if s.count(anchor) != 1:
    raise SystemExit(f'activation artwork anchor count={s.count(anchor)}')
lines = [
    '',
    '                                    /* Carry the processed printer artwork too. */',
    '                                    {',
    '                                        char src_icon_env[96], src_icon_envarc[96];',
    '                                        char dst_icon_env[96], dst_icon_envarc[96];',
    '                                        ensure_config_dir((CONST_STRPTR)"ENV:MintPRINT/Art");',
    '                                        ensure_config_dir((CONST_STRPTR)"ENVARC:MintPRINT/Art");',
    '                                        unit_icon_cache_path(current_unit_index, FALSE, src_icon_env, sizeof(src_icon_env));',
    '                                        unit_icon_cache_path(current_unit_index, TRUE, src_icon_envarc, sizeof(src_icon_envarc));',
    '                                        unit_icon_cache_path(0, FALSE, dst_icon_env, sizeof(dst_icon_env));',
    '                                        unit_icon_cache_path(0, TRUE, dst_icon_envarc, sizeof(dst_icon_envarc));',
    '                                        mp_copy_file((CONST_STRPTR)src_icon_env, (CONST_STRPTR)dst_icon_env);',
    '                                        mp_copy_file((CONST_STRPTR)src_icon_envarc, (CONST_STRPTR)dst_icon_envarc);',
    '                                    }',
]
insertion = anchor + '\n'.join(lines) + '\n'
p.write_text(s.replace(anchor, insertion, 1), encoding='utf-8')
print('Added Unit activation artwork cache copy')
