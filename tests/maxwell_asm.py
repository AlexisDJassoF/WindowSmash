"""A few Maxwell instructions, encoded as runtime/gpu_shader.c decodes them, for tests that need
whole programs (the 0x50-byte header, then code in groups of three after a scheduling word).

    prog = assemble([("ald", 0, 0x80, 4), ("isetp", 0, 0, 1, "lt"), ("bra", "A", "P0"), ...,
                     ("A:",), ("exit",)])

Operations: mov32i d imm | iadd32i d a imm | isetp p a b cmp | ald d offset count | ast d offset count
| bra label [pred] | ssy label | pbk label | sync [pred] | brk [pred] | exit [pred] | brx reg label...
| ipa d attribute (a pixel shader's input, e.g. 0x70 / 0x74 the window position)
| tlds d a b handle encoding (a texel load: a, b the coordinates' registers, as gpu_shader.c's emit_tlds)
(a BRX's table is not encoded: a jump table comes from a constant buffer). A predicate is "P0".."P6"
or "!P0"; none means PT. A label is a 1-tuple ending in ':'."""
import struct

PT = 7
CMP = {"f": 0, "lt": 1, "eq": 2, "le": 3, "gt": 4, "ne": 5, "ge": 6, "t": 7}


def top(pattern):
    return int(pattern.replace("-", "0"), 2) << 48


TOPS = {
    "bra": top("111000100100----"), "ssy": top("111000101001----"), "pbk": top("111000101010----"),
    "sync": top("1111000011111---"), "brk": top("111000110100----"), "exit": top("111000110000----"),
    "brx": top("111000100101----"), "mov32i": top("000000010000----"), "iadd32i": top("0001110---------"),
    "isetp": top("010110110110----"), "ald": top("1110111111011---"), "ast": top("1110111111110---"),
    "ipa": top("11100000--------"), "tlds": top("1101-01---------"),
}


def pred(p):
    if not p:
        return PT << 16
    neg = p.startswith("!")
    return (int(p.lstrip("!")[1:]) | (8 if neg else 0)) << 16


def address(k):
    """The address of the k-th instruction: every fourth word is scheduling control."""
    return 0x50 + 8 * (k + k // 3 + 1)


def assemble(lines, header=b""):
    labels, k = {}, 0
    for line in lines:
        if line[0].endswith(":"):
            labels[line[0][:-1]] = address(k)
        else:
            k += 1
    words, k = [], 0
    for line in lines:
        if line[0].endswith(":"):
            continue
        op, args, here = line[0], line[1:], address(k)
        w = TOPS[op]
        if op in ("bra", "ssy", "pbk"):
            w |= ((labels[args[0]] - here - 8) & 0xFFFFFF) << 20 | 15
            w |= pred(args[1] if len(args) > 1 else None) if op == "bra" else PT << 16
        elif op in ("sync", "brk", "exit"):
            w |= pred(args[0] if args else None) | 15
        elif op == "brx":
            w |= args[0] << 8 | PT << 16 | 15
        elif op == "mov32i":
            w |= args[0] | (args[1] & 0xFFFFFFFF) << 20 | PT << 16
        elif op == "iadd32i":
            w |= args[0] | args[1] << 8 | (args[2] & 0xFFFFFFFF) << 20 | PT << 16
        elif op == "isetp":   # P<p> = a <cmp> b (signed), the second destination PT
            w |= PT | args[0] << 3 | args[1] << 8 | args[2] << 20 | PT << 39 | 1 << 48 | CMP[args[3]] << 49 | PT << 16
        elif op == "ipa":   # d, attribute byte offset; pass-through mode, unsaturated, A = RZ
            w |= args[0] | 0xFF << 8 | (args[1] // 4) << 30 | PT << 16
        elif op == "tlds":   # d (two registers), a, b, handle byte offset, encoding; 32-bit results, all four components
            w |= args[0] | args[1] << 8 | args[2] << 20 | 0xFF << 28 | (args[3] // 4) << 36 | 4 << 50 | args[4] << 53 | 1 << 59 | PT << 16
        elif op in ("ald", "ast"):
            w |= args[0] | 0xFF << 8 | args[1] << 20 | (args[2] - 1) << 47 | PT << 16
        words.append(w)
        k += 1
    code = bytearray(header.ljust(0x50, b"\0"))
    for i, w in enumerate(words):
        if i % 3 == 0:
            code += struct.pack("<Q", 0)   # scheduling control
        code += struct.pack("<Q", w)
    if (len(code) - 0x50) // 8 % 4 == 0:
        code += struct.pack("<Q", 0)
    return bytes(code + struct.pack("<Q", 0xE2400FFFFF87000F))   # the end: 'BRA to self'
