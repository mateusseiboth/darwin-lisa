#!/usr/bin/env python3
"""The render pipelines CoreAnimation 26 linked on lisa, from HakiosMetal's logs, as cases for m2v-coverage.

  cases-from-logs.py OUT.tsv LOG...

Each `linked VERT+FRAG (PTR) for pixel format F ...; fragment constants not 0: I=V ...` line is one pipeline: its two
functions, colour attachment 0's format and the fragment function's non-zero constants (the others were set to 0).
A `trace: VERT+FRAG (PTR) vertex constants (n): I=V ...; c0 ...; c1 fmt F ... c2 fmt F ...` line of the same log and
process (metal_trace=1 runs) adds the vertex function's constants and attachments 1 and 2. Without one, the vertex
function gets the fragment's set (CoreAnimation hands both one MTLFunctionConstantValues) and attachments 1 and 2 are
left out. `fetches through a vertex descriptor with attribute 0 (format F, offset O, buffer B)` gives attribute 0.
Output, one pipeline a line, deduplicated:
  render VERT FRAG C0 C1 C2 VERTEX_CONSTANTS FRAGMENT_CONSTANTS ATTR0(format:offset:buffer|-) SEEN
"""
import re
import sys

LINKED = re.compile(r"^HakiosMetal\[(\S+) (\d+)\]: linked (\S+)\+(\S+) \((0x[0-9a-f]+)\) for pixel format (\d+).*?(?:constants not 0: (.*))?$")
TRACE = re.compile(r"^HakiosMetal\[(\S+) (\d+)\]: trace: (\S+)\+(\S+) \((0x[0-9a-f]+)\) vertex constants \(\d+\):([^;]*);.*c1 fmt (\d+).*c2 fmt (\d+)")
ATTR = re.compile(r"HakiosMetal(?:\[\S+ \d+\])?: (\S+)\+(\S+) fetches through a vertex descriptor with attribute 0 \(format (\d+), offset (\d+), buffer (\d+)\)")


def consts(text):
    pairs = sorted((int(i), int(v)) for i, v in re.findall(r"(\d+)=(\d+)", text or ""))
    return " ".join(f"{i}={v}" for i, v in pairs if v) or "-"


cases = {}
attrs = {}
for path in sys.argv[2:]:
    linked, traced = {}, {}
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").rstrip("\n")
            m = LINKED.match(line)
            if m:
                proc, pid, vert, frag, ptr, fmt, fc = m.groups()
                linked[(pid, ptr)] = (vert, frag, fmt, consts(fc))
                continue
            m = TRACE.match(line)
            if m:
                proc, pid, vert, frag, ptr, vc, c1, c2 = m.groups()
                traced[(pid, ptr)] = (consts(vc), c1, c2)
                continue
            m = ATTR.search(line)
            if m:
                attrs.setdefault((m.group(1), m.group(2)), f"{m.group(3)}:{m.group(4)}:{m.group(5)}")
    for key, (vert, frag, fmt, fc) in linked.items():
        vc, c1, c2 = traced.get(key, (None, "-", "-"))
        k = (vert, frag, fmt, fc)
        old = cases.get(k)
        if old is None or (old[0] is None and vc is not None):
            cases[k] = [vc, c1, c2, 0]
        cases[k][3] += 1
with open(sys.argv[1], "w") as out:
    for (vert, frag, fmt, fc), (vc, c1, c2, seen) in sorted(cases.items()):
        out.write("\t".join(["render", vert, frag, fmt, c1, c2, vc or fc, fc, attrs.get((vert, frag), "-"), str(seen)]) + "\n")
print(f"{len(cases)} pipelines, {sum(1 for c in cases.values() if c[0] is not None)} with a trace line")
