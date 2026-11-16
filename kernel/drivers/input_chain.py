"""darwin-lisa: a driver's init call in the input chain (bsd/dev/hakios.input.c, hakios_input_init()), right after the
buttons' one, under the driver's option. Used by the torch/, haptic/ and bt/ wires. Idempotent: a call already there
(whatever else was inserted next to it since) is left alone."""
import dlisa_wire as w

INPUT = "bsd/dev/hakios.input.c"


def add_init_call(option, function):
    if f"\t{function}();\n" in w.read(w.path(INPUT)):
        print(f"  [skip] hakios.input.c: {function}(): already called")
        return
    w.edit(INPUT,
           "void\nhakios_input_init(void)\n{",
           f"#ifdef {option}\nvoid {function}(void);\n#endif\n\nvoid\nhakios_input_init(void)\n{{",
           f"hakios.input.c: {function} declared")
    w.edit(INPUT,
           "\thakios_buttons_init();\n",
           f"\thakios_buttons_init();\n#ifdef {option}\n\t{function}();\n#endif\n",
           f"hakios.input.c: {function}() called after the buttons'")
