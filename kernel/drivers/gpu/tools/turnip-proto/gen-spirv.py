#!/usr/bin/env python3
"""The two shaders of tu-capass.c as SPIR-V words, written without glslang (none on the host).

  vertex:   in vec2 pos (location 0); out vec2 uv (location 0) = pos * 0.5 + 0.5; Position = vec4(pos, 0, 1)
  fragment: in vec2 uv; push constant vec4 c; set 0 binding 0 sampler2D t; out vec4 colour = texture(t, uv) * c

CoreAnimation's layer draw in miniature: a textured quad tinted by a per-draw constant (setFragmentBytes).
Usage: gen-spirv.py OUT.h  (C arrays vs_spv[] and fs_spv[])
"""
import struct
import sys


class Module:
    def __init__(self):
        self.next_id = 1
        self.words = []

    def id(self):
        self.next_id += 1
        return self.next_id - 1

    def op(self, opcode, *operands):
        flat = []
        for o in operands:
            if isinstance(o, str):
                b = o.encode() + b"\0"
                b += b"\0" * (-len(b) % 4)
                flat += list(struct.unpack("<%dI" % (len(b) // 4), b))
            elif isinstance(o, (list, tuple)):
                flat += list(o)
            else:
                flat.append(o)
        self.words += [((len(flat) + 1) << 16) | opcode] + flat

    def binary(self):
        return [0x07230203, 0x00010000, 0, self.next_id, 0] + self.words


# opcodes and enums used below (SPIR-V 1.0 spec)
CAPABILITY, MEMORY_MODEL, ENTRY_POINT, EXECUTION_MODE = 17, 14, 15, 16
DECORATE, MEMBER_DECORATE = 71, 72
T_VOID, T_FUNCTION, T_FLOAT, T_VECTOR, T_POINTER, T_STRUCT, T_INT, T_IMAGE, T_SAMPLED_IMAGE = 19, 33, 22, 23, 32, 30, 21, 25, 27
CONSTANT, CONSTANT_COMPOSITE, VARIABLE = 43, 44, 59
FUNCTION, LABEL, LOAD, STORE, RETURN, FUNCTION_END = 54, 248, 61, 62, 253, 56
COMPOSITE_CONSTRUCT, COMPOSITE_EXTRACT, ACCESS_CHAIN = 80, 81, 65
SAMPLE_IMPLICIT_LOD, F_MUL, F_ADD, VECTOR_TIMES_SCALAR = 87, 133, 129, 142
INPUT, OUTPUT, PUSH_CONSTANT, UNIFORM_CONSTANT = 1, 3, 9, 0
LOCATION, BUILTIN, BLOCK, DESCRIPTOR_SET, BINDING, OFFSET = 30, 11, 2, 34, 33, 35
POSITION = 0


def f32(v):
    return struct.unpack("<I", struct.pack("<f", v))[0]


def vertex():
    m = Module()
    void, fnv, f, v2, v4 = m.id(), m.id(), m.id(), m.id(), m.id()
    p_in2, p_out2, p_out4 = m.id(), m.id(), m.id()
    pos, uv, position = m.id(), m.id(), m.id()
    half, zero, one, half2 = m.id(), m.id(), m.id(), m.id()
    main, label = m.id(), m.id()
    m.op(CAPABILITY, 1)
    m.op(MEMORY_MODEL, 0, 1)
    m.op(ENTRY_POINT, 0, main, "main", pos, uv, position)
    m.op(DECORATE, pos, LOCATION, 0)
    m.op(DECORATE, uv, LOCATION, 0)
    m.op(DECORATE, position, BUILTIN, POSITION)
    m.op(T_VOID, void)
    m.op(T_FUNCTION, fnv, void)
    m.op(T_FLOAT, f, 32)
    m.op(T_VECTOR, v2, f, 2)
    m.op(T_VECTOR, v4, f, 4)
    m.op(T_POINTER, p_in2, INPUT, v2)
    m.op(T_POINTER, p_out2, OUTPUT, v2)
    m.op(T_POINTER, p_out4, OUTPUT, v4)
    m.op(VARIABLE, p_in2, pos, INPUT)
    m.op(VARIABLE, p_out2, uv, OUTPUT)
    m.op(VARIABLE, p_out4, position, OUTPUT)
    m.op(CONSTANT, f, half, f32(0.5))
    m.op(CONSTANT, f, zero, f32(0.0))
    m.op(CONSTANT, f, one, f32(1.0))
    m.op(CONSTANT_COMPOSITE, v2, half2, half, half)
    m.op(FUNCTION, void, main, 0, fnv)
    m.op(LABEL, label)
    p, scaled, shifted, x, y, out = m.id(), m.id(), m.id(), m.id(), m.id(), m.id()
    m.op(LOAD, v2, p, pos)
    m.op(VECTOR_TIMES_SCALAR, v2, scaled, p, half)
    m.op(F_ADD, v2, shifted, scaled, half2)
    m.op(STORE, uv, shifted)
    m.op(COMPOSITE_EXTRACT, f, x, p, 0)
    m.op(COMPOSITE_EXTRACT, f, y, p, 1)
    m.op(COMPOSITE_CONSTRUCT, v4, out, x, y, zero, one)
    m.op(STORE, position, out)
    m.op(RETURN)
    m.op(FUNCTION_END)
    return m.binary()


def fragment():
    m = Module()
    void, fnv, f, v2, v4, u32 = m.id(), m.id(), m.id(), m.id(), m.id(), m.id()
    img, simg, block = m.id(), m.id(), m.id()
    p_in2, p_out4, p_pc, p_pc4, p_tex = m.id(), m.id(), m.id(), m.id(), m.id()
    uv, colour, pc, tex = m.id(), m.id(), m.id(), m.id()
    zero_u = m.id()
    main, label = m.id(), m.id()
    m.op(CAPABILITY, 1)
    m.op(MEMORY_MODEL, 0, 1)
    m.op(ENTRY_POINT, 4, main, "main", uv, colour)
    m.op(EXECUTION_MODE, main, 7)  # OriginUpperLeft
    m.op(DECORATE, uv, LOCATION, 0)
    m.op(DECORATE, colour, LOCATION, 0)
    m.op(DECORATE, block, BLOCK)
    m.op(MEMBER_DECORATE, block, 0, OFFSET, 0)
    m.op(DECORATE, tex, DESCRIPTOR_SET, 0)
    m.op(DECORATE, tex, BINDING, 0)
    m.op(T_VOID, void)
    m.op(T_FUNCTION, fnv, void)
    m.op(T_FLOAT, f, 32)
    m.op(T_VECTOR, v2, f, 2)
    m.op(T_VECTOR, v4, f, 4)
    m.op(T_INT, u32, 32, 0)
    m.op(T_IMAGE, img, f, 1, 0, 0, 0, 1, 0)  # 2D, not depth, not arrayed, single-sampled, sampled, Unknown format
    m.op(T_SAMPLED_IMAGE, simg, img)
    m.op(T_STRUCT, block, v4)
    m.op(T_POINTER, p_in2, INPUT, v2)
    m.op(T_POINTER, p_out4, OUTPUT, v4)
    m.op(T_POINTER, p_pc, PUSH_CONSTANT, block)
    m.op(T_POINTER, p_pc4, PUSH_CONSTANT, v4)
    m.op(T_POINTER, p_tex, UNIFORM_CONSTANT, simg)
    m.op(VARIABLE, p_in2, uv, INPUT)
    m.op(VARIABLE, p_out4, colour, OUTPUT)
    m.op(VARIABLE, p_pc, pc, PUSH_CONSTANT)
    m.op(VARIABLE, p_tex, tex, UNIFORM_CONSTANT)
    m.op(CONSTANT, u32, zero_u, 0)
    m.op(FUNCTION, void, main, 0, fnv)
    m.op(LABEL, label)
    t, coord, texel, cptr, c, out = m.id(), m.id(), m.id(), m.id(), m.id(), m.id()
    m.op(LOAD, simg, t, tex)
    m.op(LOAD, v2, coord, uv)
    m.op(SAMPLE_IMPLICIT_LOD, v4, texel, t, coord)
    m.op(ACCESS_CHAIN, p_pc4, cptr, pc, zero_u)
    m.op(LOAD, v4, c, cptr)
    m.op(F_MUL, v4, out, texel, c)
    m.op(STORE, colour, out)
    m.op(RETURN)
    m.op(FUNCTION_END)
    return m.binary()


def fragment_fetch():
    """colour = texture(t, uv) * c + subpassLoad(fb) * (1 - c.a): CoreAnimation's blend done in the shader from
    [[color(0)]] (fc_framebuffer_fetch), as an input attachment at set 0 binding 1."""
    m = Module()
    void, fnv, f, v2, v4, u32, i32, iv2 = m.id(), m.id(), m.id(), m.id(), m.id(), m.id(), m.id(), m.id()
    img, simg, sub, block = m.id(), m.id(), m.id(), m.id()
    p_in2, p_out4, p_pc, p_pc4, p_tex, p_sub = m.id(), m.id(), m.id(), m.id(), m.id(), m.id()
    uv, colour, pc, tex, fb = m.id(), m.id(), m.id(), m.id(), m.id()
    zero_u, zero_i, origin, one = m.id(), m.id(), m.id(), m.id()
    main, label = m.id(), m.id()
    m.op(CAPABILITY, 1)
    m.op(CAPABILITY, 40)  # InputAttachment
    m.op(MEMORY_MODEL, 0, 1)
    m.op(ENTRY_POINT, 4, main, "main", uv, colour)
    m.op(EXECUTION_MODE, main, 7)
    m.op(DECORATE, uv, LOCATION, 0)
    m.op(DECORATE, colour, LOCATION, 0)
    m.op(DECORATE, block, BLOCK)
    m.op(MEMBER_DECORATE, block, 0, OFFSET, 0)
    m.op(DECORATE, tex, DESCRIPTOR_SET, 0)
    m.op(DECORATE, tex, BINDING, 0)
    m.op(DECORATE, fb, DESCRIPTOR_SET, 0)
    m.op(DECORATE, fb, BINDING, 1)
    m.op(DECORATE, fb, 43, 0)  # InputAttachmentIndex 0
    m.op(T_VOID, void)
    m.op(T_FUNCTION, fnv, void)
    m.op(T_FLOAT, f, 32)
    m.op(T_VECTOR, v2, f, 2)
    m.op(T_VECTOR, v4, f, 4)
    m.op(T_INT, u32, 32, 0)
    m.op(T_INT, i32, 32, 1)
    m.op(T_VECTOR, iv2, i32, 2)
    m.op(T_IMAGE, img, f, 1, 0, 0, 0, 1, 0)
    m.op(T_SAMPLED_IMAGE, simg, img)
    m.op(T_IMAGE, sub, f, 6, 0, 0, 0, 2, 0)  # SubpassData, read without a sampler
    m.op(T_STRUCT, block, v4)
    m.op(T_POINTER, p_in2, INPUT, v2)
    m.op(T_POINTER, p_out4, OUTPUT, v4)
    m.op(T_POINTER, p_pc, PUSH_CONSTANT, block)
    m.op(T_POINTER, p_pc4, PUSH_CONSTANT, v4)
    m.op(T_POINTER, p_tex, UNIFORM_CONSTANT, simg)
    m.op(T_POINTER, p_sub, UNIFORM_CONSTANT, sub)
    m.op(VARIABLE, p_in2, uv, INPUT)
    m.op(VARIABLE, p_out4, colour, OUTPUT)
    m.op(VARIABLE, p_pc, pc, PUSH_CONSTANT)
    m.op(VARIABLE, p_tex, tex, UNIFORM_CONSTANT)
    m.op(VARIABLE, p_sub, fb, UNIFORM_CONSTANT)
    m.op(CONSTANT, u32, zero_u, 0)
    m.op(CONSTANT, i32, zero_i, 0)
    m.op(CONSTANT_COMPOSITE, iv2, origin, zero_i, zero_i)
    m.op(CONSTANT, f, one, f32(1.0))
    m.op(FUNCTION, void, main, 0, fnv)
    m.op(LABEL, label)
    t, coord, texel, cptr, c, src = m.id(), m.id(), m.id(), m.id(), m.id(), m.id()
    s, dst, a, inv, under, out = m.id(), m.id(), m.id(), m.id(), m.id(), m.id()
    m.op(LOAD, simg, t, tex)
    m.op(LOAD, v2, coord, uv)
    m.op(SAMPLE_IMPLICIT_LOD, v4, texel, t, coord)
    m.op(ACCESS_CHAIN, p_pc4, cptr, pc, zero_u)
    m.op(LOAD, v4, c, cptr)
    m.op(F_MUL, v4, src, texel, c)
    m.op(LOAD, sub, s, fb)
    m.op(98, v4, dst, s, origin)  # OpImageRead
    m.op(COMPOSITE_EXTRACT, f, a, src, 3)
    m.op(131, f, inv, one, a)  # OpFSub
    m.op(VECTOR_TIMES_SCALAR, v4, under, dst, inv)
    m.op(F_ADD, v4, out, src, under)
    m.op(STORE, colour, out)
    m.op(RETURN)
    m.op(FUNCTION_END)
    return m.binary()


def c_array(name, words):
    body = ",\n".join("  " + ", ".join("0x%08x" % w for w in words[i:i + 8]) for i in range(0, len(words), 8))
    return "static const uint32_t %s[] = {\n%s\n};\n" % (name, body)


if __name__ == "__main__":
    with open(sys.argv[1], "w") as out:
        out.write("/* generated by gen-spirv.py */\n#include <stdint.h>\n")
        out.write(c_array("vs_spv", vertex()))
        out.write(c_array("fs_spv", fragment()))
        out.write(c_array("fs_fetch_spv", fragment_fetch()))
