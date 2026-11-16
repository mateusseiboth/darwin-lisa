#!/usr/bin/env python3
"""The camera sensors' tables for hakios.camera.c, made from pmOS's own sensor drivers.

HakiOS's driver reads a sensor's tables from a "RWCTAB02" file that tools/camsensor.py makes of Android's CamX module
file (com.qti.sensormodule.*.bin). lisa's stock vendor image is not on this machine, but the pmOS kernel that drives
all four cameras is: its sensor drivers (xiaomi-lisa-mainline/pmaports/.../linux-postmarketos-qcom-sc7280/*.patch)
hold the register lists, and the device tree (sm7325-xiaomi-lisa.dtb) the address, the lanes and the supplies. This
script writes the same RWCTAB02 layout from them, with the writes in the order the Linux driver makes them when the
stream starts (test/test_camtab.py replays that order):

  pwrup    the driver's power_on(): reset low, supplies, MCLK, reset high, with its waits
  pwrdn    its power_off()
  init     the list written first at every stream start
  res0..   each mode's own list, then what __v4l2_ctrl_handler_setup() writes (VBLANK, EXPOSURE, ANALOGUE_GAIN at
           their defaults, each inside the 0x0104 group hold)
  on, off  0x0100 = 1 / 0
  holdon, holdoff   0x0104 = 1 / 0 (what the driver wraps every exposure write in)

The tables are GPL-2.0 data of the Linux driver: they are not kept in this repository. camera-wire.py runs this
script and builds the result into the kernel (bsd/dev/hakios.camera_tab.h).

  python3 -I camtab.py list               what each table holds
  python3 -I camtab.py tab NAME OUT.tab   one sensor's RWCTAB02 file
  python3 -I camtab.py header OUT.h       every sensor whose driver patch is here, as a C header
"""
import os
import re
import struct
import sys

PATCHES = os.path.expanduser(
    "~/Dev/mobile/xiaomi-lisa-mainline/pmaports/device/community/linux-postmarketos-qcom-sc7280")

RAW10_DT = 0x2b
CSIPHY_TIMER_HZ = 300000000     # camcc csiNphytimer as the kernel driver sets it (and Linux's sc7280 table)
POWER_KINDS = {"mclk": 0, "vana": 1, "vdig": 2, "vio": 3, "reset": 8}

# What the device tree and the driver's probe say, per sensor (the register lists and modes come from the patch).
SENSORS = {
    "s5k5e9": {
        "tab": "camera-s5k5e9.tab",
        "patch": os.path.join(PATCHES, "0006-media-i2c-Add-S5K5E9-camera-sensor-driver.patch"),
        "slave": 0x2d,                  # dt: cci1_i2c0 camera@2d
        "id_address": 0x0000, "id": 0x559b, "id_bytes": 2,
        "lane_assign": 0x10,            # dt: data-lanes <0 1> (sensor and csiphy2)
        "mclk_khz": 19200,              # dt: assigned-clock-rates 19200000
        "init": ["s5k5e9_regs"],
        # s5k5e9_power_on(): reset 0, 1 ms; regulator_bulk_enable(vana, vdig, vio), 1 ms; clk, 10 ms; reset 1, 18 ms
        "pwrup": [("reset", 0, 1000), ("vio", 1, 0), ("vana", 1, 0), ("vdig", 1, 1000), ("mclk", None, 10000),
                  ("reset", 1, 18000)],
        # s5k5e9_power_off(): clk off, 1 ms; reset 0, 1 ms; regulators off
        "pwrdn": [("mclk", 0, 1000), ("reset", 0, 1000), ("vdig", 0, 0), ("vana", 0, 0), ("vio", 0, 0)],
        # s5k5e9_init_controls(): exposure default vts - 2, analogue gain 32..1024 default 1024
        "controls": lambda mode: [(0x0340, mode["vts"]), (0x0202, mode["vts"] - 2), (0x0204, 1024)],
    },
}

