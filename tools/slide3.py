import struct, bisect
p = '/home/xiaomi/ios17-bins-21H16/securityd'
d = open(p, 'rb').read()
n = struct.unpack_from('<I', d, 16)[0]; o = 32; segs = []; fst = None
for _ in range(n):
    c, s = struct.unpack_from('<II', d, o)
    if c == 0x19:
        vm, vs, fo, fs = struct.unpack_from('<QQQQ', d, o + 24); segs.append((vm, fo, fs))
    if c == 0x26:
        fst = struct.unpack_from('<II', d, o + 8)
    o += s
starts = []; q, end = fst[0], fst[0] + fst[1]; cur = 0x100000000
while q < end:
    v = 0; sh = 0
    while True:
        b = d[q]; q += 1; v |= (b & 0x7f) << sh; sh += 7
        if not b & 0x80: break
    if v == 0: break
    cur += v; starts.append(cur)


def rd(vm):
    for v, fo, fs in segs:
        if v <= vm < v + fs: return struct.unpack_from('<I', d, fo + vm - v)[0]


def call(x):
    return x is not None and ((x & 0xfc000000) == 0x94000000 or (x & 0xfefff000) == 0xd63f0000)
run2 = [0x10036b5f8, 0x10051070c, 0x10042d7b4, 0x10042d6b8, 0x10042663c, 0x10039c7e4, 0x10047c170]
run1 = [a - 0x270000 for a in run2] + [0x100121ac0, 0x1001bd93c]
res = []
for sl in range(0, 0x800000, 0x4000):
    ok = [call(rd(a - sl - 4)) for a in run1]
    res.append((sum(ok), sl, ok))
res.sort(key=lambda r: -r[0])
for r in res[:5]:
    print(r[0], hex(r[1]), r[2])
best = res[0][1]
for a in run1:
    vm = a - best
    i = bisect.bisect_right(starts, vm) - 1
    print(hex(a), '->', hex(vm), 'func', hex(starts[i]), '+%#x' % (vm - starts[i]), 'insn', hex(rd(vm - 4) or 0))
