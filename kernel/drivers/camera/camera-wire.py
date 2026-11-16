#!/usr/bin/env python3
# darwin-lisa: the cameras as the optional tag `hakios_camera` (option HAKIOS_CAMERA). Files:
#   bsd/dev/hakios.camera.c        HakiOS's camera driver with lisa's sensors (cam=N; inert without it)
#   bsd/dev/hakios.camera_snap.h   what is logged of a frame, the auto-exposure step (camera_snap.h, host-tested)
#   bsd/dev/hakios.camera_tab.h    the sensors' tables, made here by camtab.py from pmOS's Linux sensor drivers
#                                  (GPL-2.0 data: made at wire time, never kept in git)
#   bsd/sys/hakios.camera.h        /dev/hakios-camera's interface (cam=5), HakiOS's
# hakios_camera_init() is already called from hakios.audio.c, and hakios.stubs.c's no-ops step aside under
# HAKIOS_CAMERA. No other file of the tree is edited. Idempotent; XNU_WORK=<dir> for a dry run on a copy. Run in docker
# as root (or as the owner of the tree), then the rebuild.
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dlisa_wire as w  # noqa: E402

w.require("bsd/dev/hakios.platform.c", "hakios_smmu_lock", "the shared SMMU lock")
w.require("bsd/dev/hakios.platform.c", "hakios_spmi_lock(void)", "the shared SPMI lock")
w.require("bsd/dev/hakios.image.c", "hakios_image_file(const char *dir", "the small image's files")
w.require("bsd/dev/hakios.audio.c", "hakios_camera_init();", "the camera's start in hakios.audio.c")
w.require("bsd/dev/hakios.stubs.c", "#ifndef HAKIOS_CAMERA", "the camera's no-op stubs")
w.require("osfmk/arm64/loader_console.c", "uint64_t loader_recording[3];", "the loader's recording slot")

tab = w.path("bsd/dev/hakios.camera_tab.h")
made = tab + ".new"
subprocess.run([sys.executable, "-I", os.path.join(HERE, "camtab.py"), "header", made], check=True)
if os.path.exists(tab) and open(tab, "rb").read() == open(made, "rb").read():
    os.remove(made)
    print("  [skip] hakios.camera_tab.h: same")
else:
    os.replace(made, tab)
    print("  [ok]   bsd/dev/hakios.camera_tab.h made by camtab.py")

w.copy(os.path.join(HERE, "hakios.camera.c"), "bsd/dev/hakios.camera.c")
w.copy(os.path.join(HERE, "camera_snap.h"), "bsd/dev/hakios.camera_snap.h")
w.copy(os.path.join(HERE, "hakios.camera.h"), "bsd/sys/hakios.camera.h")
w.append_once("bsd/conf/files", "bsd/dev/hakios.camera.c",
              "\n# darwin-lisa cameras: HakiOS's driver with lisa's sensors (optional; cam=N)\n"
              "bsd/dev/hakios.camera.c\t\toptional hakios_camera\n", "bsd/conf/files")
w.add_option("HAKIOS_CAMERA", "cameras: sensors on the CCI, CSIPHY, CSID-lite, IFE-lite RDI (cam=N)", "hakios_camera")
w.add_bsd_dev_tag("hakios_camera")
print("### CAMERA WIRE DONE")
