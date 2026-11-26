#!/usr/bin/env python3
"""Sideloading step 1 for fuxi iOS 17 (21H16): IPAs preinstalled at pack time, delivered through the boot GPU addon.

  build-fuxi-sideload.py IPA_DIR ADDON_IN ADDON_OUT [PART]

PART (optional):
  auto      print the plan only (how many boots, bytes per part) and write nothing
  2         write part 2 of the plan (1-based); the plan is recomputed from the same inputs every run
  1/1       write everything into one addon (the old behaviour; over the 64 MiB addon cap for a real app)

Why parts: DarwinBoot reads the addon with SIZE_64MB (UniXNU/drivers/DarwinBoot, DarwinBoot.c) and refuses anything bigger, so
one boot carries at most ~64 MiB of addon. The kernel writes every entry into the Data volume (/private/var/ngpu, PERSIST=2)
and never deletes a file, so the parts accumulate: boot 1..N-1 put the app there, boot N closes it with apps/.fuxi-sideload and
the app is opened after that boot. Every part carries the same complete CDHash list in trust.json, so SIDELOAD=1 can add all of
them to the owner trust module on any of the boots.

Why chunks: an iOS application bundle can hold a single Mach-O larger than one boot's addon (bili-universal.app's main
executable is 230 MiB fat). A file whose compressed size does not fit one entry is split into pieces named
"<path>#<8 hex digits>", the offset of the piece inside the file; the kernel patch zzd-sideload-addon-chunks.patch writes such
a piece at that offset, so the parts may deliver one file across several boots, in any order, without duplication.

1. Every IPA_DIR/*.ipa is read with zipfile; its Payload/<Name>.app is taken as it is (symlinks are refused: the addon holds
   regular files only, and an app with Versions/ symlinks is a macOS layout anyway).
2. Each Mach-O file in the bundle (main executable, frameworks, dylibs, app extensions), deepest first, is thinned to arm64e
   (or arm64), then signed ad hoc with 4 KiB pages, its own identifier and the entitlements it was signed with (get-task-allow
   is kept, so the app can be debugged). Its CDHash goes to trust.json.
3. ADDON_OUT = the base addon's files (only with WITH_BASE=1) + this part's apps/<Name>.app entries + apps/.fuxi-sideload on the
   last part. The kernel (zz-sideload-apps-mount.patch) unions /private/var/ngpu/apps over /Applications.
The IPAs are the user's own files and are never committed. Names in the addon are at most 160 bytes; a chunked name spends 9 of
them on the offset, so such a path is at most 151 bytes.

Environment:
  PART_BUDGET_MB  addon bytes one part may hold (default 56, under the firmware's 64 MiB read cap)
  CHUNK_MB        raw bytes per chunk of an oversized file (default 12; lowered automatically until it compresses under
                  ENTRY_MAX_MB)
  ENTRY_MAX_MB    compressed bytes above which a file is chunked (default 20)
  WITH_BASE       . =1 also copy the base addon's files (plugin, Wi-Fi, audio, Bluetooth) into this part, for a boot on a Data
                  volume that never had them; off by default because they stay there from an earlier boot and each copy costs
                  ~32 MiB of the part budget
"""
import importlib.util, json, os, plistlib, re, shutil, stat, subprocess, sys, tempfile, zipfile, zlib
from pathlib import Path

BASE = Path('/home/xiaomi/sadnut-fuxi-local-20261103')
OUT = BASE / 'UniXNU/out-ios17/fuxi/tree/build/fuxi-sideload'
env = dict(os.environ)
env['PATH'] = ':'.join(['/home/xiaomi/.local/bin', str(BASE / 'bin'), '/home/xiaomi/unixnu-linux/CommandLineTools/usr/bin', env['PATH']])
MACHO = {b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}
MIB = 1024 * 1024
BUDGET = int(os.environ.get('PART_BUDGET_MB', '56')) * MIB
CHUNK_RAW = int(os.environ.get('CHUNK_MB', '12')) * MIB
ENTRY_MAX = int(os.environ.get('ENTRY_MAX_MB', '20')) * MIB
WITH_BASE = os.environ.get('WITH_BASE') == '1'
NAME_MAX = 160
MARKER = ('apps/.fuxi-sideload', 0o644, b'fuxi sideload layer\n', 22)


