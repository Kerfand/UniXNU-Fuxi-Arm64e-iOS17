"""binslide.py BIN addr...: find the PIE slide for which every return address follows a BL/BLR*, then list the
containing function (LC_FUNCTION_STARTS) and the nearest symbol / referenced selector strings."""
import struct, sys, bisect, subprocess
path = sys.argv[1]; addrs = [int(a, 16) for a in sys.argv[2:]]
d = open(path, 'rb').read()
ncmds = struct.unpack_from('<I', d, 16)[0]; off = 32
segs = []; fstarts = None; text_vm = None
for _ in range(ncmds):
    cmd, sz = struct.unpack_from('<II', d, off)
    if cmd == 0x19:
        name = d[off + 8:off + 24].rstrip(b'\0').decode()
        vm, vs, fo, fs = struct.unpack_from('<QQQQ', d, off + 24)
        segs.append((name, vm, vs, fo, fs))
        if name == '__TEXT': text_vm = vm
    if cmd == 0x26:
        doff, dsz = struct.unpack_from('<II', d, off + 8); fstarts = (doff, dsz)
    off += sz


def rd(vm):
    for n, v, s, fo, fs in segs:
        if v <= vm < v + fs:
            return struct.unpack_from('<I', d, fo + vm - v)[0]
    return None


def is_call(i):
    return i is not None and ((i & 0xfc000000) == 0x94000000 or (i & 0xfefff000) == 0xd63f0000)
best = []
for slide in range(0, 0x1000000, 0x4000):
    ok = sum(1 for a in addrs if is_call(rd(a - slide - 4)))
    best.append((ok, slide))
best.sort(reverse=True)
print('best slides', [(o, hex(s)) for o, s in best[:4]], 'of', len(addrs))
slide = best[0][1]
# function starts
starts = []
if fstarts:
    p, end = fstarts[0], fstarts[0] + fstarts[1]; cur = text_vm
    while p < end:
        v = 0; sh = 0
        while True:
            b = d[p]; p += 1; v |= (b & 0x7f) << sh; sh += 7
            if not b & 0x80: break
        if v == 0: break
        cur += v; starts.append(cur)
syms = {}
try:
    out = subprocess.run(['/home/xiaomi/unixnu-linux/llvm/bin/llvm-nm', '-n', '--defined-only', path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3: syms[int(parts[0], 16)] = parts[2]
except Exception as e:
    print('nm', e)
symaddrs = sorted(syms)
for a in addrs:
    vm = a - slide
    i = bisect.bisect_right(starts, vm) - 1
    fs = starts[i] if i >= 0 else 0
    j = bisect.bisect_right(symaddrs, vm) - 1
    sym = syms[symaddrs[j]] + '+%#x' % (vm - symaddrs[j]) if j >= 0 else '?'
    print(f'{a:#x} -> {vm:#x} func {fs:#x}+{vm - fs:#x} sym {sym}')
