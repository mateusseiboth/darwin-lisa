#!/usr/bin/env python3
"""The llvm.* integer and float intrinsics of air-y-llvm-intrinsics.patch, folded by `air2nir --eval` and compared
with LLVM's definitions written here in Python (darwin-lisa session 13).

Each case is a fragment shader (the skeleton of HakiOS's tools/air/intrincheck.py) that calls one intrinsic on
constant operands and returns the result in its uint4 render target; llvm-as assembles it, air2nir folds it.

usage: AIR2NIR=.../air2nir python3 -I air-llvm-check.py      (prints one line a case, exits 1 on a mismatch)
"""
import os, struct, subprocess, sys, tempfile

AIR2NIR = os.environ.get("AIR2NIR", os.path.expanduser("~/Dev/mobile/research/hackios-xnu-kernel/build/mesa-host/build/src/compiler/air/air2nir"))

SKELETON = """source_filename = "lc"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%(decl)s

define <{ <4 x i32> }> @lc() local_unnamed_addr {
  %%x = call %(ty)s @%(name)s(%(args)s)
%(widen)s
  %%v0 = insertelement <4 x i32> undef, i32 %%w, i32 0
  %%v1 = insertelement <4 x i32> %%v0, i32 0, i32 1
  %%v2 = insertelement <4 x i32> %%v1, i32 0, i32 2
  %%v3 = insertelement <4 x i32> %%v2, i32 0, i32 3
  %%r = insertvalue <{ <4 x i32> }> undef, <4 x i32> %%v3, 0
  ret <{ <4 x i32> }> %%r
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @lc, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"uint4", !"air.arg_name", !"out"}
!6 = !{}
"""

def m(bits): return (1 << bits) - 1
def s(v, bits): v &= m(bits); return v - (1 << bits) if v >> (bits - 1) else v
def fbits(x): return struct.unpack("<I", struct.pack("<f", x))[0]

