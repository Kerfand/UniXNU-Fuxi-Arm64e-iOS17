#!/usr/bin/env bash
# Read-only listing of the 21H16 OS volume's /private/var template.
cd /mnt/f/UniXNU-FUXI-work/ios17-cache/ipsw-21H16
7z l -slt 044-18520-018.dmg 'private/var/*' -r > /home/xiaomi/ios17-bins-21H16/varls.txt 2>&1
echo "rc=$?"
python3 - <<'EOF'
import re
t = open('/home/xiaomi/ios17-bins-21H16/varls.txt', errors='replace').read()
items = []
for block in t.split('\n\n'):
    m = re.search(r'^Path = (.*)$', block, re.M)
    if not m or not m.group(1).startswith('private/var'): continue
    a = re.search(r'^Attributes = (.*)$', block, re.M)
    u = re.search(r'^User = (.*)$', block, re.M); g = re.search(r'^Group = (.*)$', block, re.M)
    items.append((m.group(1), a.group(1) if a else '', u.group(1) if u else '', g.group(1) if g else ''))
print('entries', len(items))
for p, a, u, g in items:
    if p.count('/') <= 4:
        print(f'{a:28} {u:12} {g:8} {p}')
EOF
