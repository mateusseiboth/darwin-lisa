"""A flattened device tree (.dtb) read into nodes: enough for the camera tests to look up what pmOS's lisa tree says."""
import struct

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


class Node:
    def __init__(self, name, parent):
        self.name, self.parent, self.props, self.children = name, parent, {}, []

    @property
    def path(self):
        return "/" if self.parent is None else (self.parent.path.rstrip("/") + "/" + self.name)

    def u32s(self, prop):
        data = self.props[prop]
        return list(struct.unpack(f">{len(data) // 4}I", data))

    def string(self, prop):
        return self.props[prop].split(b"\0")[0].decode()

    def strings(self, prop):
        return [s.decode() for s in self.props[prop].split(b"\0")[:-1]]

    def child(self, prefix):
        for c in self.children:
            if c.name == prefix or c.name.startswith(prefix + "@"):
                return c
        raise KeyError(f"{self.path}: no {prefix}")

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, _, off_struct, off_strings = struct.unpack_from(">4I", data, 0)
    if magic != 0xd00dfeed:
        raise ValueError(f"{path}: not a device tree blob")
    at, root, node = off_struct, None, None
    while True:
        token = struct.unpack_from(">I", data, at)[0]
        at += 4
        if token == FDT_BEGIN_NODE:
            end = data.index(b"\0", at)
            name = data[at:end].decode()
            at = (end + 4) & ~3
            new = Node(name, node)
            if node is None:
                root = new
            else:
                node.children.append(new)
            node = new
        elif token == FDT_END_NODE:
            node = node.parent
        elif token == FDT_PROP:
            length, name_off = struct.unpack_from(">2I", data, at)
            at += 8
            name = data[off_strings + name_off:data.index(b"\0", off_strings + name_off)].decode()
            node.props[name] = data[at:at + length]
            at = (at + length + 3) & ~3
        elif token == FDT_NOP:
            continue
        elif token == FDT_END:
            return Tree(root)
        else:
            raise ValueError(f"bad token {token:#x} at {at - 4:#x}")


class Tree:
    def __init__(self, root):
        self.root = root
        self.by_phandle = {n.u32s("phandle")[0]: n for n in root.walk() if "phandle" in n.props}

    def find(self, compatible):
        return [n for n in self.root.walk() if "compatible" in n.props and compatible in n.strings("compatible")]

    def phandle(self, value):
        return self.by_phandle[value]
