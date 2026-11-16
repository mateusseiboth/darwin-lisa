#!/usr/bin/env python3
"""Read an NXP TFA98xx container (tfa98xx.cnt) and show or extract what it writes to the amplifiers.

The format is NXP's (tfa98xx_parameters.h of Xiaomi's lisa-r-oss tfa98xx driver): a container header, an index of
device lists, each device list naming bitfields, register patches, a patch file and profile lists; each profile list
naming bitfields (until a "default" marker), DSP message files and the like.

  tfa-cnt.py dump FILE.cnt [FIELDNAMES.h]     every list, every item, bitfields decoded (names from the NXP header)
  tfa-cnt.py header FILE.cnt PROFILE FIELDNAMES.h > dlisa.tfa9873.tuning.h
                                               the device's and the profile's register writes a DSP-free TFA9873 on
                                               pmOS's bus takes (LEFT_OUT: the stock bus format, rate, clock, gain)

Only the standard library; the container is data and is only parsed.
"""
import re
import struct
import sys

DSC = ["device", "profile", "register", "string", "file", "patch", "marker", "mode", "inputsel", "outputsel",
       "programcfg", "lagw", "gains", "vbatfactors", "sensescal", "sensesdelay", "bitfield", "default", "livedata",
       "livedatastring", "group", "cmd", "mbdrc", "filter", "noinit", "features", "cfmem", "fwkusecase", "vddp"]
DSC_BITFIELD, DSC_DEFAULT, DSC_REGISTER, DSC_PROFILE, DSC_STRING, DSC_FILE, DSC_PATCH = 16, 17, 2, 1, 3, 4, 5


def desc(word):
    return word & 0xFFFFFF, word >> 24


class Container:
    def __init__(self, data):
        self.data = data
        if data[0:2] != b"PM":
            raise ValueError("not a TFA container (id %r)" % data[0:2])
        self.size, self.crc, self.rev = struct.unpack_from("<IIH", data, 6)
        self.customer = data[16:24].split(b"\0")[0].decode()
        self.application = data[24:32].split(b"\0")[0].decode()
        self.type = data[32:40].split(b"\0")[0].decode()
        self.ndev, self.nprof, self.nlive = struct.unpack_from("<HHH", data, 40)
        self.devices = []
        for i in range(self.ndev):
            offset, kind = desc(struct.unpack_from("<I", data, 46 + 4 * i)[0])
            if kind != 0:
                raise ValueError("index entry %d is %s, not a device" % (i, DSC[kind]))
            self.devices.append(self.device(offset))

    def string(self, word):
        offset, kind = desc(word)
        if kind != DSC_STRING:
            return "<%s>" % DSC[kind]
        return self.data[offset:self.data.index(b"\0", offset)].decode()

    def items(self, start, count):
        return [desc(w) for w in struct.unpack_from("<%dI" % count, self.data, start)]

    def device(self, offset):
        length, bus, dev, func, devid, name = struct.unpack_from("<BBBBII", self.data, offset)
        items = self.items(offset + 12, length)
        return {"bus": bus, "address": dev, "func": func, "devid": devid, "name": self.string(name), "items": items,
                "profiles": [self.profile(o) for o, k in items if k == DSC_PROFILE]}

    def profile(self, offset):
        head, name = struct.unpack_from("<II", self.data, offset)
        length, group, ident = head & 0xFF, (head >> 8) & 0xFF, head >> 16
        return {"name": self.string(name), "group": group, "id": ident, "items": self.items(offset + 8, length - 1)}

    def bitfield(self, offset):
        value, field = struct.unpack_from("<HH", self.data, offset)
        return value, field

    def register(self, offset):
        address, value, mask = struct.unpack_from("<BHH", self.data, offset)
        return address, value, mask

    def file_name(self, offset):
        return self.string(struct.unpack_from("<I", self.data, offset)[0])


def field_parts(field):
    return field >> 8, (field >> 4) & 15, (field & 15) + 1


def load_names(path):
    names = {}
    if not path:
        return names
    with open(path, encoding="latin-1") as f:
        text = f.read()
    for match in re.finditer(r'\{\s*0x([0-9a-fA-F]+),\s*"([A-Za-z0-9_]+)"\}', text):
        names.setdefault(int(match.group(1), 16), match.group(2))
    return names


def describe(cnt, offset, kind, names):
    if kind == DSC_BITFIELD:
        value, field = cnt.bitfield(offset)
        address, pos, length = field_parts(field)
        return "bitfield %-12s reg 0x%02x bits %d..%d = 0x%x" % (names.get(field, "?%04x" % field), address, pos,
                                                                 pos + length - 1, value)
    if kind == DSC_REGISTER:
        return "register 0x%02x = 0x%04x mask 0x%04x" % cnt.register(offset)
    if kind in (DSC_FILE, DSC_PATCH):
        return "%s %s" % (DSC[kind], cnt.file_name(offset))
    if kind == DSC_STRING:
        return "string %r" % cnt.string(offset | DSC_STRING << 24)
    return DSC[kind] if kind < len(DSC) else "type %d" % kind


def dump(cnt, names):
    print("container %s %s %s, rev %d, %d devices, %d profiles" % (cnt.customer, cnt.application, cnt.type, cnt.rev,
                                                                    cnt.ndev, cnt.nprof))
    for dev in cnt.devices:
        print("device %s: bus %d address 0x%02x devid 0x%08x" % (dev["name"], dev["bus"], dev["address"], dev["devid"]))
        for offset, kind in dev["items"]:
            if kind != DSC_PROFILE:
                print("  " + describe(cnt, offset, kind, names))
        for prof in dev["profiles"]:
            print("  profile %r group %d id 0x%x" % (prof["name"], prof["group"], prof["id"]))
            for offset, kind in prof["items"]:
                print("    " + describe(cnt, offset, kind, names))


