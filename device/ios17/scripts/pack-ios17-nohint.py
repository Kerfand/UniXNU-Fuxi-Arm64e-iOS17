"""iOS 17.7 21H16 checkpoint 1: kernel-only boot.img (arm64e xnu-10063 + original fileset kexts).

For `fastboot boot` only: nothing is flashed and userdata is not touched. The kernel has no storage
driver and the ADT asks for a root volume UUID that does not exist, so a good run ends at the
root-device wait; an unmapped kext->kernel call ends in the unixnu_fileset_unmapped panic, which
names the call site.
"""
from pathlib import Path
import hashlib, json, re, shutil, struct, subprocess, time, tomllib, os

base = Path('/home/xiaomi/sadnut-fuxi-local-20261103')
kernel_dir = base / 'UniXNU/out-ios17/fuxi/kernel'
out = Path('/mnt/f/UniXNU-FUXI-work/outputs/sadnut-fuxi-ios17-arm64e-nohint-20261105')
previous = Path('/mnt/f/UniXNU-FUXI-work/outputs/fuxi17-usb-local-20261102')
work = base / 'pack-work-ios17-nohint'
out.mkdir(parents=True, exist_ok=True)
work.mkdir(exist_ok=True)
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
TITLE = 'FUXI IOS17 ARM64E KERNEL TEST 20261105'
NO_ROOT = 'D73A0000-0000-4000-8000-000000000000'   # no such volume: stop at the root-device wait


def parse(data):
    nodes = {}
    def step(off, parent=None):
        np_, nc = struct.unpack_from('<II', data, off); off += 8; props = {}
        for _ in range(np_):
            key, n = struct.unpack_from('<32sI', data, off); off += 36
            props[key.split(b'\0')[0].decode()] = data[off:off + n]; off += (n + 3) & ~3
        path = '/' if parent is None else parent.rstrip('/') + '/' + props['name'].rstrip(b'\0').decode()
        nodes[path] = props
        for _ in range(nc):
            off = step(off, path)
        return off
    assert step(0) == len(data)
    return nodes


kernel = kernel_dir / 'kernel.macho'
report = json.loads((kernel_dir / 'kernel.json').read_text())
assert sha(kernel) == report['candidate_sha256']
k = kernel.read_bytes()
assert struct.unpack_from('<4I', k) [:4] == (0xfeedfacf, 0x100000c, struct.unpack_from('<I', k, 8)[0], 12), 'not a fileset'
assert struct.unpack_from('<I', k, 8)[0] & 0xff == 2, 'not arm64e'
assert len(k) <= 128 * 1024 * 1024, 'DarwinBoot rejects a kernel over 128 MiB'

adt = work / 'kernel.macho.adt'
subprocess.run(['python3', str(base / 'device/scripts/make-fuxi-adt.py'), '--output', str(adt),
                '--dram-size', '0x80000000', '--owned-memory-size', '0x130000000',
                '--root-match-volume-uuid', NO_ROOT, '--unique-chip-id', 'ce854c9ae043320b', '--chip-id', '207',
                '--cpu-regs', '0,0x100,0x200,0x300,0x400,0x500,0x600,0x700',
                '--phone-identity', '--phone-model', 'iPhone15,2', '--trust-cache'], check=True)
nodes = parse(adt.read_bytes())
assert nodes['/']['model'] == b'iPhone15,2\0', nodes['/']['model']
assert 'text-fixups' not in nodes['/chosen'], 'no userland fixups in a kernel-only test'
assert len([n for n in nodes if n.startswith('/cpus/')]) == 8

args = ('serial=3 debug=0x14e -v kt_fixed_zones=120 qcom-usb-debug=1 sheng-log=1 armcpu-spi-ipi=1 sheng-smp=1 '
        '-no_slto_panic ml-timeout-lock-panic-scale=40 unixnu-unmapped=1 use_contiguous_hint=0')   # probe: log unmapped calls, return 0
assert len(args.encode()) <= 1000
template = (previous / 'pack-work/unixnu.toml').read_text()
first = template.index('[[entry]]')
reset = template.index('[[entry]]\nid = "reset"') if '[[entry]]\nid = "reset"' in template else None
head = template[:first]
head = re.sub(r'timeout = \d+', 'timeout = 8', head)
head = re.sub(r'default = "[^"]*"', 'default = "xnu"', head)
entry = ('[[entry]]\nid = "xnu"\ntitle = "%s"\ntype = "xnu"\npath = "Payloads/kernel.macho"\n'
         'args = "%s"\n' % (TITLE, args))   # no initrd: this ADT has no RAMDisk placeholder (DarwinBoot.c:664)
