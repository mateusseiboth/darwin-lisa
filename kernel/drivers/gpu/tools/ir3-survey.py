# darwin-lisa session 13: SWEEP=path to a sweep built with sweep-listing-analysis.patch, LIB=QuartzCore AIR slice (bytes 48..1551232)
# usage: SWEEP=... LIB=qc.metallib OUT=listing-dir python3 -I ir3-survey.py OUT.json LOG...
# survey: every (fragment function, constants) linked in the given logs, compiled by sweep, stats per variant
import re, subprocess, sys, os, collections, json
S = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SWEEP = os.environ["SWEEP"]
LIB = os.environ.get("LIB", S + "/qc.metallib")
pat = re.compile(r'HakiosMetal\[(\S+) \d+\]: linked (\S+)\+(\S+) .*?fragment constants not 0:(.*)$')
seen = collections.Counter(); procs = collections.defaultdict(set)
for log in sys.argv[2:]:
    for line in open(log, errors='replace'):
        m = pat.search(line)
        if not m: continue
        proc, vs, fs, consts = m.groups()
        consts = tuple(c for c in consts.split() if '=' in c)
        seen[(fs, consts)] += 1; procs[(fs, consts)].add(proc)
out = {}
for (fs, consts), n in seen.most_common():
    lst = os.environ.get("TMPDIR", "/tmp") + "/ir3-survey-listing.txt"
    if os.path.exists(lst): os.remove(lst)
    args = [SWEEP, "-f", fs] + sum([["-c", c] for c in consts], []) + [LIB]
    r = subprocess.run(args, env=dict(os.environ, SWEEP_LISTING=lst), capture_output=True, text=True)
    st = {}
    if os.path.exists(lst):
        txt = open(lst).read()
        m = re.search(r'; STATS (.*)', txt)
        if m: st = dict(kv.split('=') for kv in m.group(1).split())
        st['sam16'] = len(re.findall(r'\bsam\w* \(f16\)', txt)); st['sam32'] = len(re.findall(r'\bsam\w* \(f32\)', txt))
        st['isam'] = len(re.findall(r'\bisam\w*', txt)); st['bary'] = len(re.findall(r'bary\.f', txt))
        st['cov'] = len(re.findall(r'\bcov\.', txt))
        key = fs + " " + " ".join(consts)
        open(os.environ.get("OUT", ".") + "/" + re.sub(r'[^A-Za-z0-9_=.-]', '_', key)[:180] + ".txt", "w").write(txt)
    else:
        st['err'] = (r.stdout + r.stderr)[-200:]
    out[fs + " " + " ".join(consts)] = dict(st, n=n, procs=sorted(procs[(fs, consts)]))
json.dump(out, open(sys.argv[1], "w"), indent=1)
for k, v in out.items():
    print(f"{v.get('waves','?'):>3}w r{v.get('max_reg','?'):>3} h{v.get('max_half','?'):>3} i{v.get('instrs','?'):>4} pf{v.get('prefetch','?')} s16={v.get('sam16')} s32={v.get('sam32')} isam={v.get('isam')} cov={v.get('cov')} pvt={v.get('pvtmem')} x{v['n']} {','.join(v['procs'])[:30]:30} {k[:110]}")
