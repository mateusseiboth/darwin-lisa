"""tfa-cnt.py against the stock lisa container (tfa98xx.cnt) and NXP's TFA9873 field names, when both are on this host.

What a DSP-free TFA9873 on pmOS's bus can take from Xiaomi's tuning: the amplifier's own limits and boost settings, never
the stock bus format (4 slots of 32 bits at 96 kHz with sense data back to the ADSP), the stock PLL reference, nor the
gain unless asked for.
"""
import importlib.util
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
M = os.path.expanduser("~/Dev/mobile")
CNT = os.path.join(M, "stock/super/vendor/firmware/tfa98xx.cnt")
NAMES = os.path.join(M, "research/tfa98xx-lisa-r-oss/techpack/audio/asoc/codecs/tfa98xx/inc/tfa9873_tfafieldnames_B0.h")


def load_tool():
    spec = importlib.util.spec_from_file_location("tfa_cnt", os.path.join(HERE, "..", "tfa-cnt.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(os.path.exists(CNT) and os.path.exists(NAMES), "stock container or NXP field names not on this host")
class StockContainer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = load_tool()
        with open(CNT, "rb") as f:
            cls.cnt = cls.tool.Container(f.read())
        cls.names = cls.tool.load_names(NAMES)

    def tuning(self, address, gain=False):
        return self.tool.tuning_writes(self.cnt, address, "stereo", self.names, with_gain=gain)

    def by_name(self, address, gain=False):
        return {w.name: w for w in self.tuning(address, gain)}

    def test_two_devices_at_the_stock_addresses(self):
        self.assertEqual([d["address"] for d in self.cnt.devices], [0x34, 0x35])
        self.assertEqual([d["name"] for d in self.cnt.devices], ["RCV", "SPK"])

    def test_the_current_limits_and_the_boost_are_taken(self):
        for address in (0x34, 0x35):
            names = self.by_name(address)
            for field in ("DCMCC", "ref_amp_irefdist_set_ctrl", "ref_bst_irefdist_set_ctrl", "DCVOS", "DCTRIP",
                          "DCTRIPT", "bst_slope", "DCPWM", "LPM1MODE", "LNMODE"):
                self.assertIn(field, names, "%s of 0x%02x" % (field, address))

    def test_max_coil_current_is_the_stock_nine(self):
        dcmcc = self.by_name(0x35)["DCMCC"]
        self.assertEqual((dcmcc.register, dcmcc.mask, dcmcc.value), (0x70, 0x000F, 0x0009))

    def test_boost_ocp_scaling_lands_in_register_0x73_bits_7_and_8(self):
        w = self.by_name(0x34)["ref_bst_irefdist_set_ctrl"]
        self.assertEqual((w.register, w.mask, w.value), (0x73, 0x0180, 0x0180))

    def test_the_stock_bus_format_rate_and_clock_are_left_out(self):
        for address in (0x34, 0x35):
            names = self.by_name(address)
            for field in ("AUDFS", "TDMNBCK", "TDMSLLN", "TDMSSIZE", "TDMSLOTS", "TDMSPKS", "TDMSPKE", "TDMCSE",
                          "TDMSRCMAP", "REFCKEXT", "AMPINSEL", "mtp_pll_lut_sel", "FRACTDEL", "DCA"):
                self.assertNotIn(field, names, "%s of 0x%02x" % (field, address))

    def test_no_write_touches_the_sample_rate_or_the_slot_registers(self):
        for address in (0x34, 0x35):
            for w in self.tuning(address, gain=True):
                self.assertNotIn(w.register, (0x00, 0x02, 0x04, 0x20, 0x21, 0x22, 0x23, 0x26, 0x27, 0x68), w.name)

    def test_the_gain_only_when_asked(self):
        self.assertNotIn("TDMSPKG", self.by_name(0x35))
        spkg = self.by_name(0x35, gain=True)["TDMSPKG"]
        self.assertEqual((spkg.register, spkg.mask, spkg.value), (0x5F, 0x03C0, 6 << 6))
        self.assertEqual(self.by_name(0x34, gain=True)["TDMSPKG"].value, 0)

    def test_the_profile_stops_at_its_default_marker(self):
        # 'stereo' has nothing after its default; 'handset' restores TDMSPKG 0 and DCVOS 0x2e after it
        handset = self.tool.tuning_writes(self.cnt, 0x34, "handset", self.names, with_gain=True)
        spkg = [w for w in handset if w.name == "TDMSPKG"]
        self.assertEqual(spkg[-1].value, 5 << 6)

    def test_header_is_c_with_both_tables(self):
        text = self.tool.tuning_header(self.cnt, "stereo", self.names, "tfa98xx.cnt")
        self.assertIn("dlisa_tfa_tuning_34[]", text)
        self.assertIn("dlisa_tfa_tuning_35[]", text)
        entries = [line for line in text.splitlines() if line.startswith("\t{")]
        self.assertTrue(any("\"DCMCC\"" in line for line in entries))
        self.assertFalse(any("AUDFS" in line or "TDMSPKG" in line for line in entries))
        self.assertIn("#define DLISA_TFA_TUNING_SPKG_35 6", text)


if __name__ == "__main__":
    unittest.main()
