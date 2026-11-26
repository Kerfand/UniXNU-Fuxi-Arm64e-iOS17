"""Check sideload addon parts together: the firmware's 64 MiB addon read cap, and that the offset-named chunks of one bundle
file tile its byte range exactly once across the whole plan (what the kernel's zzd chunk write assumes: no gap, no overlap).

  check_part.py PART1.bin [PART2.bin ...]
"""
import importlib.util
import re
import sys
import zlib

spec = importlib.util.spec_from_file_location(
    'addfx', '/home/xiaomi/sadnut-fuxi-local-20261103/device-ios17/scripts/add-fuxi-wallpaper-to-addon.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

CAP = 64 * 1024 * 1024
CHUNK = re.compile(r'^(.*)#([0-9a-f]{8})$')


def main():
    failed = False
    groups, plain, marker = {}, [], False
    for path in sys.argv[1:]:
        blob = open(path, 'rb').read()
        files = m.read_addon(blob)
        over = len(blob) > CAP
        print(f'{path}: {len(files)} files, {len(blob)} bytes ({len(blob) / 2**20:.2f} MiB); cap 64 MiB -> {"OVER" if over else "OK"}')
        failed |= over
        for name, mode, data in files:
            match = CHUNK.match(name)
            if match and name.startswith('apps/'):
                groups.setdefault(match.group(1), []).append((int(match.group(2), 16), len(data)))
            else:
                plain.append(name)
                if name == 'apps/.fuxi-sideload':
                    marker = True
    print(f'plain entries {len(plain)}, chunked files {len(groups)}, marker {marker}')
    for base, pieces in sorted(groups.items()):
        pieces.sort()
        at = 0
        for offset, size in pieces:
            if offset != at:
                print(f'  FAIL {base}: gap or overlap, expected {at}, next piece at {offset}')
                failed = True
                break
            at = offset + size
        else:
            expected = max(offset + size for offset, size in pieces)
            print(f'  OK   {base}: {len(pieces)} pieces tile 0..{at} ({at / 2**20:.2f} MiB)')
    print('RESULT', 'FAIL' if failed else 'OK')


if __name__ == '__main__':
    main()