#!/usr/bin/env python3
"""iOS 17.7 21H16 (arm64e): build the owned NativeLegacyDisplay on the unchanged original IOMobileFramebufferLegacy.

The original class has the 21H461 ABI (0x18a0 bytes, 358 vtable slots, the same abstract leaves and overrides; see
F:/UniXNU_Claude/ios17-display-port.md), so the iPadOS class declaration is reused. arm64e adds two requirements:
  * every vtable entry is signed with the string discriminator of the mangled name of the declaration that introduced
    the slot. The private Legacy methods (slots 168..357) are opaque here, so their names are chosen (a suffix found by
    search) to give exactly the original discriminator of each slot; the build fails unless all 358 entries of the
    compiled vtable match the original's discriminator, key and address diversity.
  * calls to original code go through BTI/B adapters (<name> + absolute <name>.original), which the prelinker
    retargets (fileset_layout.relocate_original_stubs); data (gMetaClass, the vtable) stays absolute.
Output: build/native-legacy-display/libNativeLegacyDisplay.late.a + build-report.json (checked by the kernel link)."""
import hashlib, json, os, re, struct, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-legacy-display'
OUT.mkdir(parents=True, exist_ok=True)
KC = ROOT.parents[1] / 'ipsw/21H16/kernelcache.decompressed'
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
sys.path.insert(0, str(ROOT / 'scripts'))
from kc_image import Image
from kernel_target import TRIPLE


def require(value, why):
    if not value:
        raise SystemExit('build-ios17-legacy-display: ' + why)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


# ---- original 21H16 facts (verified by scratchpad vtscan/vtcmp/legloc/surfget/vsyncloc/vsyncchk) ----
LEG_VT = 0xfffffff007c46698
PURE = [211, 213, 245, 246, 306, 307, 337, 338, 345]
DATA = {'__ZN25IOMobileFramebufferLegacy10gMetaClassE': 0xfffffff00a46aab8,
        '__ZTV25IOMobileFramebufferLegacy': LEG_VT - 16}
FUNCS = {'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': 0xfffffff0099f7dcc,
         '__ZN25IOMobileFramebufferLegacyD2Ev': 0xfffffff0099f7e24,
         '_native_legacy_surface_descriptor': 0xfffffff009b55568, '_native_legacy_surface_format': 0xfffffff009b55668,
         '_native_legacy_surface_width': 0xfffffff009b55680, '_native_legacy_surface_height': 0xfffffff009b5568c,
         '_native_legacy_surface_stride': 0xfffffff009b556a4, '_native_legacy_surface_planes': 0xfffffff009b556d4,
         '_native_legacy_surface_allocation': 0xfffffff009b556e0,
         '_native_legacy_vsync_notify': 0xfffffff009a0b498, '_native_legacy_set_refresh_properties': 0xfffffff009a157b4}
GETTERS = {'descriptor': (48, 8), 'format': (128, 4), 'width': (88, 8), 'height': (96, 8), 'stride': (104, 8),
           'planes': (152, 4), 'allocation': (144, 8)}
NAMED = {184: ('bool', 'isMainDisplay', '', ''), 188: ('IOReturn', 'getDotPitch', 'uint32_t *', ''),
         213: ('IOReturn', 'supportedFrameInfo', 'uint32_t,uint32_t *,char *,uint32_t *', '=0'),
         249: ('void', 'getDisplayDimensions', 'uint32_t *,uint32_t *', ''), 290: ('void', 'kickSwapQueue', '', ''),
         296: ('bool', 'validateSwap', 'const OriginalLegacySwapRequest *', ''), 295: ('void', 'completeCurrentSwap', '', ''),
         337: ('IOReturn', 'collectCompletionReports', 'uint32_t,void *,uint32_t *,uint32_t', '=0'),
         288: ('bool', 'startHardware', '', ''), 354: ('int', 'applyStateRequest', 'OriginalLegacyStateRequest *,bool', ''),
         301: ('bool', 'getRefreshAttributes', 'uint64_t *,bool', '')}

raw = KC.read_bytes()
cache = Image(raw)
fx = cache.fixups()
original = [fx.get(LEG_VT + 8 * i) for i in range(358)]
require(all(original) and fx.get(LEG_VT + 8 * 358) is None, 'original 358-entry Legacy vtable')
require(len({original[i][0] for i in PURE}) == 1, 'one __cxa_pure_virtual target for the abstract slots')
require(all(e[1] for e in original), 'every original vtable entry is authenticated')
# The Legacy vtable re-signs the four abstract slots the Framebuffer base introduced (211, 213, 245, 246) with its own
# pure redeclarations; the original callers (through the base type) and so a real override use the base discriminator
# (2026-11-07 20:20: FPAC calling slot 213 with 0xbd70 while the display was signed with the Legacy entry's 0x7dc3).
FRAMEBUFFER_VT = 0xfffffff007c447a8
target = [(e[3], e[2], e[4]) for e in original]
for i in PURE:
    if i < 262:
        b = fx[FRAMEBUFFER_VT + 8 * i]
        require(b[0] == original[i][0], f'slot {i} is abstract in the Framebuffer base too')
        target[i] = (b[3], b[2], b[4])
