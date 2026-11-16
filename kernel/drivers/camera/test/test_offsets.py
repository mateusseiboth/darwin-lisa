#!/usr/bin/env python3
"""hakios.camera.c's hardware numbers for lisa, checked against what pmOS runs the cameras with.

- pmOS's lisa device tree (chroot_rootfs_xiaomi-lisa/boot/sm7325-xiaomi-lisa.dtb): every sensor's CCI master and its
  pins, reset GPIO, MCLK (which camcc clock, which pin), CSIPHY, supplies (PM8008 LDO and set point, or the GPIO of a
  fixed regulator), and the CAMSS blocks' addresses;
- Linux 7.2.9's drivers/clk/qcom/camcc-sc7280.c: the RCG and branch registers of every clock the driver touches;
- Linux's camss-csiphy-3ph-1-0.c lane_regs_sm8250[] (what sc7280 uses for D-PHY): the CSIPHY lane sequence, write for
  write, against the one hakios.camera.c makes (Android's csiphy_2ph_v1_2_1_reg).

The Linux files are read out of pmbootstrap's source tarball of that kernel (cached under $TMPDIR).
"""
import os
import re
import sys
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fdt  # noqa: E402

DRIVER = os.path.join(os.path.dirname(HERE), "hakios.camera.c")
DTB = os.path.expanduser("~/.local/var/pmbootstrap/chroot_rootfs_xiaomi-lisa/boot/sm7325-xiaomi-lisa.dtb")
TARBALL = os.path.expanduser("~/.local/var/pmbootstrap/cache_distfiles/linux-v7.2.9-sc7280.tar.gz")
LINUX_FILES = {
    "camcc": "drivers/clk/qcom/camcc-sc7280.c",
    "bindings": "include/dt-bindings/clock/qcom,camcc-sc7280.h",
    "csiphy": "drivers/media/platform/qcom/camss/camss-csiphy-3ph-1-0.c",
}
CACHE = os.path.join(tempfile.gettempdir(), "dlisa-camera-linux-7.2.9")
SUPPLY_NAMES = {"vana": ("avdd-supply", "vana-supply"), "vdig": ("dvdd-supply", "vdig-supply"),
                "vio": ("dovdd-supply", "vio-supply")}
SENSOR_COMPATIBLES = {"S5K5E9": "samsung,s5k5e9", "S5KGW3": "samsung,s5kgw3", "S5K3T2": "samsung,s5k3t2",
                      "IMX355": "sony,imx355"}


def read_text(path):
    with open(path) as f:
        return f.read()


def linux_sources():
    """The three Linux files, out of the tarball once."""
    if all(os.path.exists(os.path.join(CACHE, os.path.basename(p))) for p in LINUX_FILES.values()):
        return {k: read_text(os.path.join(CACHE, os.path.basename(p))) for k, p in LINUX_FILES.items()}
    os.makedirs(CACHE, exist_ok=True)
    wanted = {"linux-7.2.9-sc7280/" + p: p for p in LINUX_FILES.values()}
    with tarfile.open(TARBALL, "r|gz") as tar:
        for member in tar:
            if member.name in wanted:
                data = tar.extractfile(member).read()
                with open(os.path.join(CACHE, os.path.basename(wanted.pop(member.name))), "wb") as f:
                    f.write(data)
                if not wanted:
                    break
    return linux_sources()


def camcc_registers(text):
    regs = {}
    for kind, name, body in re.findall(r"static struct (clk_rcg2|clk_branch|gdsc) (\w+) = \{(.*?)\n\};", text, re.S):
        found = re.search(r"\.(cmd_rcgr|halt_reg|gdscr) = (0x[0-9a-f]+)", body)
        if found:
            regs[name] = int(found.group(2), 16)
    return regs


def camcc_bindings(text):
    return {int(v, 0): n.lower() for n, v in re.findall(r"#define (CAM_CC_\w+)\s+(\d+|0x[0-9a-fA-F]+)", text)}


def driver_sensors(text):
    body = re.search(r"static const struct sensor_hw sensors\[\] = \{(.*?)\n\};", text, re.S).group(1)
    sensors = []
    for block in re.split(r"\n\t\},", body):
        fields = {}
        for name, value in re.findall(r"\.(\w+) = (\{[^}]*\}|\"[^\"]*\"|[^,\n]+)", block):
            value = value.strip()
            if value.startswith("{"):
                fields[name] = [v.strip() for v in value.strip("{}").split(",")]
            elif value.startswith('"'):
                fields[name] = value.strip('"')
            else:
                fields[name] = value
        if "name" in fields:
            sensors.append(fields)
    return sensors