def run(*cmd, check=True):
    r = subprocess.run([str(c) for c in cmd], env=env, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f'{cmd[0]} failed ({r.returncode}):\n{r.stdout}\n{r.stderr}')
    return r


def entitlements_of(path):
    r = run('codesign', '-d', '--entitlements', ':-', path, check=False)
    text = r.stdout + r.stderr
    if '<?xml' not in text:
        return None
    return text[text.index('<?xml'):text.rindex('</plist>') + len('</plist>')] + '\n'


def thin(path):
    head = path.read_bytes()[:4]
    if head not in (b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
        return
    archs = run('lipo', '-archs', path).stdout.split()
    want = 'arm64e' if 'arm64e' in archs else 'arm64' if 'arm64' in archs else None
    if not want:
        sys.exit(f'{path}: no arm64/arm64e slice ({archs})')
    run('lipo', path, '-thin', want, '-output', str(path) + '.thin')
    os.replace(str(path) + '.thin', path)


def sign(path, identifier, ents, work):
    args = ['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', identifier]
    if ents:
        ef = work / 'ents.plist'
        ef.write_text(ents)
        args += ['--entitlements', ef]
    run(*args, path)
    detail = run('codesign', '--display', '--verbose=4', path).stdout + run('codesign', '--display', '--verbose=4', path).stderr
    assert re.search(r'^Page size=4096$', detail, re.M), f'{path}: not 4 KiB pages'
    return re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]


def bundle_id(app):
    info = app / 'Info.plist'
    return plistlib.loads(info.read_bytes()).get('CFBundleIdentifier') if info.is_file() else None


def build_app(ipa, work):
    root = work / ipa.stem
    with zipfile.ZipFile(ipa) as z:
        for member in z.infolist():
            name = member.filename
            if not name.startswith('Payload/') or '..' in name.split('/'):
                continue
            if stat.S_ISLNK(member.external_attr >> 16):
                sys.exit(f'{ipa.name}: {name} is a symlink, the addon holds regular files only')
            z.extract(member, root)
    apps = sorted((root / 'Payload').glob('*.app'))
    if len(apps) != 1:
        sys.exit(f'{ipa.name}: expected one Payload/*.app, found {len(apps)}')
    app = apps[0]
    for meta in ('_CodeSignature', 'embedded.mobileprovision', 'SC_Info'):
        for p in list(app.rglob(meta)):
            shutil.rmtree(p) if p.is_dir() else p.unlink()
    machos = [p for p in app.rglob('*') if p.is_file() and p.read_bytes()[:4] in MACHO]
    # deepest first: a bundle's seal covers its nested code
    machos.sort(key=lambda p: len(p.relative_to(app).parts), reverse=True)
    hashes = []
    for p in machos:
        ents = entitlements_of(p)
        thin(p)
        # a bundle's main executable is signed with the bundle identifier, every other Mach-O with its file name
        owner = p.parent
        info = owner / 'Info.plist'
        main_exe = plistlib.loads(info.read_bytes()).get('CFBundleExecutable') if info.is_file() else None
        ident = (bundle_id(owner) or p.name) if p.name == main_exe else p.name
        hashes.append(sign(p, ident, ents, work))
        p.chmod(0o755)
    files = []
    for p in sorted(app.rglob('*')):
        if p.is_file():
            name = f'apps/{app.name}/{p.relative_to(app).as_posix()}'
            if len(name.encode()) + 1 > NAME_MAX:
                sys.exit(f'{name}: longer than the addon allows ({NAME_MAX} bytes)')
            files.append((name, 0o755 if p.stat().st_mode & 0o111 else 0o644, p.read_bytes()))
    return app.name, bundle_id(app), hashes, files


def packed_size(data):
    return len(zlib.compress(data, 9))