def ref(op, bits, a):
    u = [x & m(bits) for x in a]
    if op == "smax": return max(s(a[0], bits), s(a[1], bits)) & m(bits)
    if op == "smin": return min(s(a[0], bits), s(a[1], bits)) & m(bits)
    if op == "umax": return max(u[0], u[1])
    if op == "umin": return min(u[0], u[1])
    if op == "abs": return abs(s(a[0], bits)) & m(bits)
    if op == "ctlz": return bits - u[0].bit_length()
    if op == "cttz": return bits if u[0] == 0 else (u[0] & -u[0]).bit_length() - 1
    if op == "ctpop": return bin(u[0]).count("1")
    if op == "bswap": return int.from_bytes(u[0].to_bytes(bits // 8, "little"), "big")
    if op == "bitreverse": return int(format(u[0], "0%db" % bits)[::-1], 2)
    if op in ("fshl", "fshr"):
        c = u[2] % bits; cat = (u[0] << bits) | u[1]
        return ((cat << c) >> bits) & m(bits) if op == "fshl" else (cat >> c) & m(bits)
    if op == "usub.sat": return max(u[0] - u[1], 0)
    if op == "uadd.sat": return min(u[0] + u[1], m(bits))
    if op == "ssub.sat": return max(min(s(a[0], bits) - s(a[1], bits), (1 << (bits - 1)) - 1), -(1 << (bits - 1))) & m(bits)
    if op == "sadd.sat": return max(min(s(a[0], bits) + s(a[1], bits), (1 << (bits - 1)) - 1), -(1 << (bits - 1))) & m(bits)
    raise ValueError(op)

INT = [  # (op, bits, operands)
    ("umax", 32, [3, 0xfffffff0]), ("umax", 16, [0x8001, 7]), ("umax", 8, [200, 100]),
    ("umin", 32, [3, 0xfffffff0]), ("smax", 32, [0xfffffff0, 3]), ("smax", 16, [0xfff0, 3]),
    ("smin", 32, [0xfffffff0, 3]), ("smin", 64, [0xfffffffffffffff0, 3]),
    ("abs", 32, [0xfffffff0, 0]), ("abs", 16, [0x8005, 0]),
    ("ctlz", 32, [0x00f00000, 0]), ("ctlz", 32, [0, 0]), ("ctlz", 16, [0x0010, 0]), ("ctlz", 8, [1, 0]),
    ("cttz", 32, [0x00f00000, 0]), ("cttz", 32, [0, 0]), ("cttz", 16, [0x8000, 0]),
    ("ctpop", 32, [0xf0f0f00f, 0]), ("ctpop", 16, [0xffff, 0]),
    ("bswap", 32, [0x11223344, 0]), ("bitreverse", 32, [0x00000003, 0]),
    ("fshl", 32, [0x12345678, 0x9abcdef0, 8]), ("fshl", 32, [0x12345678, 0x9abcdef0, 32]),
    ("fshl", 16, [0x1234, 0x5678, 4]), ("fshl", 8, [0x81, 0x40, 1]), ("fshr", 32, [0x12345678, 0x9abcdef0, 8]),
    ("usub.sat", 32, [3, 5]), ("uadd.sat", 32, [0xfffffff0, 0x20]),
]
FLOAT = [  # (op, operands, expected)
    ("minnum", [1.5, -2.0], -2.0), ("maxnum", [1.5, -2.0], 1.5), ("copysign", [1.5, -3.0], -1.5),
    ("copysign", [-1.5, 3.0], 1.5), ("exp2", [3.0], 8.0), ("log2", [8.0], 3.0), ("sqrt", [16.0], 4.0),
    ("floor", [-1.5], -2.0), ("ceil", [-1.5], -1.0), ("trunc", [-1.5], -1.0), ("rint", [2.5], 2.0),
    ("fma", [2.0, 3.0, 1.0], 7.0),
]

def nargs(op): return 3 if op in ("fshl", "fshr") else 1 if op in ("abs", "ctlz", "cttz", "ctpop", "bswap", "bitreverse") else 2

def run(name, ty, argtys, args, widen):
    decl = "declare %s @%s(%s)" % (ty, name, ", ".join(argtys))
    src = SKELETON % dict(decl=decl, ty=ty, name=name, args=", ".join("%s %s" % (t, v) for t, v in zip(argtys, args)), widen=widen)
    with tempfile.TemporaryDirectory() as tmp:
        ll, bc = os.path.join(tmp, "lc.ll"), os.path.join(tmp, "lc.bc")
        open(ll, "w").write(src)
        q = subprocess.run(["llvm-as", "-o", bc, ll], capture_output=True, text=True)
        if q.returncode: return None, q.stderr.strip()
        q = subprocess.run([AIR2NIR, "--eval", bc], capture_output=True, text=True)
        for line in q.stdout.splitlines():
            if line.strip().startswith("out "):
                v = line.split()[1]
                return (None, line.strip()) if "fold" in line else (int(v) & 0xffffffff, None)
        return None, (q.stderr.strip().splitlines() or ["no output"])[-1]

bad = 0
for op, bits, ops in INT:
    ty = "i%d" % bits
    k = nargs(op)
    argtys, args = [ty] * k, [str(s(x, bits)) for x in ops[:k]]
    if op in ("abs", "ctlz", "cttz"): argtys.append("i1"); args.append("false")
    widen = "  %w = zext " + ty + " %x to i32" if bits < 32 else "  %w = trunc i64 %x to i32" if bits == 64 else "  %w = add i32 %x, 0"
    got, why = run("llvm.%s.%s" % (op, ty), ty, argtys, args, widen)
    want = ref(op, bits, ops) & 0xffffffff
    ok = got == want
    bad += not ok
    print("%-4s llvm.%s.%s(%s) = %s, LLVM says %#x%s" % ("ok" if ok else "FAIL", op, ty, ", ".join(hex(x) for x in ops[:k]),
          hex(got) if got is not None else "?", want, "" if ok else " (" + str(why) + ")"))
for op, ops, want in FLOAT:
    widen = "  %w = bitcast float %x to i32"
    got, why = run("llvm.%s.f32" % op, "float", ["float"] * len(ops), ["%e" % x for x in ops], widen)
    ok = got == fbits(want)
    bad += not ok
    print("%-4s llvm.%s.f32(%s) = %s, LLVM says %s%s" % ("ok" if ok else "FAIL", op, ", ".join(str(x) for x in ops),
          struct.unpack("<f", struct.pack("<I", got))[0] if got is not None else "?", want, "" if ok else " (" + str(why) + ")"))
print("air-llvm-check: %d of %d cases agree" % (len(INT) + len(FLOAT) - bad, len(INT) + len(FLOAT)))
sys.exit(1 if bad else 0)
