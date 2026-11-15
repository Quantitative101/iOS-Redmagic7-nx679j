import re, sys
for p in sys.argv[1:]:
    s = open(p, 'rb').read().decode('latin1')
    recs = re.split(r'(?=HakiosMetal\[)', s)
    cur = None; blocks = []
    for r in recs:
        m = re.match(r'HakiosMetal\[(\w+) (\d+)\]: fps: ([0-9.]+) frames/s over ([0-9.]+) s \((\d+) presented, (\d+) command buffers\); GPU busy (\d+) ms \((\d+)%\), -commit on the caller\'s thread (\d+) ms, commit to completion ([0-9.]+) ms on average, ([0-9.]+) at most', r)
        if m:
            cur = dict(name=m.group(1), span=float(m.group(4)), cbs=int(m.group(6)), commit=float(m.group(9)), lat=float(m.group(10)), latmax=float(m.group(11)))
            m2 = re.search(r'a buffer holds ([0-9.]+) passes', r)
            if m2: cur['passes'] = float(m2.group(1))
            m2 = re.search(r'idle\), ([0-9.]+) draws', r)
            if m2: cur['draws'] = float(m2.group(1))
            blocks.append(cur)
        elif cur is not None:
            m = re.match(r'HakiosMetal\[\w+ \d+\]: fps: \.\.\. gaps between commits while something moves: (\d+) on time \(20 ms at most\), (\d+) a frame late \(to 50 ms\), (\d+) stalls \(past 50\); a buffer made to committed ([0-9.]+) ms on average, ([0-9.]+) at most', r)
            if m:
                cur.update(on=int(m.group(1)), late=int(m.group(2)), stall=int(m.group(3)), enc=float(m.group(4)), encmax=float(m.group(5)))
            m = re.match(r'HakiosMetal\[\w+ \d+\]: fps: \.\.\. draw calls by processor:(.*?) \(TPIDR', r)
            if m:
                n = 0; us = 0
                for c, k, u in re.findall(r'cpu(\d) (\d+) at (\d+) us', m.group(1)):
                    n += int(k); us += int(k) * int(u)
                cur.update(dc=n, dcus=us)
            m = re.match(r'HakiosMetal\[\w+ \d+\]: fps: \.\.\. passes timed by the GPU: (\d+) ms in all', r)
            if m: cur['gpums'] = int(m.group(1))
    bb = [b for b in blocks if b['name'] == 'backboardd' and b['cbs'] / b['span'] >= 30 and 'on' in b]
    if not bb: print(p, 'no busy blocks'); continue
    T = lambda k: sum(b.get(k, 0) for b in bb)
    cbs = T('cbs'); span = T('span')
    print('%s: %d busy periods, %.0f s, %d buffers (%.1f/s)' % (p.split('/')[-1], len(bb), span, cbs, cbs / span))
    print('  gaps: on time %d, late %d (%.1f%%), stalls %d (%.1f%%)' % (T('on'), T('late'), 100.0 * T('late') / max(1, T('on') + T('late') + T('stall')), T('stall'), 100.0 * T('stall') / max(1, T('on') + T('late') + T('stall'))))
    print('  encode avg %.2f ms (worst max %.1f), commit call %.2f ms/buffer, commit->completion avg %.1f ms' % (sum(b['enc'] * b['cbs'] for b in bb) / cbs, max(b['encmax'] for b in bb), T('commit') / cbs, sum(b['lat'] * b['cbs'] for b in bb) / cbs))
    print('  draw calls %.1f per buffer at %.1f us = %.2f ms/buffer; GPU passes %.2f ms/buffer' % (T('dc') / cbs, T('dcus') / max(1, T('dc')), T('dcus') / 1e3 / cbs, T('gpums') / cbs))
    pb = [b for b in bb if 'passes' in b]
    if pb: print('  passes per buffer %.1f (from %d periods)' % (sum(b['passes'] * b['cbs'] for b in pb) / sum(b['cbs'] for b in pb), len(pb)))
    # kernel
    k = re.findall(r'MONDRIAN FB: fps (\d+) copy avg (\d+) us \(GPU wait (\d+), flush (\d+), scale (\d+), copy-back \d+\) max (\d+) us late avg (\d+) us max (\d+) us', s)
    k = [tuple(map(int, x)) for x in k if int(x[0]) >= 30]
    if k:
        n = sum(x[0] for x in k)
        print('  kernel: %d s at >=30 fps, mean %.1f fps; per swap: GPU wait %.2f ms, flush %.2f, scale %.2f; swap starts %.1f ms after vsync' % (len(k), n / len(k), sum(x[0]*x[2] for x in k)/n/1e3, sum(x[0]*x[3] for x in k)/n/1e3, sum(x[0]*x[4] for x in k)/n/1e3, sum(x[0]*x[6] for x in k)/n/1e3))
        h = {}
        for x in k: h[x[0] // 5 * 5] = h.get(x[0] // 5 * 5, 0) + 1
        print('  fps histogram (seconds):', ' '.join('%d-%d:%d' % (a, a + 4, h[a]) for a in sorted(h)))