def entries_for(name, mode, data):
    """(addon name, mode, data, packed bytes) entries for one bundle file: the file itself, or offset-named chunks.

    The compressed size is kept next to the data so the part planner does not compress the app again for every candidate.
    """
    packed = packed_size(data)
    if packed <= ENTRY_MAX:
        return [(name, mode, data, packed)]
    room = CHUNK_RAW
    while room >= MIB // 2:
        pieces = []
        for at in range(0, len(data), room):
            piece = data[at:at + room]
            pieces.append((name + '#%08x' % at, mode, piece, packed_size(piece)))
        if all(p[3] <= ENTRY_MAX for p in pieces):
            if len(pieces[0][0].encode()) + 1 > NAME_MAX:
                sys.exit(f'{name}: chunked name longer than the addon allows ({NAME_MAX} bytes)')
            return pieces
        room //= 2
    sys.exit(f'{name}: one chunk of {MIB // 2} bytes does not compress under {ENTRY_MAX} bytes')


def part_bytes(entries):
    return 24 + sum(24 + (len(n.encode()) + 1 + 7) // 8 * 8 + (packed + 7) // 8 * 8 for n, _, _, packed in entries)


def plan_parts(entries):
    parts, current = [], []
    for entry in entries:
        if current and part_bytes(current + [entry]) > BUDGET:
            parts.append(current)
            current = []
        if part_bytes([entry]) > BUDGET:
            sys.exit(f'{entry[0]}: one entry is {part_bytes([entry])} bytes, over the {BUDGET}-byte part budget')
        current.append(entry)
    if current:
        parts.append(current)
    return parts


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    ipa_dir, addon_in, addon_out = map(Path, sys.argv[1:4])
    want = sys.argv[4] if len(sys.argv) == 5 else '1/1'
    ipas = sorted(ipa_dir.glob('*.ipa'))
    if not ipas:
        sys.exit(f'no .ipa in {ipa_dir}')
    spec = importlib.util.spec_from_file_location('addfx', BASE / 'device-ios17/scripts/add-fuxi-wallpaper-to-addon.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    OUT.mkdir(parents=True, exist_ok=True)
    apps, report, hashes = [], [], []
    with tempfile.TemporaryDirectory(dir=OUT) as tmp:
        for ipa in ipas:
            name, bid, h, files = build_app(ipa, Path(tmp))
            report.append({'ipa': ipa.name, 'app': name, 'bundle_id': bid, 'files': len(files), 'cdhashes': h})
            hashes += h
            for entry in files:
                apps += entries_for(*entry)
            print(f'{ipa.name}: {name} ({bid}), {len(files)} files, {len(h)} Mach-O signed')
    base = [f for f in m.read_addon(addon_in.read_bytes()) if not f[0].startswith('apps/')]
    if WITH_BASE:
        base_note = f'base addon files {len(base)}'
    else:
        base = []
        base_note = 'base addon files left on the Data volume (WITH_BASE=0)'
    parts = plan_parts(apps)
    total = len(parts)
    print(f'{base_note}; {len(apps)} app entries in {total} part(s), budget {BUDGET} bytes')
    for i, part in enumerate(parts, 1):
        print('  part %d: %d entries, %d bytes, largest %s' % (i, len(part), part_bytes(part),
                                                               max(part, key=lambda e: e[3])[0]))
    (OUT / 'trust.json').write_text(json.dumps({'cdhashes': sorted(set(hashes)), 'apps': report,
                                                'parts': [{'part': i, 'entries': len(p), 'bytes': part_bytes(p)}
                                                          for i, p in enumerate(parts, 1)]}, indent=2) + '\n')
    if want == 'auto':
        print(f'{len(set(hashes))} CDHashes in {OUT / "trust.json"}; plan only, nothing written')
        return
    if '/' in want:
        index, count = map(int, want.split('/'))
        if count != total:
            sys.exit(f'{want}: the inputs plan {total} part(s); use PART={index} or PART=auto')
        assert 1 <= index <= count, want
    else:
        index, count = int(want), total
        if not 1 <= index <= count:
            sys.exit(f'PART={index}: the inputs plan {count} part(s); use 1..{count} or PART=auto')
    mine = [(n, mode, data) for n, mode, data, _ in parts[index - 1]] + ([MARKER[:3]] if index == count else [])
    taken = {n for n, _, _ in mine}
    out = [f for f in base if f[0] not in taken] + mine
    blob = m.write_addon(out)
    assert m.read_addon(blob) == out
    addon_out.write_bytes(blob)
    print(f'part {index}/{count}: {addon_out}: {len(out)} files, {len(blob)} bytes; {len(set(hashes))} CDHashes in {OUT / "trust.json"}')


if __name__ == '__main__':
    main()