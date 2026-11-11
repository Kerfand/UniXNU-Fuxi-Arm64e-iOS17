"""hitscan.py <log>: for every 'fileset: unmapped call #n from SITE' line, decode the BL/B right before SITE (or the
GOT-loaded BLRAA) into its target, group hits by target and print the owning kext, first-argument strings and the
current stub state from kernel.json."""
import json, re, struct, sys
from collections import OrderedDict
K = '/mnt/f/UniXNU-FUXI-work/ios17-cache/ipsw-21H16/kernelcache.decompressed'
d = open(K, 'rb').read()


def lcs(base):
    n = struct.unpack_from('<I', d, base + 16)[0]; off = base + 32
    for _ in range(n):
        cmd, size = struct.unpack_from('<II', d, off); yield cmd, off; off += size


segs = [struct.unpack_from('<QQQQ', d, o + 24) for c, o in lcs(0) if c == 0x19]
entries = []
for c, o in lcs(0):
    if c == 0x80000035:
        vm, fo, sid = struct.unpack_from('<QQI', d, o + 8)
        entries.append((fo, d[o + sid:d.index(b'\0', o + sid)].decode()))
kext_ranges = []
for fo, name in entries:
    for c, o in lcs(fo):
        if c == 0x19:
            svm, ssz = struct.unpack_from('<QQ', d, o + 24)
            kext_ranges.append((svm, svm + ssz, name))


def owner(a):
    for lo, hi, name in kext_ranges:
        if lo <= a < hi:
            return name
    return '?'


def va2off(a):
    for svm, ssz, sfo, sfs in segs:
        if svm <= a < svm + sfs:
            return sfo + a - svm


def cstr(a):
    o = va2off(a)
    if o is None:
        return None
    end = d.find(b'\0', o, o + 80)
    s = d[o:end]
    return s.decode('ascii', 'replace') if end > o and all(32 <= ch < 127 for ch in s) else None


def target(site):
    w = struct.unpack_from('<I', d, va2off(site - 4))[0]
    if (w & 0xfc000000) in (0x94000000, 0x14000000):  # bl / b
        imm = w & 0x3ffffff
        if imm & (1 << 25): imm -= 1 << 26
        return site - 4 + imm * 4
    return None


stubbed = {}
r = json.load(open('/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/out-ios17/fuxi/kernel/kernel.json'))
for comp, targets in r['stubbed'].items():
    for t in targets:
        stubbed.setdefault(t, []).append(comp.split('.')[-1])
groups = OrderedDict()
pat = re.compile(r'unmapped call #(\d+) from (0x[0-9a-f]+) \(x0 (0x[0-9a-f]+) x1 (0x[0-9a-f]+) x2 (0x[0-9a-f]+) x3 (0x[0-9a-f]+)')
for line in open(sys.argv[1], errors='replace'):
    m = pat.search(line)
    if not m:
        continue
    n, site, x0, x1, x2, x3 = m.groups()
    site = int(site, 16); t = target(site)
    groups.setdefault(t, []).append((int(n), site, int(x0, 16), int(x1, 16), int(x2, 16), int(x3, 16)))
for t, hits in groups.items():
    tag = hex(t) if t else 'indirect'
    print(f'== target {tag} stubbed-in {stubbed.get(hex(t) if t else "", [])} hits {len(hits)}')
    for n, site, x0, x1, x2, x3 in hits[:4]:
        print(f'   #{n} site {site:#x} [{owner(site)}] x0 {x0:#x} {cstr(x0)!r} x1 {x1:#x} x2 {x2:#x} x3 {x3:#x}')
