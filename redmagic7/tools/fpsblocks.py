import re, sys
p = sys.argv[1]
minrate = float(sys.argv[2]) if len(sys.argv) > 2 else 30
s = open(p, 'rb').read().decode('latin1')
# split into records at 'HakiosMetal['
recs = re.split(r'(?=HakiosMetal\[)', s)
out = []
cur = None
for r in recs:
    m = re.match(r'HakiosMetal\[(\w+) (\d+)\]: fps: ([0-9.]+) frames/s over ([0-9.]+) s \((\d+) presented, (\d+) command buffers\)', r)
    if m:
        rate = int(m.group(6)) / max(float(m.group(4)), 0.01)
        cur = (m.group(1), rate, [r.split('\n')[0].split('|')[0]])
        out.append(cur)
    elif cur is not None and re.match(r'HakiosMetal\[\w+ \d+\]: fps: \.\.\.', r):
        cur[2].append(r.split('\n')[0].split('|')[0])
    else:
        pass
n = 0
for name, rate, lines in out:
    if rate >= minrate:
        n += 1
        if n > int(sys.argv[3]) if len(sys.argv) > 3 else 6: break
        print('=== %s %.1f cb/s' % (name, rate))
        for l in lines: print(l[:1800])
