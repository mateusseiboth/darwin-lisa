#!/usr/bin/env python3
"""Add the SoC half of an iPhone's identity to a loader identity header (darwin-lisa/loader/identity-soc.patch).

usage: identity-soc.py DeviceTree.<board>.bin IDENTITY_HEADER

mkidentity.py copies the root and /product of the iPhone's device tree. What iBoot fills in on a real phone is not in
that tree: the chip and board IDs are zero there, and the regulatory model number is a "syscfg/RMd#" placeholder. The
loader used to write the iPhone X's (t8015, chip-id 0x8015, board-id 6, A1865) whatever the identity was. This script
appends #defines with the values that belong to the identity's own board, and the loader takes them when they are there
(the iPhone X's otherwise):

  IDENTITY_PLATFORM_NAME     root "platform-name": from the tree's /arm-io "compatible" (arm-io,tNNNN)
  IDENTITY_CHIP_ID           /chosen "chip-id": 0xNNNN of that same tNNNN (Apple numbers its SoCs so: t8015 = 0x8015)
  IDENTITY_BOARD_ID          /chosen "board-id": the board's ApBoardID (BOARDS below)
  IDENTITY_REGULATORY_MODEL  root "regulatory-model-number" (BOARDS below)

Run it again on the same header: the block it wrote before is replaced. The header stays Apple data, out of git.
"""
import re
import struct
import sys

# ApBoardID from the IPSW's BuildManifest (also "ipsw device-info --prod iPhone14,4"); the regulatory model is the US one
BOARDS = {
    'D22AP': {'board_id': 0x06, 'regulatory': 'A1865'},        # iPhone10,3, iPhone X
    'D16AP': {'board_id': 0x08, 'regulatory': 'A2481'},        # iPhone14,4, iPhone 13 mini
}
BEGIN = '/* --- identity-soc.py: the SoC and board (darwin-lisa/loader/identity-soc.patch) --- */'
END = '/* --- end of identity-soc.py --- */'


def parse(data, pos=0):
    nprops, nchildren = struct.unpack_from('<II', data, pos)
    pos += 8
    props = {}
    for _ in range(nprops):
        name = data[pos:pos + 32].split(b'\0')[0].decode()
        length = struct.unpack_from('<I', data, pos + 32)[0] & 0x7FFFFFFF
        props[name] = data[pos + 36:pos + 36 + length]
        pos += 36 + (length + 3 & ~3)
    children = []
    for _ in range(nchildren):
        child, pos = parse(data, pos)
        children.append(child)
    return (props, children), pos


def text(value):
    return value.split(b'\0')[0].decode()


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, _ = parse(open(sys.argv[1], 'rb').read())
    board = text(root[0]['target-sub-type'])
    if board not in BOARDS:
        sys.exit(f'identity-soc: no board table entry for {board}')
    arm_io = next(c for c in root[1] if text(c[0].get('name', b'')) == 'arm-io')
    m = re.fullmatch(r'arm-io,(t([0-9a-f]{4}))', text(arm_io[0]['compatible']))
    if m is None:
        sys.exit(f'identity-soc: /arm-io compatible {arm_io[0]["compatible"]!r} is not arm-io,tNNNN')
    header = open(sys.argv[2]).read()
    if board.encode() + b'\0' not in _root_bytes(header):
        sys.exit(f'identity-soc: {sys.argv[2]} is not the identity of {board} (mkidentity.py of the same tree)')
    info = BOARDS[board]
    block = '\n'.join([
        BEGIN,
        f'#define IDENTITY_PLATFORM_NAME     "{m.group(1)}"',
        f'#define IDENTITY_CHIP_ID           0x{m.group(2)}',
        f'#define IDENTITY_BOARD_ID          0x{info["board_id"]:02x}',
        f'#define IDENTITY_REGULATORY_MODEL  "{info["regulatory"]}"',
        END,
    ]) + '\n'
    header = re.sub(re.escape(BEGIN) + r'.*?' + re.escape(END) + r'\n', '', header, flags=re.S)
    open(sys.argv[2], 'w').write(header.rstrip('\n') + '\n\n' + block)
    print(f'{sys.argv[2]}: {board}: platform {m.group(1)}, chip-id 0x{m.group(2)}, board-id 0x{info["board_id"]:02x}, '
          f'regulatory {info["regulatory"]}')


def _root_bytes(header):
    """the bytes of identity_root_data, where target-sub-type is"""
    m = re.search(r'identity_root_data\[\] = \{(.*?)\};', header, re.S)
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m.group(1))) if m else b''


if __name__ == '__main__':
    main()
