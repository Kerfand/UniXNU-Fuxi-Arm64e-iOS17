"""secfuncs.py: name stripped securityd functions via ObjC method lists (relative lists) and list C strings each
function references (ADRP+ADD/LDR into __cstring/__objc_methname/__cfstring)."""
import struct, bisect, sys
p = '/home/xiaomi/ios17-bins-21H16/securityd'
d = open(p, 'rb').read()
n = struct.unpack_from('<I', d, 16)[0]; o = 32; segs = []; sects = {}; fst = None
for _ in range(n):
    c, s = struct.unpack_from('<II', d, o)
    if c == 0x19:
        segname = d[o + 8:o + 24].rstrip(b'\0').decode()
        vm, vs, fo, fs, mp, ip, ns, fl = struct.unpack_from('<QQQQIIII', d, o + 24)
        segs.append((vm, fo, fs, vs))
        so = o + 72
        for _ in range(ns):
            sn = d[so:so + 16].rstrip(b'\0').decode(); sa, sz, sfo = struct.unpack_from('<QQI', d, so + 32)
            sects[(segname, sn)] = (sa, sz, sfo); so += 80
    if c == 0x26:
        fst = struct.unpack_from('<II', d, o + 8)
    o += s


def off(vm):
    for v, fo, fs, vs in segs:
        if v <= vm < v + fs: return fo + vm - v


def u32(vm): x = off(vm); return struct.unpack_from('<I', d, x)[0] if x is not None else None
def u64(vm): x = off(vm); return struct.unpack_from('<Q', d, x)[0] if x is not None else None


def cstr(vm):
    x = off(vm)
    if x is None: return None
    e = d.index(b'\0', x); return d[x:e].decode('latin1')


def ptr(v):
    if v is None: return None
    if v >> 63: return None  # bind
    t = v & 0xfffffffff  # 36-bit target
    return t if t >= 0x100000000 else t + 0x100000000
starts = []; q, end = fst[0], fst[0] + fst[1]; cur = 0x100000000
while q < end:
    v = 0; sh = 0
    while True:
        b = d[q]; q += 1; v |= (b & 0x7f) << sh; sh += 7
        if not b & 0x80: break
    if v == 0: break
    cur += v; starts.append(cur)
names = {}
if ('__TEXT', '__objc_methlist') in sects:
    sa, sz, sfo = sects[('__TEXT', '__objc_methlist')]
    a = sa
    while a < sa + sz:
        fl, cnt = u32(a), u32(a + 4)
        es = fl & 0xffff
        if not (fl & 0x80000000) or es != 12 or cnt > 5000:
            a += 4; continue
        for i in range(cnt):
            e = a + 8 + i * 12
            no, to, io = struct.unpack_from('<iii', d, off(e))
            selref = e + no
            sel = ptr(u64(selref))
            nm = cstr(sel) if sel else None
            imp = e + 8 + io
            if nm: names.setdefault(imp, nm)
        a += 8 + cnt * 12
print('objc methods named', len(names))
strsects = [sects[k] for k in sects if k[1] in ('__cstring', '__objc_methname', '__oslogstring', '__cfstring')]


def strings_in(fs, fe):
    out = []
    a = fs
    while a < fe:
        i = u32(a)
        if i is not None and (i & 0x9f000000) == 0x90000000:  # adrp
            rd = i & 31
            immlo = (i >> 29) & 3; immhi = (i >> 5) & 0x7ffff
            imm = ((immhi << 2) | immlo) << 12
            if imm & (1 << 32): imm -= 1 << 33
            page = (a & ~0xfff) + imm
            j = u32(a + 4)
            if j is not None and (j & 0xffc00000) == 0x91000000 and ((j >> 5) & 31) == rd:  # add
                tgt = page + ((j >> 10) & 0xfff)
                for sa, sz, sfo in strsects:
                    if sa <= tgt < sa + sz:
                        s = cstr(tgt)
                        if s and len(s) > 2: out.append(s[:90])
                        break
                # __cfstring entries: {isa, flags, ptr, len}
                cf = sects.get(('__DATA_CONST', '__cfstring')) or sects.get(('__DATA', '__cfstring'))
                if cf and cf[0] <= tgt < cf[0] + cf[1]:
                    s = cstr(ptr(u64(tgt + 16)) or 0)
                    if s: out.append('@"' + s[:90] + '"')
        a += 4
    return out
for f in [int(x, 16) for x in sys.argv[1:]]:
    i = bisect.bisect_right(starts, f) - 1
    fs = starts[i]; fe = starts[i + 1] if i + 1 < len(starts) else fs + 0x400
    print(f'== {f:#x} in {fs:#x} (+{f - fs:#x}, size {fe - fs:#x}) objc: {names.get(fs)}')
    seen = []
    for s in strings_in(fs, fe):
        if s not in seen: seen.append(s)
    for s in seen[:14]: print('   ', s)