ARRAY = re.compile(r"static const struct (\w+)_reg (\w+)\[\] = \{(.*?)\n\};", re.S)
PAIR = re.compile(r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}")
MODES = re.compile(r"static struct (\w+)_mode \w+_modes\[\] = \{(.*?)\n\};", re.S)
FIELD = re.compile(r"\.(\w+)\s*=\s*([^,\n]+),")


def added_lines(text):
    """A patch's new file, without the leading '+'."""
    return "\n".join(line[1:] for line in text.splitlines() if line.startswith("+") and not line.startswith("+++"))


def parse_reg_arrays(text):
    source = added_lines(text)
    return {name: [(int(a, 16), int(v, 16)) for a, v in PAIR.findall(body)] for _, name, body in ARRAY.findall(source)}


def parse_modes(text, prefix):
    source = added_lines(text)
    found = [body for kind, body in MODES.findall(source) if kind == prefix]
    if not found:
        raise ValueError(f"no {prefix}_modes[] in the patch")
    modes = []
    for block in re.split(r"\n\t\},", found[0]):
        fields = dict(FIELD.findall(block))
        if "width" not in fields:
            continue
        regs = re.search(r"\.regs = (\w+)", block)
        modes.append({"width": int(fields["width"]), "height": int(fields["height"]), "hts": int(fields["hts"]),
                      "vts": int(fields["vts"]), "link_freq": int(fields["link_freq"]),
                      "lane_count": int(fields["lane_count"]), "depth": int(fields["depth"]),
                      "regs": regs.group(1) if regs else None})
    return modes


def settle_count(link_freq, timer_hz):
    """camss-csiphy-3ph-1-0.c csiphy_settle_cnt_calc(), integer for integer."""
    ui = 1000000000000 // link_freq // 2
    return (85000 + 6 * ui) // (1000000000000 // timer_hz) - 6


def entry(address, data, data_bytes, delay_us=0):
    return struct.pack("<HHBBBBI", address, data, 2, data_bytes, 0, 0, delay_us)


def block(name, rows):
    return name.encode().ljust(8, b"\0") + struct.pack("<I", len(rows)) + b"".join(rows)


def power_rows(spec, which):
    rows = []
    for kind, value, delay in spec[which]:
        data = spec["mclk_khz"] if kind == "mclk" and value is None else value
        rows.append(struct.pack("<HHBBBBI", POWER_KINDS[kind], data, 0, 0, 0, 0, delay))
    return rows


def build_tab(name):
    spec = SENSORS[name]
    with open(spec["patch"]) as f:
        text = f.read()
    arrays = parse_reg_arrays(text)
    modes = parse_modes(text, name)
    out = bytearray(b"RWCTAB02")
    out += struct.pack("<8I", spec["slave"] << 1, 2, spec["id_bytes"], spec["id_address"], spec["id"],
                       (1 << (8 * spec["id_bytes"])) - 1, spec["lane_assign"], len(modes))
    for m in modes:
        pixel = m["link_freq"] * 2 * m["lane_count"] // m["depth"]
        fps_milli = pixel * 1000 // (m["hts"] * m["vts"])
        out += struct.pack("<12I", m["width"], m["height"], 0, 0, m["lane_count"], 0,
                           settle_count(m["link_freq"], CSIPHY_TIMER_HZ), pixel, m["hts"], m["vts"], fps_milli, RAW10_DT)
    out += block("pwrup", power_rows(spec, "pwrup")) + block("pwrdn", power_rows(spec, "pwrdn"))
    tables = [("init", [entry(a, v, 1) for n in spec["init"] for a, v in arrays[n]])]
    for k, m in enumerate(modes):
        rows = [entry(a, v, 1) for a, v in arrays[m["regs"]]]
        for register, value in spec["controls"](m):
            rows += [entry(0x0104, 1, 1), entry(register, value, 2), entry(0x0104, 0, 1)]
        tables.append((f"res{k}", rows))
    tables += [("on", [entry(0x0100, 1, 1)]), ("off", [entry(0x0100, 0, 1)]), ("holdon", [entry(0x0104, 1, 1)]),
               ("holdoff", [entry(0x0104, 0, 1)])]
    out += struct.pack("<I", len(tables)) + b"".join(block(n, rows) for n, rows in tables)
    return bytes(out)


