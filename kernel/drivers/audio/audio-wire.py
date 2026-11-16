#!/usr/bin/env python3
# darwin-lisa: the speakers (ADSP APR path + two TFA9873 amplifiers) as the optional tag `dlisa_sound`:
#   bsd/dev/dlisa.sound.c      sound=1..4: MI2S port, ASM/ADM/AFE, the test tone, /dev/hakios-audio's sink,
#                              kern.hakios.sound_status / sound_tone
#   bsd/dev/dlisa.tfa9873.c    the amplifiers over bit-banged I2C (GPIO 12/13)
#   bsd/dev/dlisa.tfa9873.h    (beside them: included as "dlisa.tfa9873.h", as are the two below)
#   bsd/dev/dlisa.tfa9873.flags.h   the status registers decoded, the fault rules
#   bsd/dev/dlisa.tfa9873.tuning.h  Xiaomi's container items (tfa-cnt.py), sound_tune=1|2
# Options under the tag: DLISA_SOUND, and HAKIOS_SOUND, which drops hakios.stubs.c's empty hakios_sound_dsp_up(),
# _dsp_down() and _quiesce() (dlisa.sound.c defines them; hakios.adsp.c already calls them at the right moments), so
# nothing in hakios.adsp.c or hakios.stubs.c is edited. No init call: the entry is hakios_sound_dsp_up(), called by
# hakios.adsp.c once APR is up (adsp=1). Inert without sound=N. Idempotent; XNU_WORK=<dir> for a dry run on a copy.
# Run in docker as root (as the other wires), after hakios-wire.py and adsp-wire.py, then the rebuild.
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import dlisa_wire as w  # noqa: E402

# what this needs and does not touch: the DSP driver (with its calls into the sound) and the stubs it replaces
w.require("config/MASTER", "options\t\tHAKIOS_ADSP", "the ADSP driver's option (adsp-wire.py / the base tree)")
w.require("bsd/dev/hakios.adsp.c", "hakios_sound_dsp_up();", "hakios.adsp.c calling the sound once APR is up")
w.require("bsd/dev/hakios.adsp.c", "hakios_sound_dsp_down();", "hakios.adsp.c stopping the sound before the DSP")
w.require("bsd/dev/hakios.stubs.c", "#ifndef HAKIOS_SOUND\n", "hakios.stubs.c's sound stubs under HAKIOS_SOUND")
w.require("bsd/dev/hakios.platform.c", "LCK_MTX_DECLARE(hakios_smmu_lock", "the shared SMMU lock")
w.require("bsd/sys/hakios.audio.h", "hakios_adsp_apr_events(hakios_apr_events_t handler);", "the APR interface")

w.copy(os.path.join(HERE, "dlisa.tfa9873.h"), "bsd/dev/dlisa.tfa9873.h")
w.copy(os.path.join(HERE, "dlisa.tfa9873.flags.h"), "bsd/dev/dlisa.tfa9873.flags.h")
w.copy(os.path.join(HERE, "dlisa.tfa9873.tuning.h"), "bsd/dev/dlisa.tfa9873.tuning.h")
w.copy(os.path.join(HERE, "dlisa.tfa9873.c"), "bsd/dev/dlisa.tfa9873.c")
w.copy(os.path.join(HERE, "dlisa.sound.c"), "bsd/dev/dlisa.sound.c")
w.append_once("bsd/conf/files", "bsd/dev/dlisa.sound.c",
              "\n# darwin-lisa speakers: APR (ASM/ADM/AFE PRIMARY_MI2S_RX) + TFA9873 over I2C by hand (optional; sound=1..4)\n"
              "bsd/dev/dlisa.tfa9873.c\t\toptional dlisa_sound\n"
              "bsd/dev/dlisa.sound.c\t\toptional dlisa_sound\n", "bsd/conf/files")
w.add_option("DLISA_SOUND", "speakers: ADSP APR path + TFA9873 (sound=1..4, needs adsp=1)", "dlisa_sound")
w.add_option("HAKIOS_SOUND", "hakios.stubs.c: hakios_sound_* come from dlisa.sound.c", "dlisa_sound")
w.add_bsd_dev_tag("dlisa_sound")
print("### AUDIO WIRE DONE")
