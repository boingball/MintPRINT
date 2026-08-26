from pathlib import Path

p = Path('.github/pr63_icon_cache_patch.py')
s = p.read_text(encoding='utf-8')
a = s.index('# 6) Activating another Unit copies its artwork cache too, just like capabilities.')
b = s.index('# 7) Final tiny layout alignment requested from the latest screenshot.')
p.write_text(s[:a] + '# 6) activation artwork copy is applied by pr63_activation_icon_copy.py\n\n' + s[b:], encoding='utf-8')
print('Prepared main icon-cache patch script')
