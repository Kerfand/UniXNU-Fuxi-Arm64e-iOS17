"""ppround.py LOG SLIDE PROC ROUND: every thread of PROC in ROUND, symbolized (skips idle workqueue threads)."""
import re, sys, subprocess
log, slide, proc, rnd = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
lines = open(log, errors='replace').read().splitlines()
inside = False; tid = None; out = []
for ln in lines:
    m = re.match(r'unixnu pp: round (\d+) (\S+) pid', ln)
    if m:
        inside = int(m.group(1)) == rnd and m.group(2) == proc; continue
    if not inside: continue
    m = re.match(r'unixnu pp: (t\d+) state (\S+) wait (\S+)', ln)
    if m:
        tid = m.group(1) + ' state ' + m.group(2); continue
    m = re.match(r'unixnu pp:   u pc (\S+) lr (.*)', ln)
    if m:
        frames = [m.group(1)] + m.group(2).split()
        if len(frames) <= 3: continue  # parked workqueue thread
        out.append((tid, frames))
for tid, frames in out:
    print('===', tid)
    addrs = ['0x' + a for a in frames if a != '0']
    r = subprocess.run(['python3', '-I', 'dscsym.py', slide] + addrs, capture_output=True, text=True,
                       cwd='/mnt/c/Users/Xiaomi/AppData/Local/Temp/claude/fa78473d8419f0a5/bb81b53f-24a8-4b58-bdef-56a9b385157b/scratchpad').stdout
    for l in r.splitlines()[:16]:
        print('   ', l[:160])