# The firmware's built-in DarwinBoot accepts only arm64 MH_EXECUTE kernels ("returned: Unsupported" on the
# fileset), and secure boot refuses a loose driver from the payload. The template's firmware volume carries
# DarwinBoot v2 instead (UniXNU drivers/DarwinBoot 3c75734: arm64e + MH_FILESET; build-darwinboot-fv.sh,
# make-t32-darwinboot-v2-template.py).
TEMPLATE = base / 'pack-work-ios17-kernel/t32-bl-daily-darwinboot-v2.img'
assert TEMPLATE.is_file()
drivers = ''
config = head + drivers + entry + (template[reset:] if reset is not None else '')
parsed = tomllib.loads(config)
assert parsed['boot']['default'] == 'xnu' and parsed['entry'][0]['args'] == args
assert 'driver' not in parsed and 'initrd' not in parsed['entry'][0]
(work / 'unixnu.toml').write_text(config)

payload = work / 'payload.img'
shutil.copy2(previous / 'pack-work/payload.img', payload)
env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
# Leftovers from the iPadOS payload: a GPU addon sidecar without a GpuAddon placeholder in this ADT is
# rejected (DarwinBoot.c:694), and the scratch ramdisk is no longer referenced.
for leftover in ['kernel.macho.gpuaddon', 'scratch-data.img']:
    subprocess.run(['mdel', '-i', str(payload), '::/EFI/UNIXNU/Payloads/' + leftover], env=env)
listing = subprocess.run(['mdir', '-b', '-i', str(payload), '::/EFI/UNIXNU/Payloads'], env=env,
                         capture_output=True, text=True, check=True).stdout
assert 'gpuaddon' not in listing and 'scratch-data' not in listing, listing
for source, dest in [(kernel, '::/EFI/UNIXNU/Payloads/kernel.macho'), (adt, '::/EFI/UNIXNU/Payloads/kernel.macho.adt'),
                     (work / 'unixnu.toml', '::/EFI/UNIXNU/unixnu.toml')]:
    subprocess.run(['mcopy', '-D', 'o', '-i', str(payload), str(source), dest], check=True, env=env)
readback = work / ('readback-' + str(time.time_ns())); readback.mkdir()
for name in ['kernel.macho', 'kernel.macho.adt']:
    subprocess.run(['mcopy', '-D', 'o', '-i', str(payload), '::/EFI/UNIXNU/Payloads/' + name, str(readback / name)], check=True, env=env)
subprocess.run(['mcopy', '-D', 'o', '-i', str(payload), '::/EFI/UNIXNU/unixnu.toml', str(readback / 'unixnu.toml')], check=True, env=env)
assert sha(readback / 'kernel.macho') == report['candidate_sha256']
assert sha(readback / 'kernel.macho.adt') == sha(adt)
assert (readback / 'unixnu.toml').read_text() == config

(out / 'payload').mkdir(exist_ok=True)
shutil.copy2(payload, out / 'payload/payload.img')
shutil.copy2(adt, out / 'payload/kernel.macho.adt')
shutil.copy2(kernel_dir / 'kernel.json', out / 'kernel.json')
packer = work / 'uefi-fuxi/scripts/bootimage.py'; packer.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(previous / 'pack-work/uefi-fuxi/scripts/bootimage.py', packer)
subprocess.run(['python3', str(packer), '--template', str(TEMPLATE),
                '--ramdisk', str(out / 'payload/payload.img'), '-o', str(out / 'boot.img')], check=True)
boot = json.loads((out / 'bootimage.json').read_text())
assert boot['header_version'] == 3 and boot['payload_sha256'] == sha(payload)
missing = report['missing']; stubbed = report['stubbed']
summary = dict(boot_img=str(out / 'boot.img'), boot_img_sha256=sha(out / 'boot.img'), kernel_sha256=report['candidate_sha256'],
               kernel_bytes=len(k), arch='arm64e', xnu='10063.141.1', kernelcache='iPhone15,2 21H16',
               adt_sha256=sha(adt), identity='iPhone15,2', template=str(TEMPLATE), template_sha256=sha(TEMPLATE), root_matching=NO_ROOT, kernel_args=args, menu_title=TITLE,
               closure=report['closure'], early_security_init=report['early_security_init'],
               stubbed_targets=sum(len(v) for v in stubbed.values()), stubbed_sites=sum(sum(v.values()) for v in stubbed.values()),
               missing_targets=sum(len(v) for v in missing.values()), missing_sites=sum(sum(v.values()) for v in missing.values()),
               flash='fastboot boot only; userdata untouched', hardware_tested=False)
(out / 'pack.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2))
print('PACK_COMPLETE: ' + str(out / 'boot.img'), flush=True)
