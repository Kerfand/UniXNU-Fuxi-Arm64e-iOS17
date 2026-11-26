#!/usr/bin/env python3
"""fuxi iOS 17 (2026-11-26; S18a: the logs in /private/var/tmp, 1777, which _wireless can open --
a root-owned /private/var/db file made launchd skip CommCenter, and an append without the two edits left launchd
without a service cache): the stock 21H16 launchd boot manifest with fuxi-logdump appended and CommCenter /
abm-helper given log files, at the binary-plist level (ljhhkx529/unixnu-fuxi scripts/make-fuxi-manifest.py, whose
append_launchd_job / set_job_logging this imports), signed detached with 4 KiB pages.

The program is /usr/sbin/fuxi-logdump: iOS 17 refuses to exec a program from the Data volume, so the binary rides the
addon's btd/ directory, which the kernel unions over /usr/sbin (ios17-zaudio-hal-mount.patch).

usage: make-fuxi-manifest.py STOCK OUTDIR   ->  OUTDIR/launchd.plist, launchd.plist.sig, trust.json
"""
import hashlib, importlib.util, json, plistlib, re, struct, subprocess, sys
from pathlib import Path

REF = Path('/home/xiaomi/ref/manifest-20261126/make-fuxi-manifest.py')
spec = importlib.util.spec_from_file_location('ljh_manifest', REF)
ljh = importlib.util.module_from_spec(spec); spec.loader.exec_module(ljh)

JOB_PATH = '/System/Library/LaunchDaemons/org.unixnu.fuxi.logdump.plist'
JOB = {
    'Label': 'org.unixnu.fuxi.logdump',
    'ProgramArguments': ['/usr/sbin/fuxi-logdump'],
    'RunAtLoad': True,
    'KeepAlive': False,
    'UserName': 'root',
    'ProcessType': 'Background',
    'com.apple.private.security.no-sandbox': True,
    'StandardOutPath': '/private/var/log/fuxi-logdump.log',
    'StandardErrorPath': '/private/var/log/fuxi-logdump.log',
}
CC = '/System/Library/LaunchDaemons/com.apple.CommCenter.plist'
ABM = '/System/Library/LaunchDaemons/com.apple.abm-helper.plist'


def detached_signature(data, identifier, page_shift=12):
    """SuperBlob {CodeDirectory, Requirements (empty), CMS wrapper (empty)}: an ad hoc detached signature."""
    requirements = struct.pack('>III', 0xfade0c01, 12, 0)
    page = 1 << page_shift
    code = [hashlib.sha256(data[i:i + page]).digest() for i in range(0, len(data), page)]
    special = [hashlib.sha256(requirements).digest(), bytes(32)]   # slot -2 requirements, slot -1 Info.plist (none)
    ident = identifier.encode() + b'\0'
    header = 88
    ident_offset = header
    hash_offset = ident_offset + len(ident) + 32 * len(special)
    length = hash_offset + 32 * len(code)
    cd = struct.pack('>9I', 0xfade0c02, length, 0x20400, 0x2, hash_offset, ident_offset, len(special), len(code), len(data))
    cd += struct.pack('>4B', 32, 2, 0, page_shift) + struct.pack('>4I', 0, 0, 0, 0) + struct.pack('>4Q', 0, 0, 0, 0)
    assert len(cd) == header
    cd += ident + b''.join(special) + b''.join(code)
    assert len(cd) == length
    cms = struct.pack('>II', 0xfade0b01, 8)
    blobs = [(0, cd), (2, requirements), (0x10000, cms)]
    at = 12 + 8 * len(blobs)
    index, body = b'', b''
    for kind, blob in blobs:
        index += struct.pack('>II', kind, at + len(body))
        body += blob
    return struct.pack('>III', 0xfade0cc0, 12 + len(index) + len(body), len(blobs)) + index + body


def main():
    stock_path, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    stock = stock_path.read_bytes()
    packed = ljh.append_launchd_job(stock, JOB_PATH, JOB)
    packed = ljh.set_job_logging(packed, CC, '/private/var/tmp/commcenter.log', '/private/var/tmp/commcenter.log')
    packed = ljh.set_job_logging(packed, ABM, '/private/var/tmp/abm-helper.log', '/private/var/tmp/abm-helper.log')
    check = plistlib.loads(packed)
    assert check['LaunchDaemons'].pop(JOB_PATH) == JOB
    for path, log in ((CC, '/private/var/tmp/commcenter.log'), (ABM, '/private/var/tmp/abm-helper.log')):
        job = check['LaunchDaemons'][path]
        assert job.pop('StandardOutPath') == log and job.pop('StandardErrorPath') == log
    assert check == plistlib.loads(stock), 'an original launchd entry changed'
    manifest = out / 'launchd.plist'
    manifest.write_bytes(packed)
    sig = out / 'launchd.plist.sig'
    # The WSL codesign is a Mach-O-only stand-in, so the detached signature is written here: what Apple's codesign
    # --detached --pagesize 4096 makes for a data file (ad hoc CodeDirectory v0x20400, SHA-256, empty requirements).
    sig.write_bytes(detached_signature(packed, 'com.fuxi.launchd.manifest'))
    cds = ljh.signature_directories(sig.read_bytes())
    hashes = []
    for cd in cds:
        offset, = struct.unpack_from('>I', cd, 16)
        slots, limit = struct.unpack_from('>II', cd, 28)
        size, bits = cd[36], cd[39]
        assert bits == 12 and limit == len(packed) and slots == (len(packed) + 4095) // 4096
        for i in range(slots):
            assert cd[offset + i * size:offset + (i + 1) * size] == hashlib.sha256(packed[i * 4096:(i + 1) * 4096]).digest()[:size]
        hashes.append(hashlib.sha256(cd).hexdigest()[:40])
    (out / 'trust.json').write_text(json.dumps({'cdhash': hashes[0], 'all': hashes, 'bytes': len(packed),
                                               'stock_sha256': hashlib.sha256(stock).hexdigest()}, indent=2) + '\n')
    print('manifest', len(stock), '->', len(packed), 'bytes; CodeDirectory hashes', hashes)


if __name__ == '__main__':
    main()