ENTRY_FIELDS = ("address", "data", "address_bytes", "data_bytes", "operation", "zero", "delay_us")
MODE_FIELDS = ("width", "height", "x", "y", "lanes", "cphy", "settle", "pixel_clock", "line_length", "frame_length",
               "fps_milli", "data_type")
HEAD_FIELDS = ("slave", "address_bytes", "data_bytes", "id_address", "id", "id_mask", "lane_assign", "modes")


def read_blocks(data, at, count):
    blocks = {}
    for _ in range(count):
        if at + 12 > len(data):
            raise ValueError("a block runs past the end")
        name = data[at:at + 8].rstrip(b"\0").decode()
        n = struct.unpack_from("<I", data, at + 8)[0]
        at += 12
        if at + 12 * n > len(data):
            raise ValueError(f"block {name} runs past the end")
        blocks[name] = [dict(zip(ENTRY_FIELDS, struct.unpack_from("<HHBBBBI", data, at + 12 * i))) for i in range(n)]
        at += 12 * n
    return blocks, at


def parse_tab(data):
    """What the kernel's ctab_load() and ctab_block() read, with the same checks."""
    if len(data) < 40 or data[:8] != b"RWCTAB02":
        raise ValueError("not a RWCTAB02 table")
    head = dict(zip(HEAD_FIELDS, struct.unpack_from("<8I", data, 8)))
    if head["modes"] == 0 or head["modes"] > 16:
        raise ValueError("modes out of range")
    at = 40
    modes = []
    for _ in range(head["modes"]):
        modes.append(dict(zip(MODE_FIELDS, struct.unpack_from("<12I", data, at))))
        at += 48
    power, at = read_blocks(data, at, 2)
    count = struct.unpack_from("<I", data, at)[0]
    tables, at = read_blocks(data, at + 4, count)
    return {"head": head, "modes": modes, "power": power, "tables": tables, "size": at}


def c_header(tabs):
    lines = ["/* made by kernel/drivers/camera/camtab.py from pmOS's sensor drivers (GPL-2.0 data): do not edit */",
             "struct hakios_camera_builtin_tab {", "\tconst char *name;", "\tconst uint8_t *data;", "\tuint64_t size;",
             "};", ""]
    for k, (name, data) in enumerate(sorted(tabs.items())):
        lines.append(f"static const uint8_t hakios_camera_tab{k}[] __attribute__((aligned(8))) = {{  /* {name} */")
        for i in range(0, len(data), 16):
            lines.append("\t" + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
        lines.append("};")
    lines.append("static const struct hakios_camera_builtin_tab hakios_camera_builtin_tabs[] = {")
    for k, (name, data) in enumerate(sorted(tabs.items())):
        lines.append(f'\t{{ "{name}", hakios_camera_tab{k}, {len(data)} }},')
    lines += ["};", ""]
    return "\n".join(lines)


def available():
    return {s["tab"]: build_tab(n) for n, s in SENSORS.items() if os.path.exists(s["patch"])}


def main(argv):
    if len(argv) >= 2 and argv[1] == "list":
        for name in SENSORS:
            parsed = parse_tab(build_tab(name))
            print(f"{name}: {parsed['head']}")
            for m in parsed["modes"]:
                print(f"  mode {m}")
            for t, rows in parsed["tables"].items():
                print(f"  {t}: {len(rows)} writes")
        return 0
    if len(argv) == 4 and argv[1] == "tab":
        open(argv[3], "wb").write(build_tab(argv[2]))
        return 0
    if len(argv) == 3 and argv[1] == "header":
        tabs = available()
        if not tabs:
            print("camtab.py: no sensor driver patch found under " + PATCHES, file=sys.stderr)
            return 1
        open(argv[2], "w").write(c_header(tabs))
        print(f"camtab.py: {argv[2]}: " + ", ".join(f"{n} ({len(d)} bytes)" for n, d in sorted(tabs.items())))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
