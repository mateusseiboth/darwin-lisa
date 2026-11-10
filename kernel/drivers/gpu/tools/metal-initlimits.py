#!/usr/bin/env python3
"""metal-initlimits.py METAL LISTING: run -[_MTLDevice initLimits] of an iOS Metal.framework under unicorn for each
featureProfile 0..15 and say which values pass its asserts, with the limit words it writes into the device object.

HakiosMetalCA.m answers -featureProfile, and MTLAddDevice calls initLimits on it: a value whose table is empty ends in
_MTLReleaseAssertionFailure inside Metal, which kills every Metal client. On iOS 26 (23F84): 0, 1 and 4 to 12 pass,
2, 3 and 13 and up assert (_limits.maxIndirect...PerDevice > 0). The plug-in's 1 is still good.

  METAL    the dylib out of the dyld cache:  ipsw dyld extract <cache> Metal --output DIR
  LISTING  initLimits disassembled, from its label to the next one:
           ipsw dyld disass <cache> -i Metal -d --no-color --force > metal.s, then cut from "initLimits:" to
           "initFeatureQueries:" (the calls in it are matched by address: featureProfile gets the value under test,
           maxBufferLength 1 GB, the rest 0; a call into Metal's assertion stubs counts as an assert)
Needs: pip install unicorn (in a venv).
"""
import re, struct, sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UcError
from unicorn.arm64_const import *

METAL, LISTING = sys.argv[1], sys.argv[2]
data = open(METAL, 'rb').read()
ncmds, = struct.unpack_from('<I', data, 16)
segs = []
at = 32
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', data, at)
    if cmd == 0x19:
        name = data[at+8:at+24].rstrip(b'\0').decode()
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, at+24)
        segs.append((name, vmaddr, vmsize, fileoff, filesize))
    at += size

lines = open(LISTING).read().splitlines()
calls = {}
for l in lines:
    m = re.match(r'(0x[0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*bl\s+(.*)', l)
    if m:
        calls[int(m.group(1), 16)] = m.group(2)
start = int(re.match(r'(0x[0-9a-f]+):', lines[1]).group(1), 16)
rets = [int(re.match(r'(0x[0-9a-f]+):', l).group(1), 16) for l in lines if re.search(r'\sretab$', l)]

SELF, STACK = 0x300000000, 0x310000000

def run(profile, answers):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for name, vmaddr, vmsize, fileoff, filesize in segs:
        if name == '__PAGEZERO' or vmsize == 0:
            continue
        base = vmaddr & ~0x3fff
        size = ((vmaddr + vmsize + 0x3fff) & ~0x3fff) - base
        try:
            uc.mem_map(base, size)
        except UcError:
            pass
        uc.mem_write(vmaddr, data[fileoff:fileoff+filesize])
    uc.mem_map(SELF, 0x10000)
    uc.mem_map(STACK - 0x100000, 0x100000)
    uc.reg_write(UC_ARM64_REG_SP, STACK - 0x1000)
    uc.reg_write(UC_ARM64_REG_X0, SELF)
    uc.reg_write(UC_ARM64_REG_LR, 0xdead0000)
    # FP/SIMD on
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)
    state = {'assert': None}

    def hook(uc, addr, size, _):
        if addr in calls:
            what = calls[addr]
            m = re.search(r'"([^"]+)"', what)
            if m:
                uc.reg_write(UC_ARM64_REG_X0, profile if m.group(1) == 'featureProfile' else answers.get(m.group(1), 0))
            elif what.startswith('sub_18628'):
                state['assert'] = what
                uc.emu_stop()
                return
            uc.reg_write(UC_ARM64_REG_PC, addr + 4)
        elif addr in rets:
            uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook)
    # pacibsp is a hint (a no-op here); retab is caught above
    try:
        uc.emu_start(start, 0xdead0000, count=200000)
    except UcError as e:
        if uc.reg_read(UC_ARM64_REG_PC) not in rets:
            return 'emulation error %s at %#x' % (e, uc.reg_read(UC_ARM64_REG_PC)), None
    return state['assert'], uc.mem_read(SELF, 0x200)

answers = {'maxBufferLength': 1 << 30, 'argumentBuffersSupport': 0, 'indirectArgumentBufferCapabilities': 0}
for p in range(0, 16):
    a, mem = run(p, answers)
    if a:
        print('featureProfile %2d: ASSERT %s' % (p, a))
    else:
        words = struct.unpack('<128I', bytes(mem))
        nz = ['%#x=%d' % (i*4, w) for i, w in enumerate(words) if w and i*4 >= 0x10][:40]
        print('featureProfile %2d: ok  %s' % (p, ' '.join(nz)))
