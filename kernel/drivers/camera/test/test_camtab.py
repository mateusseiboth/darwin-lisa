#!/usr/bin/env python3
"""camtab.py: the sensor tables the kernel builds in, checked against the Linux driver they come from.

What the kernel's camera driver writes to a sensor (hakios.camera.c: power_up(), read_chip_id(), sensor_table("init"),
sensor_table("res0"), sensor_table("on")) has to be what pmOS's driver writes when libcamera starts the stream
(s5k5e9_power_on(), s5k5e9_start_stream(): the common list, the mode's list, __v4l2_ctrl_handler_setup(), 0x0100 = 1).
The tables are made from the patch itself, so these tests read the same patch and replay Linux's order by hand.
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import camtab  # noqa: E402

PATCH = camtab.SENSORS["s5k5e9"]["patch"]


def entries(table):
    return [(e["address"], e["data"], e["data_bytes"]) for e in table]


@unittest.skipUnless(os.path.exists(PATCH), "the pmaports patch of the S5K5E9 driver is not on this machine")
class S5k5e9Table(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(PATCH) as f:
            cls.text = f.read()
        cls.arrays = camtab.parse_reg_arrays(cls.text)
        cls.modes = camtab.parse_modes(cls.text, "s5k5e9")
        cls.tab = camtab.build_tab("s5k5e9")
        cls.parsed = camtab.parse_tab(cls.tab)

    def test_the_patch_arrays_are_read_whole(self):
        self.assertEqual(len(self.arrays["s5k5e9_regs"]), 42)
        self.assertEqual(self.arrays["s5k5e9_regs"][0], (0x0100, 0x00))
        self.assertEqual(self.arrays["s5k5e9_regs"][-1], (0x3c35, 0x5c))
        self.assertEqual(len(self.arrays["s5k5e9_2592x1944_2lane_regs"]), 48)

    def test_the_mode_is_the_patch_mode(self):
        self.assertEqual(self.modes, [{"width": 2592, "height": 1944, "hts": 3112, "vts": 2030, "link_freq": 480000000,
                                       "lane_count": 2, "depth": 10, "regs": "s5k5e9_2592x1944_2lane_regs"}])

    def test_head_is_the_device_tree_and_the_probe(self):
        head = self.parsed["head"]
        self.assertEqual(head["slave"], 0x2d << 1)              # camera@2d on cci1_i2c0, the module file's 8-bit form
        self.assertEqual((head["address_bytes"], head["data_bytes"]), (2, 2))
        self.assertEqual((head["id_address"], head["id"], head["id_mask"]), (0x0000, 0x559b, 0xffff))
        self.assertEqual(head["lane_assign"], 0x10)             # data-lanes <0 1> on csiphy2
        self.assertIn("0x559b", self.text)
        self.assertIn("s5k5e9_read(s5k5e9, 0x0000, 2, &id)", self.text)

    def test_the_mode_row(self):
        mode = self.parsed["modes"][0]
        self.assertEqual((mode["width"], mode["height"], mode["lanes"], mode["cphy"]), (2592, 1944, 2, 0))
        self.assertEqual(mode["pixel_clock"], 480000000 * 2 * 2 // 10)    # Linux's V4L2_CID_PIXEL_RATE
        self.assertEqual((mode["line_length"], mode["frame_length"]), (3112, 2030))
        self.assertEqual(mode["fps_milli"], 192000000 * 1000 // (3112 * 2030))
        self.assertEqual(mode["data_type"], 0x2b)
        # camss-csiphy-3ph-1-0.c csiphy_settle_cnt_calc() at the 300 MHz csiphy timer: (85 ns + 6 UI) / 3.33 ns - 6
        self.assertEqual(mode["settle"], 21)

    def test_settle_is_linux_formula(self):
        self.assertEqual(camtab.settle_count(480000000, 300000000), 21)
        self.assertEqual(camtab.settle_count(1000000000, 300000000), 20)

    def test_power_up_is_s5k5e9_power_on(self):
        # reset low, 1 ms; the supplies (regulator_bulk_enable: vio first here, as Android's sequences have it), 1 ms;
        # xvclk at 19.2 MHz, 10 ms; reset high, 18 ms
        up = [(e["address"], e["data"], e["delay_us"]) for e in self.parsed["power"]["pwrup"]]
        self.assertEqual(up, [(8, 0, 1000), (3, 1, 0), (1, 1, 0), (2, 1, 1000), (0, 19200, 10000), (8, 1, 18000)])

    def test_power_down_is_s5k5e9_power_off(self):
        down = [(e["address"], e["data"], e["delay_us"]) for e in self.parsed["power"]["pwrdn"]]
        self.assertEqual(down, [(0, 0, 1000), (8, 0, 1000), (2, 0, 0), (1, 0, 0), (3, 0, 0)])

    def test_stream_on_replays_linux_order(self):
        # s5k5e9_start_stream(): the common list, the mode list, then __v4l2_ctrl_handler_setup() in the order the
        # controls were made (VBLANK, EXPOSURE, ANALOGUE_GAIN), each inside 0x0104 group hold, then 0x0100 = 1
        expected = [(a, v, 1) for a, v in self.arrays["s5k5e9_regs"]]
        expected += [(a, v, 1) for a, v in self.arrays["s5k5e9_2592x1944_2lane_regs"]]
        for register, value in ((0x0340, 2030), (0x0202, 2028), (0x0204, 1024)):
            expected += [(0x0104, 1, 1), (register, value, 2), (0x0104, 0, 1)]
        expected += [(0x0100, 1, 1)]
        tables = self.parsed["tables"]
        written = entries(tables["init"]) + entries(tables["res0"]) + entries(tables["on"])
        self.assertEqual(written, expected)

    def test_every_write_is_two_address_bytes(self):
        for name, table in self.parsed["tables"].items():
            for e in table:
                self.assertEqual(e["address_bytes"], 2, name)
                self.assertEqual(e["operation"], 0, name)

    def test_stream_off_and_hold(self):
        tables = self.parsed["tables"]
        self.assertEqual(entries(tables["off"]), [(0x0100, 0, 1)])
        self.assertEqual(entries(tables["holdon"]), [(0x0104, 1, 1)])
        self.assertEqual(entries(tables["holdoff"]), [(0x0104, 0, 1)])

    def test_the_layout_is_the_kernel_one(self):
        # struct ctab_head: magic[8] + 8 u32; struct ctab_mode: 12 u32; struct ctab_entry: u16 u16 u8 u8 u8 u8 u32
        self.assertEqual(self.tab[:8], b"RWCTAB02")
        self.assertEqual(struct.calcsize("<8I"), 32)
        self.assertEqual(struct.calcsize("<HHBBBBI"), 12)
        self.assertEqual(self.parsed["size"], len(self.tab))


class Header(unittest.TestCase):
    def test_c_header_carries_the_bytes(self):
        text = camtab.c_header({"camera-test.tab": b"RWCTAB02\x01\xff"})
        self.assertIn("camera-test.tab", text)
        self.assertIn("0x52, 0x57, 0x43, 0x54, 0x41, 0x42, 0x30, 0x32, 0x01, 0xff", text)
        self.assertIn("hakios_camera_builtin_tabs[]", text)
        self.assertIn("10 }", text)

    def test_parse_rejects_a_short_file(self):
        with self.assertRaises(ValueError):
            camtab.parse_tab(b"RWCTAB02")


if __name__ == "__main__":
    unittest.main()
