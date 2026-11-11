#!/usr/bin/env bash
# Compare the scratch Data seed with the 21H16 OS volume's /private/var template (both read-only; the seed is expanded to /tmp).
set -e
R=/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU
python3 - <<'EOF'
import sys
sys.path.insert(0, '/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/drivers/DarwinBoot/Test')
import packed_ramdisk
blob = open('/mnt/f/UniXNU-FUXI-work/ios17-cache/scratch-data-21H16.img', 'rb').read()
logical, extents = packed_ramdisk.regions(blob)
with open('/home/xiaomi/ios17-bins-21H16/seed-21h16.raw', 'wb') as f:
    f.truncate(logical)
    for off, data in extents:
        f.seek(off); f.write(data)
print('raw', logical)
EOF
7z l -slt /home/xiaomi/ios17-bins-21H16/seed-21h16.raw > /home/xiaomi/ios17-bins-21H16/seedls.txt 2>&1 || true
python3 - <<'EOF'
import re
def parse(path, prefix):
    t = open(path, errors='replace').read(); out = {}
    for block in t.split('\n\n'):
        m = re.search(r'^Path = (.*)$', block, re.M)
        if not m: continue
        p = m.group(1)
        if prefix:
            if not p.startswith(prefix): continue
            p = p[len(prefix):]
        f = re.search(r'^Folder = (.*)$', block, re.M)
        out[p] = (f.group(1) if f else '?')
    return out
seed = parse('/home/xiaomi/ios17-bins-21H16/seedls.txt', '')
# the seed lists paths relative to the volume root, possibly under a volume folder name
roots = {p.split('/')[0] for p in seed}
print('seed top', sorted(roots)[:20], len(seed))
tmpl = parse('/home/xiaomi/ios17-bins-21H16/varls.txt', 'private/var/')
strip = None
for r in roots:
    if r + '/mobile' in seed: strip = r + '/'
if strip: seed = {p[len(strip):]: v for p, v in seed.items() if p.startswith(strip)}
tdirs = {p for p, v in tmpl.items() if v == '+' and not p.startswith('staged_system_apps')}
sdirs = {p for p, v in seed.items() if v == '+'}
missing = sorted(tdirs - sdirs)
print('template dirs', len(tdirs), 'seed dirs', len(sdirs), 'missing', len(missing))
open('/home/xiaomi/ios17-bins-21H16/seed-missing.txt', 'w').write('\n'.join(missing) + '\n')
for p in missing:
    if p.count('/') <= 3: print('  ', p)
EOF
