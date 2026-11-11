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
for i in Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN).disasm(rd(a,280),a):print(hex(i.address),i.mnemonic,i.op_str)