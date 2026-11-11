"""dscsym.py <slide> <va>...: nearest symbol for runtime addresses in the 21H16 dyld shared cache, using the local symbols file
(dyld_shared_cache_arm64e.symbols: dyld_cache_local_symbols_info, 64-bit entries) plus each image's own symbol table in the shared
linkedit, and the owning image from the main cache's image list. Results are cached in /tmp/dscsym.pkl."""
import bisect, os, pickle, struct, sys
D = '/mnt/f/UniXNU-FUXI-work/ios17-cache/dyld-21H16'
# Decoded copies of the symbols file and the linkedit subcaches (the IPSW's own copies are empty or encoded) take precedence.
DEC = '/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU/out-ios17/dsc-21H16-decoded'
CACHE = '/home/xiaomi/ios17-bins-21H16/dscsym.pkl'
_open = open


def open(path, mode='r', *a, **k):
    name = os.path.basename(path)
    alt = os.path.join(DEC, name)
    if path.startswith(D) and os.path.exists(alt) and os.path.getsize(alt):
        path = alt
    return _open(path, mode, *a, **k)


def load():
    if os.path.exists(CACHE):
        return pickle.load(open(CACHE, 'rb'))
    syms = []
    # Local symbols.
    sf = open(os.path.join(D, 'dyld_shared_cache_arm64e.symbols'), 'rb').read()
    lso, lss = struct.unpack_from('<QQ', sf, 0x48)
    info = lso
    nlo, nlc, so, ss, eo, ec = struct.unpack_from('<IIIIII', sf, info)
    strs = info + so
    for i in range(nlc):
        o = info + nlo + 16 * i
        strx, ntype, nsect, ndesc, nval = struct.unpack_from('<IBBHQ', sf, o)
        if (ntype & 0x0e) == 0x0e and nval:   # N_SECT
            e = sf.index(b'\0', strs + strx)
            syms.append((nval, sf[strs + strx:e].decode('utf-8', 'replace')))
    # Global symbols: every image's LC_SYMTAB (offsets into the subcache that holds __LINKEDIT).
    main = open(os.path.join(D, 'dyld_shared_cache_arm64e'), 'rb').read()
    maps = {}
    for name in sorted(os.listdir(D)):
        if not name.startswith('dyld_shared_cache_arm64e') or name.endswith(('.symbols', '.map', '.atlas')):
            continue
        b = open(os.path.join(D, name), 'rb').read() if name != 'dyld_shared_cache_arm64e' else main
        if len(b) < 0x200 or not b.startswith(b'dyld_v1'):
            continue
        moff, mcount = struct.unpack_from('<II', b, 0x10)
        for i in range(mcount):
            addr, size, fo, maxp, initp = struct.unpack_from('<QQQII', b, moff + 32 * i)
            maps.setdefault(name, []).append((addr, size, fo))
    files = {}

    def read_va(a, n):
        for name, ms in maps.items():
            for addr, size, fo in ms:
                if addr <= a < addr + size:
                    if name not in files:
                        files[name] = open(os.path.join(D, name), 'rb').read() if name != 'dyld_shared_cache_arm64e' else main
                    return files[name], fo + (a - addr), name, (addr, fo)
        return None, None, None, None
    ioff, icount = struct.unpack_from('<II', main, 0x1c0)
    images = []
    for i in range(icount):
        addr, mod, inode, pathoff, pad = struct.unpack_from('<QQQII', main, ioff + 32 * i)
        path = main[pathoff:main.index(b'\0', pathoff)].decode()
        images.append((addr, path))
        b, o, name, _ = read_va(addr, 32)
        if b is None:
            continue
        ncmds, sizeofcmds = struct.unpack_from('<II', b, o + 16)
        at = o + 32
        linkedit = None; symtab = None
        for _ in range(ncmds):
            cmd, sz = struct.unpack_from('<II', b, at)
            if cmd == 0x19 and b[at + 8:at + 24].rstrip(b'\0') == b'__LINKEDIT':
                lvm, lvs, lfo, lfs = struct.unpack_from('<QQQQ', b, at + 24)
                linkedit = (lvm, lfo)
            elif cmd == 2:
                symtab = struct.unpack_from('<IIII', b, at + 8)
            at += sz
        if not linkedit or not symtab:
            continue
        symoff, nsyms, stroff, strsize = symtab
        lvm, lfo = linkedit
        lb, lo, _, _ = read_va(lvm, 1)
        if lb is None:
            continue
        base = lo - lfo   # file offsets in LC_SYMTAB are relative to the linkedit subcache's own file
        for k in range(min(nsyms, 2000000)):
            p = base + symoff + 16 * k
            if p + 16 > len(lb):
                break
            strx, ntype, nsect, ndesc, nval = struct.unpack_from('<IBBHQ', lb, p)
            if (ntype & 0x0e) == 0x0e and nval and strx:
                s = base + stroff + strx
                if s < len(lb):
                    e = lb.find(b'\0', s, s + 512)
                    if e > s:
                        syms.append((nval, lb[s:e].decode('utf-8', 'replace')))
    syms.sort()
    images.sort()
    data = ([a for a, _ in syms], [n for _, n in syms], images)
    pickle.dump(data, open(CACHE, 'wb'))
    return data


addrs, names, images = load()
slide = int(sys.argv[1], 16)
for v in sys.argv[2:]:
    va = int(v, 16)
    a = va - slide
    i = bisect.bisect_right(addrs, a) - 1
    j = bisect.bisect_right([x for x, _ in images], a) - 1
    img = images[j][1].rsplit('/', 1)[-1] if j >= 0 else '?'
    print(f'{va:#x} {img:28s} {names[i] if i >= 0 else "?"}+{a - addrs[i]:#x}' if i >= 0 else f'{va:#x} {img} ?')
