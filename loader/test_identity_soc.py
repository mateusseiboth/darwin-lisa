#!/usr/bin/env python3
"""Tests of identity-soc.py on a small device tree made here (no Apple data needed).

    python3 -I darwin-lisa/loader/test_identity_soc.py
"""
import importlib.util
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE / 'identity-soc.py'


def load_module():
    spec = importlib.util.spec_from_file_location('identity_soc', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prop(name, value):
    return name.encode().ljust(32, b'\0') + struct.pack('<I', len(value)) + value + b'\0' * (-len(value) % 4)


def node(props, children=()):
    body = b''.join(prop(k, v) for k, v in props.items())
    return struct.pack('<II', len(props), len(children)) + body + b''.join(children)


def tree(board='D16AP', buttons=None):
    """the root, /arm-io (arm-io,t8110) and, when given, /buttons with these properties"""
    children = [node({'name': b'arm-io\0', 'compatible': b'arm-io,t8110\0'})]
    if buttons is not None:
        children.append(node(dict({'name': b'buttons\0'}, **buttons)))
    return node({'name': b'device-tree\0', 'target-sub-type': board.encode() + b'\0'}, children)


def header(board='D16AP'):
    data = ', '.join(f'0x{b:02x}' for b in board.encode() + b'\0')
    return f'static const unsigned char identity_root_data[] = {{ {data} }};\n'


class HomeButtonType(unittest.TestCase):
    def setUp(self):
        self.soc = load_module()

    def test_reads_home_button_type_of_buttons(self):
        root, _ = self.soc.parse(tree(buttons={'home-button-type': struct.pack('<I', 2)}))
        self.assertEqual(self.soc.home_button_type(root), 2)

    def test_none_without_buttons_node(self):
        root, _ = self.soc.parse(tree())
        self.assertIsNone(self.soc.home_button_type(root))

    def test_none_without_the_property(self):
        root, _ = self.soc.parse(tree(buttons={'button-names': b'volup\0'}))
        self.assertIsNone(self.soc.home_button_type(root))


class Header(unittest.TestCase):
    def run_script(self, dt_bytes, text):
        with tempfile.TemporaryDirectory() as d:
            dt, hdr = os.path.join(d, 'dt.bin'), os.path.join(d, 'identity.h')
            open(dt, 'wb').write(dt_bytes)
            open(hdr, 'w').write(text)
            subprocess.run([sys.executable, '-I', str(SCRIPT), dt, hdr], check=True, capture_output=True)
            return open(hdr).read()

    def test_block_carries_home_button_type(self):
        out = self.run_script(tree(buttons={'home-button-type': struct.pack('<I', 2)}), header())
        self.assertIn('#define IDENTITY_HOME_BUTTON_TYPE  2\n', out)
        self.assertIn('#define IDENTITY_CHIP_ID           0x8110\n', out)

    def test_block_without_buttons_has_no_home_button_type(self):
        out = self.run_script(tree(), header())
        self.assertNotIn('IDENTITY_HOME_BUTTON_TYPE', out)
        self.assertIn('#define IDENTITY_BOARD_ID          0x08\n', out)

    def test_second_run_replaces_the_block(self):
        dt = tree(buttons={'home-button-type': struct.pack('<I', 2)})
        once = self.run_script(dt, header())
        twice = self.run_script(dt, once)
        self.assertEqual(once, twice)
        self.assertEqual(twice.count('IDENTITY_HOME_BUTTON_TYPE'), 1)


if __name__ == '__main__':
    unittest.main()
