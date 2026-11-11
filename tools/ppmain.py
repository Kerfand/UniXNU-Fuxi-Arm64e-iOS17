"""ppmain.py LOG SLIDE: for each sampled process, the main thread's (t0) user stack per round, deduplicated, symbolized with dscsym."""
import re, sys, subprocess
log, slide = sys.argv[1], sys.argv[2]
lines = open(log, errors='replace').read().splitlines()
cur = None; tid = None; stacks = {}
order = []
for ln in lines:
    m = re.match(r'unixnu pp: round (\d+) (\S+) pid (\d+)', ln)
    if m:
        cur = (int(m.group(1)), m.group(2)); continue
    m = re.match(r'unixnu pp: (t\d+) state (\S+) wait (\S+)', ln)
    if m:
        tid = (m.group(1), m.group(2), m.group(3)); continue
    m = re.match(r'unixnu pp:   u pc (\S+) lr (.*)', ln)
    if m and cur and tid and tid[0] == 't0':
        frames = [m.group(1)] + m.group(2).split()
        key = (cur[1], ' '.join(frames[:10]))
        if key not in stacks:
            stacks[key] = []
            order.append(key)
        stacks[key].append(cur[0])
for proc, fr in order:
    rounds = stacks[(proc, fr)]
    print(f'=== {proc} t0 rounds {rounds[0]}..{rounds[-1]} ({len(rounds)})')
    addrs = ['0x' + a for a in fr.split() if a != '0']
    out = subprocess.run(['python3', '-I', 'dscsym.py', slide] + addrs, capture_output=True, text=True,
                         cwd='/mnt/c/Users/Xiaomi/AppData/Local/Temp/claude/fa78473d8419f0a5/bb81b53f-24a8-4b58-bdef-56a9b385157b/scratchpad').stdout
    for l in out.splitlines()[:10]:
        print('   ', l[:150])