for name, (field, size) in GETTERS.items():
    w = struct.unpack('<3I', cache.read(FUNCS['_native_legacy_surface_' + name], 12))
    if w[0] == 0xd503245f:   # arm64e entry: bti c
        w = w[1:]
    ldr = (0xf9400000 if size == 8 else 0xb9400000) | ((field // size) << 10)
    require(w[:2] == (ldr, 0xd65f03c0), 'original IOSurface getter ' + name)

# ---- class declaration: the iPadOS fixed part, then slots 168..357 with discriminator-matching names ----
ipad = (ROOT / 'scripts/build-native-legacy-display.py').read_text()
fixed = re.search(r"header='''(.*?)'''\n", ipad, re.S)
require(fixed, 'iPadOS Legacy header template')
FIXED = fixed.group(1)


def header(suffix):
    text = FIXED
    defines = []
    for i in range(168, 358):
        s = suffix.get(i, '')
        if i in NAMED:
            ret, name, params, pure = NAMED[i]
            text += f' virtual {ret} {name}{s}({params}){pure};\n'
            if s:
                defines.append(f'#define {name} {name}{s}\n')
        elif i in PURE:
            text += f' virtual __attribute__((noreturn)) void opaque{i}{s}()=0;\n'
            if s:   # the display's LEAF(n) pastes opaque##n and passes n as the slot number: rename by macro
                defines.append(f'#define opaque{i} opaque{i}{s}\n')
        else:
            text += f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}{s}();\n'
    text += '};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");\n'
    leaves = [i for i in PURE if i not in (213, 337)]
    text += '#define NATIVE_LEGACY_LEAVES(X) ' + ' '.join(f'X({i})' for i in leaves) + '\n'
    text += 'static constexpr unsigned kLegacyVsyncClients = 5592;\n'
    # Source-level names of the renamed declarations (the display overrides and calls them by these names).
    # 21H16 swap_submit gates on the requested-state word (+0x16cc): the display announces it with the powered word.
    pre = ('// Generated by build-ios17-legacy-display.py (21H16 arm64e).\n#define NATIVE_LEGACY_ANNOUNCE_REQUESTED 1\n'
           + ''.join(defines))
    return text.replace('#pragma once\n', '#pragma once\n' + pre, 1)


# ---- Mach-O object reading (symbols, the display vtable's relocations) ----
def macho(path):
    d = Path(path).read_bytes()
    require(struct.unpack_from('<I', d)[0] == 0xfeedfacf, f'{path} is a 64-bit Mach-O object')
    ncmds = struct.unpack_from('<I', d, 16)[0]
    off, sections, symtab = 32, [], None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', d, off + 64)[0]
            for s in range(nsects):
                so = off + 72 + 80 * s
                sect, seg = d[so:so + 16].rstrip(b'\0').decode(), d[so + 16:so + 32].rstrip(b'\0').decode()
                addr, sz, fo, _, reloff, nreloc = struct.unpack_from('<QQIIII', d, so + 32)
                sections.append(dict(seg=seg, sect=sect, addr=addr, size=sz, off=fo, reloff=reloff, nreloc=nreloc))
        elif cmd == 2:
            symtab = struct.unpack_from('<4I', d, off + 8)
        off += size
    symoff, nsyms, stroff, _ = symtab
    syms = []
    for i in range(nsyms):
        strx, ntype, nsect, ndesc, value = struct.unpack_from('<IBBHQ', d, symoff + 16 * i)
        name = d[stroff + strx:d.index(b'\0', stroff + strx)].decode()
        syms.append(dict(name=name, type=ntype, sect=nsect, value=value))
    return d, sections, syms


def vtable_entries(path):
    d, sections, syms = macho(path)
    vt = next((s for s in syms if s['name'] == '__ZTV19NativeLegacyDisplay' and s['type'] & 0x0e == 0x0e), None)
    require(vt, 'compiled vtable __ZTV19NativeLegacyDisplay')
    sec = next(s for s in sections if s['addr'] <= vt['value'] < s['addr'] + s['size'])
    relocs = {}
    for k in range(sec['nreloc']):
        address, info = struct.unpack_from('<iI', d, sec['reloff'] + 8 * k)
        symnum, extern, rtype = info & 0xffffff, (info >> 27) & 1, info >> 28
        relocs[sec['addr'] + address] = (syms[symnum]['name'] if extern else None, rtype)
    entries = []
    for i in range(358):
        at = vt['value'] + 16 + 8 * i
        name, rtype = relocs.get(at, (None, None))
        require(name and rtype == 11, f'slot {i}: authenticated pointer relocation to a symbol ({name}, type {rtype})')
        word = struct.unpack_from('<Q', d, sec['off'] + at - sec['addr'])[0]
        entries.append(dict(name=name, div=(word >> 32) & 0xffff, addr=(word >> 48) & 1, key=(word >> 49) & 3,
                            auth=word >> 63))
    require((vt['value'] + 16 + 8 * 358) not in relocs, 'exactly 358 compiled vtable entries')
    return entries, syms


# ---- discriminator search (clang ptrauth string discriminator: SipHash-2-4, (h % 0xffff) + 1) ----
SEARCH_C = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static const uint8_t K[16]={0xb5,0xd4,0xc9,0xeb,0x79,0x10,0x4a,0x79,0x6f,0xec,0x8b,0x1b,0x42,0x87,0x81,0xd4};
#define ROTL(x,b) (uint64_t)(((x)<<(b))|((x)>>(64-(b))))
#define RND do{v0+=v1;v1=ROTL(v1,13);v1^=v0;v0=ROTL(v0,32);v2+=v3;v3=ROTL(v3,16);v3^=v2;v0+=v3;v3=ROTL(v3,21);v3^=v0;v2+=v1;v1=ROTL(v1,17);v1^=v2;v2=ROTL(v2,32);}while(0)
static uint64_t sip(const uint8_t *in,size_t n){
 uint64_t k0,k1;memcpy(&k0,K,8);memcpy(&k1,K+8,8);
 uint64_t v0=k0^0x736f6d6570736575ULL,v1=k1^0x646f72616e646f6dULL,v2=k0^0x6c7967656e657261ULL,v3=k1^0x7465646279746573ULL;
 size_t i;for(i=0;i+8<=n;i+=8){uint64_t m;memcpy(&m,in+i,8);v3^=m;RND;RND;v0^=m;}
 uint64_t b=((uint64_t)n)<<56;for(size_t j=0;j<n-i;j++)b|=((uint64_t)in[i+j])<<(8*j);
 v3^=b;RND;RND;v0^=b;v2^=0xff;RND;RND;RND;RND;return v0^v1^v2^v3;}
static unsigned disc(const char *s){return (unsigned)(sip((const uint8_t*)s,strlen(s))%0xffff)+1;}
int main(void){char pre[512],name[256],rest[512];unsigned want;
 while(scanf("%511s %255s %511s %x",pre,name,rest,&want)==4){
  if(!strcmp(rest,"-"))rest[0]=0;
  char buf[1400],suf[32];int found=0;
  for(unsigned long n=0;n<100000000UL&&!found;n++){
   snprintf(suf,sizeof suf,"_u%lx",n);
   char full[300];snprintf(full,sizeof full,"%s%s",name,suf);
   snprintf(buf,sizeof buf,"%s%zu%s%s",pre,strlen(full),full,rest);
   if(disc(buf)==want){printf("%s\n",suf);found=1;}}
  if(!found)printf("-\n");fflush(stdout);}
 return 0;}
'''


def disc(text):
    import importlib
    k = bytes([0xb5, 0xd4, 0xc9, 0xeb, 0x79, 0x10, 0x4a, 0x79, 0x6f, 0xec, 0x8b, 0x1b, 0x42, 0x87, 0x81, 0xd4])
    M = (1 << 64) - 1
    def rotl(x, b): return ((x << b) | (x >> (64 - b))) & M
    k0, k1 = struct.unpack('<QQ', k)
    v = [k0 ^ 0x736f6d6570736575, k1 ^ 0x646f72616e646f6d, k0 ^ 0x6c7967656e657261, k1 ^ 0x7465646279746573]
    def rnd():
        v[0] = (v[0] + v[1]) & M; v[1] = rotl(v[1], 13); v[1] ^= v[0]; v[0] = rotl(v[0], 32)
        v[2] = (v[2] + v[3]) & M; v[3] = rotl(v[3], 16); v[3] ^= v[2]
        v[0] = (v[0] + v[3]) & M; v[3] = rotl(v[3], 21); v[3] ^= v[0]
        v[2] = (v[2] + v[1]) & M; v[1] = rotl(v[1], 17); v[1] ^= v[2]; v[2] = rotl(v[2], 32)
    data = text.encode()
    n = len(data)
    for i in range(0, n - n % 8, 8):
        m = struct.unpack_from('<Q', data, i)[0]; v[3] ^= m; rnd(); rnd(); v[0] ^= m
    b = (n & 0xff) << 56
    for j, c in enumerate(data[n - n % 8:]):
        b |= c << (8 * j)
    v[3] ^= b; rnd(); rnd(); v[0] ^= b; v[2] ^= 0xff
    for _ in range(4):
        rnd()
    return ((v[0] ^ v[1] ^ v[2] ^ v[3]) % 0xffff) + 1


MANGLED = re.compile(r'^_ZN(K?)(19NativeLegacyDisplay|25IOMobileFramebufferLegacy)(\d+)(.*)$')


def introducing(symbol):
    """Mangled name of the Legacy declaration behind a vtable entry symbol (override or inherited)."""
    m = MANGLED.match(symbol[1:])
    require(m, 'unexpected Legacy vtable symbol ' + symbol)
    const, _, length, tail = m.groups()
    name, rest = tail[:int(length)], tail[int(length):]
    return f'_ZN{const}25IOMobileFramebufferLegacy', name, rest


def compile_display(suffix):
    (OUT / 'OriginalLegacyFramebufferABI.h').write_text(header(suffix))
    subprocess.run(['python3', str(ROOT / 'scripts/build-arm-driver.py'), 'NativeLegacyDisplay'], check=True)
    return vtable_entries(OUT / 'NativeLegacyDisplay.o')


# Pass 1: no suffixes. Every slot below 168 (IOService ancestry) must already match; the Legacy slots are renamed.
entries, _ = compile_display({})
bad_low = [i for i in range(168) if (entries[i]['div'], entries[i]['key'], entries[i]['addr']) != target[i]]
require(not bad_low, f'IOService-ancestry vtable discriminators differ from the original at slots {bad_low}')
jobs = []
for i in range(168, 358):
    e = entries[i]
    pre, name, rest = introducing(e['name']) if e['name'] != '___cxa_pure_virtual' else (None, None, None)
    require(pre, f'slot {i} is still abstract in the compiled display ({e["name"]})')
    require(disc(pre + str(len(name)) + name + rest) == e['div'], f'slot {i}: discriminator model disagrees with clang')
    if e['div'] != target[i][0]:
        jobs.append((i, pre, name, rest or '-', target[i][0]))
print(f'build-ios17-legacy-display: {len(jobs)} Legacy slots need discriminator-matching names')
suffix = {}
if jobs:
    with tempfile.TemporaryDirectory() as t:
        src, exe = Path(t, 's.c'), Path(t, 's')
        src.write_text(SEARCH_C)
        # A host tool: the build's PATH/environment points cc at the Darwin cross toolchain, so use the host compiler
        # with a clean environment.
        host_cc = next((c for c in ('/usr/bin/gcc', '/usr/bin/cc') if os.path.exists(c)), 'cc')
        host_env = {k: v for k, v in os.environ.items() if k not in ('CPATH', 'C_INCLUDE_PATH', 'SDKROOT', 'CFLAGS',
                                                                       'CC', 'LIBRARY_PATH', 'COMPILER_PATH')}
        host_env['PATH'] = '/usr/bin:/bin'
        subprocess.run([host_cc, '-O2', '-o', str(exe), str(src)], check=True, env=host_env)
        res = subprocess.run([str(exe)], input=''.join(f'{p} {n} {r} {w:x}\n' for _, p, n, r, w in jobs),
                             capture_output=True, text=True, check=True).stdout.split()
    for (i, *_), s in zip(jobs, res):
        require(s != '-', f'no discriminator-matching name found for slot {i}')
        suffix[i] = s
# Pass 2: the renamed declaration; all 358 entries must now sign exactly like the original.
entries, syms = compile_display(suffix)
for i in range(358):
    e, o = entries[i], target[i]
    require(e['auth'] and (e['div'], e['key'], e['addr']) == o,
            f'slot {i} ({e["name"]}): div/key/addr {e["div"]:#x}/{e["key"]}/{e["addr"]} vs original {o[0]:#x}/{o[1]}/{o[2]}')
owned = [i for i, e in enumerate(entries) if e['name'].startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay'))]
require(set(PURE) <= set(owned), 'every abstract Legacy leaf is implemented by the display')

# ---- bindings ----
undefined = {s['name'] for s in syms if s['type'] & 0x0e == 0 and s['type'] & 1}
functions = dict(FUNCS)
for i, e in enumerate(entries):
    if '25IOMobileFramebufferLegacy' in e['name']:
        require(i not in PURE, f'abstract slot {i} bound to the original')
        functions[e['name']] = original[i][0]
legacy_imports = {n for n in undefined if 'IOMobileFramebufferLegacy' in n or n in FUNCS}
require(legacy_imports <= set(functions) | set(DATA), f'unbound original imports {sorted(legacy_imports - set(functions) - set(DATA))}')
bound = {n: functions[n] for n in sorted(legacy_imports) if n in functions}
absolute = ['// Generated by build-ios17-legacy-display.py: original 21H16 addresses.']
for name, value in sorted(DATA.items()):
    if name in undefined:
        absolute += [f'.globl "{name}"', f'.set "{name}", {value:#x}']
for name, value in bound.items():
    absolute += [f'.globl "{name}.original"', f'.set "{name}.original", {value:#x}']
adapters = ['// Generated by build-ios17-legacy-display.py: BTI/B adapters to original 21H16 code.', '.text']
for name in bound:
    adapters += [f'.globl "{name}"', '.p2align 2', f'"{name}":', '  bti c', f'  b "{name}.original"']
(OUT / 'legacy-original-bindings.S').write_text('\n'.join(absolute) + '\n')
(OUT / 'legacy-original-adapters.S').write_text('\n'.join(adapters) + '\n')
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                    .read_text().strip().rstrip(','))
clang = record['arguments'][0]
for stem in ('legacy-original-bindings', 'legacy-original-adapters'):
    run([clang, '-target', TRIPLE, '-c', OUT / f'{stem}.S', '-o', OUT / f'{stem}.o'])

# ---- the probe/lifecycle object, compiled like the platform expert ----
probe_src = ROOT / 'scripts/data/Ios17LegacyDisplayProbe.cpp'
probe_obj = OUT / 'Ios17LegacyDisplayProbe.o'
args, cmd, k = record['arguments'], [], 0
while k < len(args):
    a = args[k]
    if a == record['file']:
        cmd.append(str(probe_src))
    elif a == '-o':
        cmd += ['-o', str(probe_obj)]; k += 1
    elif a == '-Xclang' and args[k + 1].startswith('-W'):
        k += 1
    elif not a.startswith('-W'):
        cmd.append(a)
    k += 1
cmd += ['-Wno-unknown-warning-option', '-I', str(ROOT / '.cache/xnu-work/osfmk')]
subprocess.run(cmd, cwd=record['directory'], check=True)
# Board GPU bring-up (power, GMU cold start, CP init, publish), compiled like the probe with the Adreno includes.
# The iOS 17 native __DATA has ~20 KiB left (the retained AppleARMPlatform data follows it): the 128 KiB command-DB
# copy moves to the heap, allocated where the probe first maps the command DB. The shared source stays unchanged.
gpupower_text = (ROOT / 'kits/sheng/GpuPowerProbe.cpp').read_text()
for old_text, new_text in (
        ('alignas(8) uint8_t gCmdDb[kCmdDbBytes];', 'uint8_t *gCmdDb;   // ios17: heap copy, see build-ios17-legacy-display.py'),
        ('    if (volatile uint32_t *db = mapWords(kCmdDbBase, kCmdDbBytes, gCmdDbMap)) {',
         '    if (!gCmdDb) gCmdDb = static_cast<uint8_t *>(IOMallocAligned(kCmdDbBytes, 8));\n'
         '    if (volatile uint32_t *db = gCmdDb ? mapWords(kCmdDbBase, kCmdDbBytes, gCmdDbMap) : nullptr) {'),
        # No boot-progress screen on iOS 17: each diagnostic row also goes to the kernel log (USB console) when its text changes.
        ('&gDeviceInfo, 32, 40);', '&gDeviceInfo, ios17PublishInputBits(), 40);'),
        ('            setRow(2, stall);\n',
         '            setRow(2, stall);\n'
         '            {   // ios17: the IB the CP stopped in, through the context table, as DRAM holds it (uncached reads)\n'
         '                static uint64_t dumped;\n'
         '                const uint64_t ib = (uint64_t(gpuRd(0x0929)) << 32) | gpuRd(0x0928);\n'
         '                if (ib && ib != dumped) {\n'
         '                    dumped = ib;\n'
         '                    uint64_t pa = 0;\n'
         '                    const uint64_t desc = gpuWalk(ib, &pa);\n'
         '                    uint32_t w[8] = {};\n'
         '                    for (unsigned i = 0; pa && i < 8; ++i) dramRead32(pa + 4 * i, &w[i]);\n'
         '                    printf("gpu stall ib1 %llx rem %x desc %llx pa %llx dram: %08x %08x %08x %08x %08x %08x %08x %08x\\n", ib, gpuRd(0x092a),\n'
         '                           desc, pa, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);\n'
         '                }\n'
         '            }\n'),
        ('    boot_progress_ext[slot][47] = 0;\n}\n',
         '    boot_progress_ext[slot][47] = 0;\n'
         '    static char printed[8][48];\n'
         '    if (slot < 8 && strncmp(printed[slot], boot_progress_ext[slot], 47)) {\n'
         '        memcpy(printed[slot], boot_progress_ext[slot], 48);\n'
         '        printf("gpu row %u: %s\\n", slot, boot_progress_ext[slot]);\n'
         '    }\n'
         # A refused user-client mount (AGXDeviceUserClient::mount: stage 1 core open, 2 process VM, 3 queue scope; status).
         '    static uint32_t mountFails;\n'
         '    if (&native_adreno_mount_fail && native_adreno_mount_fail[3] != mountFails) {\n'
         '        mountFails = native_adreno_mount_fail[3];\n'
         '        printf("gpu mount fail #%u stage %u status %#x hi %#x\\n", mountFails, native_adreno_mount_fail[0], native_adreno_mount_fail[1], native_adreno_mount_fail[2]);\n'
         '    }\n'
         '    static uint32_t cpSeen[11] = {0xffffffffu};\n'
         '    if (&native_adreno_cp_aperture && &native_adreno_cp_check && &native_adreno_recover_log_count) {\n'
         '        const uint32_t now[11] = {native_adreno_cp_aperture[0], native_adreno_cp_aperture[1], native_adreno_cp_aperture[2],\n'
         '            native_adreno_cp_aperture[3], native_adreno_cp_check[0], native_adreno_cp_check[1], native_adreno_cp_check[2],\n'
         '            native_adreno_cp_check[3], native_adreno_cp_check[4], native_adreno_cp_check[5], native_adreno_recover_log_count};\n'
         '        if (memcmp(now, cpSeen, sizeof now)) {\n'
         '            memcpy(cpSeen, now, sizeof now);\n'
         '            printf("gpu cp aperture %x/%x check %x mode %u switch %x/%x/%x/%x/%x/%x restarts %u\\n", now[0], now[1], now[2], now[3],\n'
         '                   now[4], now[5], now[6], now[7], now[8], now[9], now[10]);\n'
         '        }\n'
         '    }\n'
         '    static uint32_t ibSeen;\n'
         '    if (&native_adreno_ib_log_count) {\n'
         '        for (uint32_t n = native_adreno_ib_log_count; ibSeen < n; ++ibSeen) {\n'
         '            if (n - ibSeen > 4) continue;   /* older records were overwritten */\n'
         '            const volatile uint32_t *r = native_adreno_ib_log[ibSeen & 3];\n'
         '            printf("gpu ib #%u seq %u iova %x%08x bytes %u: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\\n", ibSeen, r[0], r[2], r[1], r[3],\n'
         '                   r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);\n'
         '        }\n'
         '    }\n'
         '    static uint32_t wcSeen[16] = {0xffffffffu};\n'
         '    if (&native_adreno_wc_audit && &native_adreno_coherence) {\n'
         '        uint32_t now[16] = {};\n'
         '        for (unsigned i = 0; i < 6; ++i) now[i] = native_adreno_wc_audit[i];\n'
         '        now[6] = &native_adreno_wc_distrusted ? native_adreno_wc_distrusted : 0;\n'
         '        for (unsigned i = 0; i < 8; ++i) now[7 + i] = native_adreno_coherence[i];\n'
         '        now[15] = &native_adreno_wc_enabled ? native_adreno_wc_enabled : 0xff;\n'
         '        if (memcmp(now, wcSeen, sizeof now)) {\n'
         '            memcpy(wcSeen, now, sizeof now);\n'
         '            printf("gpu wc %u audit %u lines %u dirty %u stale %u st %x reset %u distrust %u coh %u %x/%x/%x mism %u %x/%x/%x\\n", now[15],\n'
         '                   now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9], now[10], now[11], now[12], now[13], now[14]);\n'
         '        }\n'
         '    }\n'
         '    static uint32_t swSeen[16] = {0xffffffffu};\n'
         '    if (&native_legacy_swapstat) {\n'
         '        uint32_t now[16];\n'
         '        for (unsigned i = 0; i < 16; ++i) now[i] = native_legacy_swapstat[i];\n'
         '        if (memcmp(now, swSeen, sizeof now)) {\n'
         '            memcpy(swSeen, now, sizeof now);\n'
         '            printf("gpu swapstat ok %u kick %u xfer %u notready %u unvalidated %u direct %u fence %u present %u blank %u flip %u latch %u done %u slow %u max %u total %u depth %u\\n",\n'
         '                   now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9], now[10], now[11], now[12], now[13], now[14], now[15]);\n'
         '        }\n'
         '    }\n}\n')):
    if gpupower_text.count(old_text) != 1:
        raise SystemExit('GpuPowerProbe.cpp changed: ' + old_text)
    gpupower_text = gpupower_text.replace(old_text, new_text)
# setRow lives in an anonymous namespace: the mount-failure record is declared once at file scope.
gpupower_text = ('#include <stdint.h>\nextern "C" volatile uint32_t native_adreno_mount_fail[4] __attribute__((weak));\n'
                 'extern "C" unsigned NativeAdrenoGpuContextInputBits() __attribute__((weak));\n'
                 'extern "C" volatile uint32_t native_adreno_cp_aperture[4] __attribute__((weak));\n'
                 'extern "C" volatile uint32_t native_adreno_cp_check[6] __attribute__((weak));\n'
                 'extern "C" volatile uint32_t native_adreno_recover_log_count __attribute__((weak));\n'
                 'extern "C" volatile uint32_t native_adreno_ib_log[4][32] __attribute__((weak));\n'
                 'extern "C" volatile uint32_t native_adreno_ib_log_count __attribute__((weak));\n'
                 'extern "C" volatile uint32_t native_adreno_wc_audit[6] __attribute__((weak));\n'
                 'extern "C" volatile uint32_t native_adreno_wc_distrusted __attribute__((weak));\n'
                 'extern "C" volatile uint32_t native_adreno_coherence[8] __attribute__((weak));\n'
                 'extern "C" volatile uint32_t native_legacy_swapstat[16] __attribute__((weak));\n'
                 'extern "C" int printf(const char *, ...);\n'
                 '// The GPU context\'s own input width (the SMMU IAS), as the driver\'s per-connection windows need (> 32).\n'
                 'static unsigned ios17PublishInputBits() {\n'
                 '    unsigned bits = NativeAdrenoGpuContextInputBits ? NativeAdrenoGpuContextInputBits() : 0;\n'
                 '    printf("gpu publish input bits %u\\n", bits);\n'
                 '    return bits > 32 && bits <= 48 ? bits : 32;\n'
                 '}\n'
                 + gpupower_text)
gpupower_src = OUT / 'GpuPowerProbe.ios17.cpp'
gpupower_src.write_text(gpupower_text)
gpupower_obj = OUT / 'GpuPowerProbe.o'
subprocess.run([str(gpupower_src) if a == str(probe_src) else str(gpupower_obj) if a == str(probe_obj) else a for a in cmd]
               + ['-I', str(ROOT / 'kits/sheng'), '-I', str(ROOT / 'drivers/Adreno740'), '-I', str(ROOT / 'kits/ArmSmmu4K'),
                  '-I', str(ROOT / 'kits/AdrenoFirmware'), '-I', str(ROOT / 'kits/AdrenoPM4'), '-I', str(ROOT / 'build/fuxi-gmu')],
               cwd=record['directory'], check=True)

# The AppleKeyStore stand-in, as the iPadOS build-native-legacy-display.py builds it: the probe that registers it (compiled like the
# lifecycle probe) and the service itself (build-arm-driver.py NativeKeyStoreEvents, its static initializer run by the probe).
# The native __DATA ends 0x170 bytes past its last 16 KiB page with the full counters (0x174000 overlaps the retained
# AppleARMPlatform __DATA at 0x171070): the copy keeps per-selector counts as uint16 and the error flag as one byte.
keystore_text = (ROOT / 'kits/sheng/KeyStoreProbe.cpp').read_text()
for old_text, new_text in (
        ('static volatile uint32_t aks_count[96];', 'static volatile uint16_t aks_count[104];'),
        ('static volatile int32_t aks_rc[96];', 'static volatile uint8_t aks_rc[104];'),
        ('for (unsigned sel = 0; sel < 96; ++sel) {', 'for (unsigned sel = 0; sel < 104; ++sel) {'),
        ('if (selector < 96) { aks_count[selector] = aks_count[selector] + 1; aks_rc[selector] = result; }',
         'if (selector < 104) { aks_count[selector] = (uint16_t)(aks_count[selector] + 1); aks_rc[selector] = result != 0; }')):
    require(keystore_text.count(old_text) == 1, 'KeyStoreProbe anchor: ' + old_text)
    keystore_text = keystore_text.replace(old_text, new_text)
keystore_src = OUT / 'KeyStoreProbe.ios17.cpp'
keystore_src.write_text(keystore_text)
keystore_obj = OUT / 'KeyStoreProbe.o'
subprocess.run([str(keystore_src) if a == str(probe_src) else str(keystore_obj) if a == str(probe_obj) else a for a in cmd],
               cwd=record['directory'], check=True)
subprocess.run(['python3', str(ROOT / 'scripts/build-arm-driver.py'), 'NativeKeyStoreEvents'], check=True)
keyevents_late = ROOT / 'build/native-keystore-events/NativeKeyStoreEvents.late.o'
run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp',
     ROOT / 'build/native-keystore-events/NativeKeyStoreEvents.o', keyevents_late])

# The launchd job policy (kits/sheng/BootWatchdogPolicy.c), called by ios17-job-policy.patch after the scratch Data mount,
# compiled like kern_exec.c as the iPadOS build-boot-progress.py compiles it.
policy_record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json')
                           .read_text().strip().rstrip(','))
policy_src = ROOT / 'kits/sheng/BootWatchdogPolicy.c'
policy_obj = OUT / 'BootWatchdogPolicy.o'
pargs, pcmd, k = policy_record['arguments'], [], 0
while k < len(pargs):
    a = pargs[k]
    if a == policy_record['file']:
        pcmd.append(str(policy_src))
    elif a == '-o':
        pcmd += ['-o', str(policy_obj)]; k += 1
    elif a in ('-MF', '-MT', '-MJ'):
        k += 1
    elif a == '-Xclang' and pargs[k + 1].startswith('-W'):
        k += 1
    elif not a.startswith('-W'):
        pcmd.append(a)
    k += 1
subprocess.run(pcmd + ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror'], cwd=policy_record['directory'], check=True)

# The fuxi touch stack (as the iPadOS build-fuxi-touch.py / build-native-legacy-display.py build it): the board probe, its HID
# glue and the HID probe compiled like the lifecycle probe; the FTM5 protocol, contact tracking, RPMh and GENI SPI engine like
# osfmk startup.c. The touch probe's 128 KiB command-DB copy comes from the heap (the native __DATA is small).
touch_text = (ROOT / 'kits/sheng/FuxiTouchProbe.cpp').read_text()
for old_text, new_text in (
        ('    static uint8_t db[kCmdDbSize];\n',
         '    static uint8_t *db;   // ios17: heap copy, see build-ios17-legacy-display.py\n'
         '    if (!db) db = static_cast<uint8_t *>(IOMallocAligned(kCmdDbSize, 8));\n'),
        ('    if (mapRegion(kCmdDbBase, kCmdDbSize, dbw)) {\n', '    if (db && mapRegion(kCmdDbBase, kCmdDbSize, dbw)) {\n'),
        ('        for (size_t i = 0; i < sizeof db; i += 4) {', '        for (size_t i = 0; i < kCmdDbSize; i += 4) {'),
        ('if (!qcom_cmd_db_find(db, sizeof db, FTB_VDD_RPMH_RESOURCE, &res))', 'if (!qcom_cmd_db_find(db, kCmdDbSize, FTB_VDD_RPMH_RESOURCE, &res))'),
        ('if (!qcom_cmd_db_find(db, sizeof db, FTB_AVDD_RPMH_RESOURCE, &res))', 'if (!qcom_cmd_db_find(db, kCmdDbSize, FTB_AVDD_RPMH_RESOURCE, &res))')):
    require(touch_text.count(old_text) == 1, 'FuxiTouchProbe anchor: ' + old_text.strip())
    touch_text = touch_text.replace(old_text, new_text)
touch_src = OUT / 'FuxiTouchProbe.ios17.cpp'
touch_text = touch_text.replace(
    'if ((++tick & 15) == 0) rows(*s);',
    'if ((++tick & 15) == 0) rows(*s);' +
    r' if ((tick & 127) == 0) {report(*s); printf("%s\n", hidText());}')
touch_text = touch_text.replace('if (!drain(*s)) break;',
    r'if (!drain(*s)) {printf("fuxi-touch FIFO loop stopped%s\n", hidText()); break;}')
touch_text = touch_text.replace(
    's.drained += count;',
    's.drained += count; if (s.drained <= 32) {for (unsigned i=0;i<count;++i) '
    r'printf("FTS RAW %02x %02x %02x %02x %02x %02x %02x %02x\n", '
    's.events[8*i],s.events[8*i+1],s.events[8*i+2],s.events[8*i+3],'
    's.events[8*i+4],s.events[8*i+5],s.events[8*i+6],s.events[8*i+7]);}')
touch_text = touch_text.replace(
    'native_touch_stage_done = 4;',
    'memset(s->tx,0,212); s->tx[0]=0xa7; '
    'int infoRc=busXfer(s,s->tx,s->rx,212); '
    'printf("FTS SYSINFO rc%d",infoRc); '
    r'for(unsigned i=4;i<92;++i)printf(" %02x",s->rx[i]); printf("\n"); '
    'native_touch_stage_done = 4;')
touch_text = touch_text.replace(
    'native_touch_stage_done = 4;',
    'unsigned fwEvents=0; PE_parse_boot_argn("fuxi-touch-fw-events",&fwEvents,sizeof fwEvents); '
    'if(fwEvents==1){s->tx[0]=0xa4;s->tx[1]=1;s->tx[2]=1;'
    'int fwRc=busXfer(s,s->tx,s->rx,3); IOSleep(20);'
    'printf("FTS firmware events enable rc%d%c",fwRc,10);}'
    'native_touch_stage_done = 4;')
touch_src.write_text(touch_text)
touch_includes = ['-I', str(ROOT / 'kits/sheng'), '-I', str(ROOT / 'drivers/FuxiFtsTouch'), '-I', str(ROOT / 'drivers/QcomGeniSpi')]
touch_cxx = []
for src in (touch_src, ROOT / 'kits/sheng/FuxiHidProbe.cpp', ROOT / 'kits/sheng/HidInputProbe.cpp',
            ROOT / 'kits/sheng/FuxiButtons.cpp'):   # fuxi buttons (fuxi-buttons=1), docs/zh/porting/input.md section 4
    obj = OUT / (src.name.split('.')[0] + '.o')
    subprocess.run([str(src) if a == str(probe_src) else str(obj) if a == str(probe_obj) else a for a in cmd] + touch_includes,
                   cwd=record['directory'], check=True)
    touch_cxx.append(obj)
startup_record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json')
                            .read_text().strip().rstrip(','))
touch_c = []
for src in (ROOT / 'drivers/FuxiFtsTouch/FtsProtocol.c', ROOT / 'drivers/FuxiFtsTouch/FtsTouchCore.c',
            ROOT / 'kits/sheng/QcomRpmh.c', ROOT / 'drivers/QcomGeniSpi/GeniSpi.c'):
    obj = OUT / (src.stem + '.o')
    sargs, scmd, k = startup_record['arguments'], [], 0
    while k < len(sargs):
        a = sargs[k]
        if a == startup_record['file']:
            scmd.append(str(src))
        elif a == '-o':
            scmd += ['-o', str(obj)]; k += 1
        elif a in ('-MF', '-MT', '-MJ'):
            k += 1
        elif a == '-Xclang' and sargs[k + 1].startswith('-W'):
            k += 1
        elif not a.startswith('-W'):
            scmd.append(a)
        k += 1
    subprocess.run(scmd + ['-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only', '-I', str(ROOT / '.cache/xnu-work/osfmk')]
                   + touch_includes, cwd=startup_record['directory'], check=True)
    touch_c.append(obj)
hid_dir = ROOT / 'build/native-hid-input-ios17'
hid_objs = [hid_dir / n for n in ('NativeHidInput.late.o', 'hid-original-bindings.o', 'hid-original-adapters.o', 'hid-ios17-stubs.o')]
for o in hid_objs:
    require(o.is_file(), f'{o} missing: build-ios17-hid-input.py runs before this script')

# ---- the panel brightness service (kits/sheng/BacklightProbe.cpp -> AppleARMBacklight.cpp) ----
# The iPadOS recipe had this and the 21H16 one did not: nothing compiled or archived the backlight sources and the late
# probe never called them, so backlight-control was never published. Compiled with the platform-expert command like the
# probe; AppleARMBacklight's static initializer is moved out of __mod_init_func because the prelinker does not run it at
# load time - BacklightProbe.cpp calls it explicitly via __GLOBAL__sub_I_AppleARMBacklight.cpp.
backlight_objs = []
for bl_stem, bl_rel in (('BacklightProbe', 'kits/sheng/BacklightProbe.cpp'),
                        ('AppleARMBacklight', 'kits/sheng/AppleARMBacklight.cpp'),
                        ('GeniI2c', 'kits/sheng/GeniI2c.c')):
    bl_obj = OUT / f'{bl_stem}.ios17.o'
    bl_src = ROOT / bl_rel
    require(bl_src.is_file(), f'{bl_rel} missing')
    bl_args, bl_cmd, bl_k = record['arguments'], [], 0
    while bl_k < len(bl_args):
        bl_a = bl_args[bl_k]
        if bl_a == record['file']:
            bl_cmd.append(str(bl_src))
        elif bl_a == '-o':
            bl_cmd += ['-o', str(bl_obj)]; bl_k += 1
        elif bl_a in ('-MF', '-MT', '-MJ'):
            bl_k += 1
        elif bl_a == '-Xclang' and bl_args[bl_k + 1].startswith('-W'):
            bl_k += 1
        elif not bl_a.startswith('-W'):
            bl_cmd.append(bl_a)
        bl_k += 1
    bl_cmd += ['-Wno-unknown-warning-option', '-I', str(ROOT / '.cache/xnu-work/osfmk')]
    subprocess.run(bl_cmd, cwd=record['directory'], check=True)
    if bl_stem == 'AppleARMBacklight':
        bl_late = OUT / f'{bl_stem}.late.o'
        run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
             '--globalize-symbol=__GLOBAL__sub_I_AppleARMBacklight.cpp', bl_obj, bl_late])
        bl_obj = bl_late
    backlight_objs.append(bl_obj)

# ---- archive ----
late = OUT / 'NativeLegacyDisplay.late.o'
run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp',
     OUT / 'NativeLegacyDisplay.o', late])
archive = OUT / 'libNativeLegacyDisplay.late.a'
archive.unlink(missing_ok=True)
run(['llvm-ar', 'rcs', archive, late, probe_obj, gpupower_obj, keystore_obj, keyevents_late, policy_obj, *touch_cxx, *touch_c, *hid_objs, *backlight_objs, OUT / 'legacy-original-bindings.o', OUT / 'legacy-original-adapters.o'])
sources = ['drivers/GopFramebuffer/NativeLegacyDisplay.cpp', 'scripts/data/Ios17LegacyDisplayProbe.cpp', 'kits/sheng/GpuPowerProbe.cpp',
           'kits/sheng/KeyStoreProbe.cpp', 'kits/sheng/BootWatchdogPolicy.c', 'kits/sheng/FuxiTouchProbe.cpp', 'kits/sheng/HidInputProbe.cpp', 'drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp',
           'kits/sheng/BacklightProbe.cpp', 'kits/sheng/AppleARMBacklight.cpp', 'kits/sheng/GeniI2c.c', 'scripts/build-ios17-legacy-display.py']
(OUT / 'build-report.json').write_text(json.dumps(dict(
    kernelcache_sha256=sha(KC), legacy_vtable=hex(LEG_VT), renamed_slots={str(i): s for i, s in sorted(suffix.items())},
    owned_slots=owned, bound=len(bound), archive_sha256=sha(archive),
    source_sha256={s: sha(ROOT / s) for s in sources}), indent=2) + '\n')
print(f'build-ios17-legacy-display: {len(suffix)} renamed slots, {len(owned)} owned, {len(bound)} adapters -> {archive}')



