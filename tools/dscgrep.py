"""dscgrep.py PATTERN [PATTERN...]: symbols of the 21H16 dyld shared cache (dscsym's cached table) whose name contains every pattern,
with the owning image."""
import sys, pickle, bisect
addrs, names, images = pickle.load(open('/home/xiaomi/ios17-bins-21H16/dscsym.pkl', 'rb'))
pats = sys.argv[1:]
iaddrs = [a for a, _ in images]
n = 0
for a, nm in zip(addrs, names):
    if all(p in nm for p in pats):
        j = bisect.bisect_right(iaddrs, a) - 1
        print(f'{a:#x} {images[j][1].rsplit("/", 1)[-1]:24s} {nm}')
        n += 1
        if n >= 120: break
print('matches', n)
