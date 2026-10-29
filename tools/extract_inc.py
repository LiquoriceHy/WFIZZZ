import re, io

s = io.open('.cproject', encoding='utf-8', errors='replace').read()
items = re.findall(r'value="&quot;\$\{workspace_loc://\$\{ProjName\}//([^&]*)\}&quot;"', s)

seen = []
for it in items:
    p = it.replace(chr(92) + chr(92), '/').replace(chr(92), '/')
    if p not in seen:
        seen.append(p)

print("include entries =", len(seen))
import os
out = os.path.join(os.environ.get('TEMP', '.'), 'inc_list.txt')
with io.open(out, 'w', encoding='utf-8') as f:
    f.write('\n'.join(seen))
print("written to", out)
for x in seen[:15]:
    print("  ", x)
