#!/usr/bin/env python3
"""Averages the last N perf reports of a dev log (default 10; one per 60 displayed frames).
   tools/ps3.sh log | tools/perfsum.py [reports]"""
import re
import sys
from collections import defaultdict

n = int(sys.argv[1]) if len(sys.argv) > 1 else 10
fps, players = [], defaultdict(lambda: defaultdict(list))
for line in sys.stdin:
    if m := re.match(r"frame: ([\d.]+) fps", line):
        fps.append(float(m[1]))
    elif m := re.match(r"perf P(\d) .*: (\d+) frames, avg ([\d.]+) ms \(renderer ([\d.]+)\), max ([\d.]+)", line):
        for k, v in zip(("frames", "ms", "render", "max"), m.groups()[1:]):
            players[m[1]][k].append(float(v))
    elif m := re.match(r"link P(\d): (\d+) sleeps, ([\d.]+) ms blocked", line):
        players[m[1]]["sleeps"].append(float(m[2]))
        players[m[1]]["blocked"].append(float(m[3]))

avg = lambda xs: sum(xs[-n:]) / len(xs[-n:]) if xs else float("nan")
if fps:
    print(f"fps {avg(fps):.1f} (min {min(fps[-n:]):.1f}, max {max(fps[-n:]):.1f}, last {len(fps[-n:])} reports)")
for p, d in sorted(players.items()):
    print(f"P{p}: {avg(d['frames']):.0f}/60 frames, emu {avg(d['ms']):.2f} ms (renderer {avg(d['render']):.2f}, "
          f"max {avg(d['max']):.1f}), link {avg(d['sleeps']):.0f} sleeps, {avg(d['blocked']):.2f} ms blocked")