def number(value):
    return int(value, 0)


@unittest.skipUnless(os.path.exists(DTB) and os.path.exists(TARBALL), "pmOS's lisa rootfs or kernel tarball not here")
class Offsets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tree = fdt.load(DTB)
        src = linux_sources()
        cls.camcc = camcc_registers(src["camcc"])
        cls.bindings = camcc_bindings(src["bindings"])
        cls.csiphy_linux = src["csiphy"]
        with open(DRIVER) as f:
            cls.driver = f.read()
        cls.sensors = {s["chip"]: s for s in driver_sensors(cls.driver)}
        cls.camss = cls.tree.find("qcom,sc7280-camss")[0]

    def pins_of(self, phandle):
        node = self.tree.phandle(phandle)
        return [int(p[4:]) for p in node.strings("pins")]

    def test_every_lisa_sensor_is_in_the_driver(self):
        self.assertEqual(set(self.sensors), set(SENSOR_COMPATIBLES))

    def test_bus_pins_reset_and_mclk(self):
        for chip, compatible in SENSOR_COMPATIBLES.items():
            with self.subTest(chip):
                s = self.sensors[chip]
                node = self.tree.find(compatible)[0]
                bus = node.parent
                cci = bus.parent
                pins = self.pins_of(cci.u32s("pinctrl-0")[bus.u32s("reg")[0]])
                self.assertEqual([number(s["sda"]), number(s["scl"])], pins)
                self.assertEqual(number(s["reset"]), node.u32s("reset-gpios")[1])
                clock = self.bindings[node.u32s("clocks")[1]]              # cam_cc_mclkN_clk
                self.assertEqual(number(s["mclk_cbcr"]), self.camcc[clock])
                self.assertEqual(number(s["mclk_cmd"]), self.camcc[clock + "_src"])
                self.assertEqual(number(s["mclk_pin"]), self.pins_of(node.u32s("pinctrl-0")[0])[0])
                self.assertEqual(node.u32s("assigned-clock-rates")[0], 19200000)

    def test_csiphy_and_its_clocks(self):
        for chip, compatible in SENSOR_COMPATIBLES.items():
            with self.subTest(chip):
                s = self.sensors[chip]
                node = self.tree.find(compatible)[0]
                endpoint = node.child("port").child("endpoint")
                remote = self.tree.phandle(endpoint.u32s("remote-endpoint")[0])
                n = remote.parent.u32s("reg")[0]
                self.assertEqual(number(s["csiphy"]), n)
                self.assertEqual(number(s["phy_cbcr"]), self.camcc[f"cam_cc_csiphy{n}_clk"])
                self.assertEqual(number(s["timer_cmd"]), self.camcc[f"cam_cc_csi{n}phytimer_clk_src"])
                self.assertEqual(number(s["timer_cbcr"]), self.camcc[f"cam_cc_csi{n}phytimer_clk"])
                self.assertEqual(remote.u32s("bus-type")[0], 4)            # MEDIA_BUS_TYPE_CSI2_DPHY

    def test_supplies(self):
        for chip, compatible in SENSOR_COMPATIBLES.items():
            node = self.tree.find(compatible)[0]
            for role, props in SUPPLY_NAMES.items():
                with self.subTest(chip=chip, supply=role):
                    kind, ldo, mv, gpio, _ramp = self.sensors[chip][role]
                    prop = next(p for p in props if p in node.props)
                    regulator = self.tree.phandle(node.u32s(prop)[0])
                    low, high = regulator.u32s("regulator-min-microvolt")[0], regulator.u32s("regulator-max-microvolt")[0]
                    self.assertTrue(low <= number(mv) * 1000 <= high, f"{mv} mV outside {low}..{high} uV")
                    if "regulator-fixed" in regulator.strings("compatible") if "compatible" in regulator.props else []:
                        self.assertEqual(kind, "SUPPLY_GPIO")
                        self.assertEqual(number(gpio), regulator.u32s("gpio")[1])
                    else:
                        self.assertEqual(regulator.parent.name, "regulators")
                        self.assertIn("qcom,pm8008", regulator.parent.parent.strings("compatible"))
                        self.assertEqual(kind, "SUPPLY_LDO")
                        self.assertEqual(number(ldo), int(regulator.name[3:]))

    def test_the_pm8008_is_where_the_driver_bit_bangs_it(self):
        pmic = self.tree.find("qcom,pm8008")[0]
        self.assertEqual(pmic.u32s("reg")[0], 0x08)
        self.assertEqual(self.pins_of(pmic.parent.u32s("pinctrl-0")[0]), [4, 5])
        reset = pmic.u32s("reset-gpios")
        self.assertIn("qcom,pm8350c-gpio", self.tree.phandle(reset[0]).strings("compatible"))
        self.assertEqual(reset[1], 3)
        self.assertIn("#define PIN_PMIC_SDA            4", self.driver)
        self.assertIn("#define PGPIO_ID                0x28a", self.driver)

    def test_block_addresses(self):
        names = self.camss.strings("reg-names")
        regs = self.camss.u32s("reg")
        base = {names[i]: regs[4 * i + 1] for i in range(len(names))}
        self.assertIn(f"#define CSID_PHYS               {base['csid_lite0']:#010x}UL", self.driver)
        self.assertIn(f"#define IFE_PHYS                {base['vfe_lite0']:#010x}UL", self.driver)
        for n in range(5):
            self.assertEqual(base[f"csiphy{n}"], 0x0ace0000 + 0x2000 * n)
        self.assertEqual(self.camss.u32s("iommus")[1:], [0x800, 0x4e0])

    def test_titan_clocks(self):
        rcgs = dict(re.findall(r'\{ "(\w+) [^"]*", (0x[0-9a-f]+), \d+, \d+ \}', self.driver))
        for name, cmd in rcgs.items():
            with self.subTest(name):
                self.assertEqual(number(cmd), self.camcc[f"cam_cc_{name}_clk_src"])
        branches = dict(re.findall(r'\{ "(\w+)", false, (0x[0-9a-f]+) \}', self.driver))
        for name, cbcr in branches.items():
            with self.subTest(name):
                self.assertEqual(number(cbcr), self.camcc[f"cam_cc_{name}_clk"])
        self.assertEqual(self.camcc["cam_cc_titan_top_gdsc"], 0xc194)
        self.assertIn("#define TITAN_TOP_GDSCR         0xc194", self.driver)

    def test_csiphy_dphy_lanes_match_linux(self):
        ours = dphy_writes(self.driver, settle=0x10)
        body = re.search(r"lane_regs_sm8250\[\] = \{(.*?)\n\};", self.csiphy_linux, re.S).group(1)
        linux = [(int(a, 16), int(v, 16)) for a, v, _, kind in
                 re.findall(r"\{(0x[0-9a-fA-F]+), (0x[0-9a-fA-F]+), (0x[0-9a-fA-F]+), (\w+)\}", body)
                 if kind != "CSIPHY_DNP_PARAMS"]
        self.assertEqual(ours, linux)


def dphy_writes(driver, settle):
    """csiphy_dphy()'s writes in order, its lane table expanded: (offset, value) each."""
    body = re.search(r"^csiphy_dphy\(unsigned settle\)\n\{(.*?)^\}", driver, re.S | re.M).group(1)
    lanes = [(int(b, 16), int(m, 16)) for b, m in re.findall(r"\{ (0x[0-9a-f]+), (0x[0-9a-f]+) \}", body)]
    calls = re.findall(r"(\(clock && )?!blk_put\(csiphy, \"CSIPHY\", ([^,]+), ([^,]+), true\)", body)
    writes = []
    for index, (b, m) in enumerate(lanes):
        clock = index == 1
        for only_clock, where, value in calls:
            if only_clock and not clock:
                continue
            where = where.replace("b", str(b)).replace("m", str(m))
            value = value.replace("settle & 0xff", str(settle)).replace("clock", "True" if clock else "False")
            value = re.sub(r"(\w+) \? (\w+) : (\w+)", lambda x: x.group(2) if x.group(1) == "True" else x.group(3), value)
            writes.append((eval(where, {}), eval(value, {})))
    return writes


if __name__ == "__main__":
    unittest.main()
