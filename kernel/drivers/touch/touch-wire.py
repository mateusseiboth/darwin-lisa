#!/usr/bin/env python3
# darwin-lisa: the touch test hook (dlisa.touch_synth.c, boot-arg touch_synth=SECS: a synthetic swipe up through
# /dev/hakios-touch) as a `standard` file, started at the end of hakios_input_init(). Inert without the boot-arg.
# The Goodix driver itself is wired by ../goodix-wire.py (tag hakios_goodix). Idempotent; run in docker as root,
# then .work/run-rebuild.sh.
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
T = "/home/mateusseiboth/Dev/mobile/research/UniXNU/out/lisa26/ipados-26.5.2-23F84/lisa26/tree/.cache/xnu-work"


def edit(path, old, new, tag):
    s = open(path).read()
    if new in s:
        print(f"  [skip] {tag}")
        return
    if s.count(old) != 1:
        print(f"  [FAIL] {tag}: anchor found {s.count(old)} times")
        sys.exit(2)
    open(path, "w").write(s.replace(old, new))
    print(f"  [ok]   {tag}")


shutil.copyfile(os.path.join(HERE, "dlisa.touch_synth.c"), os.path.join(T, "bsd/dev/dlisa.touch_synth.c"))
print("  [ok]   copied dlisa.touch_synth.c")
bf = os.path.join(T, "bsd/conf/files")
if "bsd/dev/dlisa.touch_synth.c" not in open(bf).read():
    open(bf, "a").write("\n# darwin-lisa touch test hook (touch_synth=SECS)\nbsd/dev/dlisa.touch_synth.c\t\tstandard\n")
    print("  [ok]   bsd/conf/files")
edit(os.path.join(T, "bsd/dev/hakios.input.c"),
     "\thakios_mpss_init();\n}",
     "\thakios_mpss_init();\n\t{\n\t\textern void dlisa_touch_synth_init(void);\n\n\t\tdlisa_touch_synth_init();\n\t}\n}",
     "hakios.input.c calls dlisa_touch_synth_init()")
print("### TOUCH WIRE DONE")