class Write:
    """one read-modify-write of a register: the field's bits (mask) set to value (already shifted)"""

    def __init__(self, register, mask, value, name):
        self.register, self.mask, self.value, self.name = register, mask, value, name


# What of Xiaomi's tuning a TFA9873 on pmOS's bus does not take (tfaContWriteRegsDev/Prof would write it on Android):
# the stock bus runs 4 slots of 32 bits at 96 kHz and returns current and voltage sense to the ADSP's speaker
# protection; here the ADSP plays 2 slots of 16 bits at 48 kHz with nothing coming back (dlisa.sound.c, pmOS's way).
LEFT_OUT = {
    "AUDFS": "sample rate: 96 kHz on Android, 48 kHz here (the POR value 8)",
    "FRACTDEL": "V/I fractional delay: only for sense data, which is not returned here",
    "INPLEV": "TDM output attenuation: of the sense data",
    "REFCKEXT": "PLL reference: chosen for the stock 12.288 MHz bit clock",
    "mtp_pll_lut_sel": "PLL lookup table: chosen for the stock clocking",
    "AMPINSEL": "amplifier input selection: pmOS leaves the POR value and plays",
    "TDMSPKG": "gain: only with sound_tune=2 (DLISA_TFA_TUNING_SPKG_xx)",
    "DCA": "SYS_CTRL0's DC-DC bit: dlisa.tfa9873.c owns SYS_CTRL0 (sound_dcdc, default 1, as Xiaomi's profile)",
}
LEFT_OUT_PREFIXES = ("TDM",)    # every slot, width, source and sense-routing field of the stock bus


def is_left_out(name):
    return name in LEFT_OUT or name.startswith(LEFT_OUT_PREFIXES)


def items_written(cnt, items, profile):
    """the bitfields and registers a DSP-free device writes from a list: a device list up to its patch, file or first
    profile (tfaContWriteRegsDev), a profile up to its default marker (tfaContWriteRegsProf)"""
    for offset, kind in items:
        if not profile and kind in (DSC_PATCH, DSC_FILE, DSC_PROFILE):
            return
        if profile and kind == DSC_DEFAULT:
            return
        if kind in (DSC_BITFIELD, DSC_REGISTER):
            yield offset, kind


def raw_writes(cnt, address, profile_name, names):
    dev = next(d for d in cnt.devices if d["address"] == address)
    prof = next((p for p in dev["profiles"] if p["name"] == profile_name), None)
    if prof is None:
        raise SystemExit("device 0x%02x has no profile %r" % (address, profile_name))
    out = []
    for items, is_profile in ((dev["items"], False), (prof["items"], True)):
        for offset, kind in items_written(cnt, items, is_profile):
            if kind == DSC_REGISTER:
                register, value, mask = cnt.register(offset)
                out.append(Write(register, mask, value & mask, "register 0x%02x" % register))
                continue
            value, field = cnt.bitfield(offset)
            register, pos, length = field_parts(field)
            out.append(Write(register, ((1 << length) - 1) << pos, (value << pos) & ((1 << length) - 1) << pos,
                             names.get(field, "field 0x%04x" % field)))
    return out


def tuning_writes(cnt, address, profile_name, names, with_gain=False):
    """what dlisa.tfa9873.c applies to the chip at this address: the device list, then the profile, less LEFT_OUT"""
    return [w for w in raw_writes(cnt, address, profile_name, names)
            if not is_left_out(w.name) or (with_gain and w.name == "TDMSPKG")]


def tuning_header(cnt, profile_name, names, source):
    lines = ["/*",
             " * Generated by tfa-cnt.py from %s (%s %s %s), profile '%s': do not edit." % (source, cnt.customer,
                                                                                        cnt.application, cnt.type,
                                                                                        profile_name),
             " * Each entry: register, mask, value (already shifted), the field's NXP name; read, modified, written, read back.",
             " * Left out (the stock bus, rate and clock; see tfa-cnt.py LEFT_OUT):"]
    for name, why in LEFT_OUT.items():
        lines.append(" *   %s: %s" % (name, why))
    lines.append(" *   TDM*: the stock 4x32-bit slots and the sense data's routing")
    lines.append(" */")
    for dev in cnt.devices:
        address = dev["address"]
        lines.append("static const struct dlisa_tfa_tuning dlisa_tfa_tuning_%02x[] = {	/* %s */" % (address, dev["name"]))
        for w in tuning_writes(cnt, address, profile_name, names):
            lines.append("	{ 0x%02x, 0x%04x, 0x%04x, \"%s\" }," % (w.register, w.mask, w.value, w.name))
        lines.append("};")
        gain = [w for w in raw_writes(cnt, address, profile_name, names) if w.name == "TDMSPKG"]
        lines.append("#define DLISA_TFA_TUNING_SPKG_%02x %d" % (address, gain[-1].value >> 6 if gain else -1))
    lines.append("#define DLISA_TFA_TUNING_PROFILE \"%s\"" % profile_name)
    return "\n".join(lines) + "\n"


def main(argv):
    if len(argv) < 3 or argv[1] not in ("dump", "header"):
        raise SystemExit(__doc__)
    with open(argv[2], "rb") as f:
        cnt = Container(f.read())
    if argv[1] == "dump":
        dump(cnt, load_names(argv[3] if len(argv) > 3 else None))
        return
    if len(argv) < 5:
        raise SystemExit(__doc__)
    sys.stdout.write(tuning_header(cnt, argv[3], load_names(argv[4]), argv[2].rsplit("/", 1)[-1]))


if __name__ == "__main__":
    main(sys.argv)
