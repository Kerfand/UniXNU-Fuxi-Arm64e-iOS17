import struct as s,sys,pathlib,hashlib
from capstone import *
base=pathlib.Path('/home/xiaomi/sadnut-fuxi-local-20261103/UniXNU')
d=(base/'out-ios17/ipsw/21H16/kernelcache.decompressed').read_bytes()
def lc(o):
 p=o+32
 for _ in range(s.unpack_from('<I',d,o+16)[0]):
  c,n=s.unpack_from('<II',d,p);yield c,p;p+=n
o=next(s.unpack_from('<Q',d,p+16)[0] for c,p in lc(0) if c==0x80000035 and d[p+32:p+49].startswith(b'com.apple.kernel\0'))
segs=[s.unpack_from('<QQQQ',d,p+24) for c,p in lc(o) if c==25]
def rd(a,n):
 for v,size,f,z in segs:
  if v<=a<v+z:return d[f+a-v:f+a-v+n]
a=0xfffffff0081c0640
import json
p=base/'kits/XnuPrelink/fixtures/fileset-21H16-symbols.json'
j=json.loads(p.read_text());assert hashlib.sha256(d).hexdigest()==j['inputs']['kernelcache_sha256']
row=dict(original=hex(a),original_size=156,original_sha256=hashlib.sha256(rd(a,156)).hexdigest(),native_symbol='_proc_set_syscall_filter_index',evidence='Sandbox BL at 0xfffffff009e974ec, observed with which=2 and ascending index. Original checks KOBJ=2 else EINVAL, probes 1031 MIG entries of stride 24, stores index at +4 and returns 0 or ENOENT; matches kern_proc.c proc_set_syscall_filter_index and inline ipc_kobject_set_kobjidx. Next entry BTI at +156.')
assert not any(x['original']==hex(a) for x in j['functions']);j['functions'].append(row);p.write_text(json.dumps(j,indent=2,ensure_ascii=False)+'\n')