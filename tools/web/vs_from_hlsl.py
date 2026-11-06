"""Vertex program microcode back from the HLSL runtime/gpu/shaders.cpp generated for it (D3D12 shader captures).

The capture keeps the generated HLSL, not the microcode. VertexProgramHlsl spells every field it decodes (opcodes,
operands with swizzle, negation, register file and relative addressing, write masks, outputs), so the words can be
rebuilt from the text; tests/wgsl_vertex_test.cpp then regenerates the HLSL from them (it must match the capture
exactly, which proves the words) and the WGSL beside it.

    python tools/web/vs_from_hlsl.py <captures dir> <out dir>

writes <out dir>/programs.txt (one line per distinct program: name regs decode words...) and <out dir>/<name>.body
(the capture's instruction blocks). The words come from the game's shaders: keep the output local.
"""
import pathlib
import re
import sys

MASK = ["", "w", "z", "zw", "y", "yw", "yz", "yzw", "x", "xw", "xz", "xzw", "xy", "xyw", "xyz", "xyzw"]
OUT = {"oPos": 0, "oD0": 3, "oD1": 4, "oFog": 5, "oPts": 6, "oB0": 7, "oB1": 8, "oT0": 9, "oT1": 10, "oT2": 11, "oT3": 12}
LANES_MASK = {"x": 8, "xy": 12, "xyz": 14, "xyzw": 15}  # oFog: one lane per mask bit, from x
PROLOGUE_END = "oT3 = float4(0, 0, 0, 1);\n"
EPILOGUE = "\n    VOut o;"
OPERAND = re.compile(r"\((-?)\((R\d+|oPos|i\.v\d+|vattr\[\d+\]|c\[\d+\]|c\[clamp\(a0 \+ \d+, 0, 191\)\])\)\.([xyzw]{4})\)")
DECODES = [  # VsInputs' lines per VertexDecode
    (1, "i.v{r} = float4(ri.v{r}.xyz, 1);"),
    (2, "i.v{r} = float4(ri.v{r});"),
    (3, "i.v{r} = float4(float3(ri.v{r}.xyz), 1);"),
    (4, "i.v{r} = UnpackNp3(ri.v{r});"),
    (5, "i.v{r} = float4(ri.v{r}.xy, 0, ri.v{r}.z);"),
]


def mac_expr(op, a, b, c):  # shaders.cpp MacExpr
    return {
        1: a, 2: f"mac_mul({a}, {b})", 3: f"({a} + {c})", 4: f"(mac_mul({a}, {b}) + {c})",
        5: f"dot({a}.xyz, {b}.xyz).xxxx", 6: f"dot(float4({a}.xyz, 1), {b}).xxxx", 7: f"dot({a}, {b}).xxxx",
        8: f"float4(1, {a}.y * {b}.y, {a}.z, {b}.w)", 9: f"min({a}, {b})", 10: f"max({a}, {b})",
        11: f"(float4)({a} < {b})", 12: f"(float4)({a} >= {b})",
    }[op]


ILU = {"ilu_rcp": 2, "ilu_rcc": 3, "ilu_rsq": 4, "ilu_exp": 5, "ilu_log": 6}


class Words:
    def __init__(self):
        self.t = [0, 0, 0, 0]

    def set(self, sub, bit, length, value):
        assert 0 <= value < (1 << length), (sub, bit, length, value)
        self.t[sub] &= ~(((1 << length) - 1) << bit)
        self.t[sub] |= value << bit

    def operand(self, which, text):
        m = OPERAND.fullmatch(text)
        assert m, text
        neg, base, sw = m.group(1) == "-", m.group(2), ["xyzw".index(ch) for ch in m.group(3)]
        if base == "oPos":
            mux, reg = 1, 12
        elif base[0] == "R":
            mux, reg = 1, int(base[1:])
        elif base.startswith("i.v") or base.startswith("vattr"):
            mux, reg = 2, 0
            self.set(1, 9, 4, int(re.search(r"\d+", base).group()))
        else:
            mux, reg = 3, 0
            self.set(1, 13, 8, int(re.search(r"(\d+)(?:, 0, 191\))?\]$", base).group(1)))
            if "clamp" in base:
                self.set(3, 1, 1, 1)
        if which == 0:
            self.set(1, 8, 1, neg)
            for i, s in enumerate(sw):
                self.set(1, 6 - 2 * i, 2, s)
            self.set(2, 28, 4, reg)
            self.set(2, 26, 2, mux)
        elif which == 1:
            self.set(2, 25, 1, neg)
            for i, s in enumerate(sw):
                self.set(2, 23 - 2 * i, 2, s)
            self.set(2, 13, 4, reg)
            self.set(2, 11, 2, mux)
        else:
            self.set(2, 10, 1, neg)
            for i, s in enumerate(sw):
                self.set(2, 8 - 2 * i, 2, s)
            self.set(2, 0, 2, reg >> 2)
            self.set(3, 30, 2, reg & 3)
            self.set(3, 28, 2, mux)


def mac_of(expr):
    ops = OPERAND.findall(expr)
    texts = [f"({n}({b}).{s})" for n, b, s in ops]
    uniq = list(dict.fromkeys(texts))
    zero = "((R0).xyzw)"
    for op in range(1, 13):
        if op in (3,):
            cands = [(texts[0], zero, texts[1])] if len(texts) == 2 else []
        elif op == 4:
            cands = [tuple(texts)] if len(texts) == 3 else []
        elif op == 1:
            cands = [(texts[0], zero, zero)] if len(texts) == 1 else []
        elif op == 8:
            cands = [(texts[0], texts[1], zero)] if len(texts) == 4 else []
        else:
            cands = [(texts[0], texts[1], zero)] if len(texts) == 2 else []
        for a, b, c in cands:
            if mac_expr(op, a, b, c) == expr:
                used = {1: "a", 2: "ab", 3: "ac", 4: "abc", 8: "ab"}.get(op, "ab")
                return op, {k: v for k, v in zip("abc", (a, b, c)) if k in used}
    raise ValueError("unknown MAC expression: " + expr)


def ilu_of(expr):
    m = re.fullmatch(r"(ilu_\w+)\((.*)\.x\)", expr)
    if m and m.group(1) in ILU:
        return ILU[m.group(1)], m.group(2)
    m = re.fullmatch(r"ilu_lit\((.*)\)", expr)
    if m:
        return 7, m.group(1)
    assert OPERAND.fullmatch(expr), expr
    return 1, expr


def encode_block(lines):
    w = Words()
    mac = ilu = 0
    writes = []
    for line in lines:
        line = line.strip()
        if line.startswith("precise float4 mr = "):
            expr = line[len("precise float4 mr = "):-1]
            mac, ops = mac_of(expr)
            for k, v in ops.items():
                w.operand("abc".index(k), v)
        elif line.startswith("precise float4 ir = "):
            ilu, c = ilu_of(line[len("precise float4 ir = "):-1])
            w.operand(2, c)
        elif line == "a0 = mac_arl(mr.x);":
            mac = 13
        else:
            m = re.fullmatch(r"(\w+)\.([xyzw]+) = (mr|ir)\.([xyzw]+);", line)
            assert m and m.group(2) == m.group(4), line
            writes.append((m.group(1), m.group(2), m.group(3)))
    w.set(1, 21, 4, mac)
    w.set(1, 25, 3, ilu)
    mac_temp = ilu_temp = False
    r = None
    for dest, lanes, src in writes:
        temp = dest[0] == "R" or (dest == "oPos" and (
            (src == "mr" and not mac_temp and not ilu_temp) or (src == "ir" and not mac and not ilu_temp)))
        if temp and src == "mr" and not mac_temp and not ilu_temp:
            r = 12 if dest == "oPos" else int(dest[1:])
            w.set(3, 24, 4, MASK.index(lanes))
            mac_temp = True
        elif temp and src == "ir" and not ilu_temp:
            reg = 12 if dest == "oPos" else int(dest[1:])
            if mac:
                assert reg == 1, writes
            else:
                r = reg
            w.set(3, 16, 4, MASK.index(lanes))
            ilu_temp = True
        else:
            o = OUT[dest]
            w.set(3, 12, 4, LANES_MASK[lanes] if o == 5 else MASK.index(lanes))
            w.set(3, 11, 1, 1)
            w.set(3, 3, 8, o)
            w.set(3, 2, 1, src == "ir")
    w.set(3, 20, 4, 0 if r is None else r)  # no temporary written: the masks are 0
    return w.t


def inputs(text):
    regs = decode = 0
    raw = "struct VRaw {" in text
    struct = text[text.index("struct VRaw {" if raw else "struct VIn {"):]
    struct = struct[:struct.index("};")]
    for r in map(int, re.findall(r" v(\d+) : ATTR", struct)):
        regs |= 1 << r
        for d, pattern in DECODES:
            if raw and pattern.format(r=r) in text:
                decode |= d << (4 * r)
    return regs, decode


def program(text):
    start = text.index(PROLOGUE_END, text.index("VOut main(")) + len(PROLOGUE_END)  # not PrevPos (velocity)
    body = text[start:text.index(EPILOGUE, start)]
    words = []
    for block in re.findall(r"    \{\n(.*?)    \}\n", body, re.S):
        words += encode_block(block.splitlines())
    if words:
        words[-1] |= 1  # last instruction
    return body, words


def main():
    src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    seen, lines = set(), []
    for f in sorted(src.glob("*/vs_*.hlsl")):
        if f.name in seen:
            continue
        text = f.read_text(encoding="utf-8")
        if "precise float4 mr =" not in text and "precise float4 ir =" not in text:
            continue  # fixed function
        seen.add(f.name)
        regs, decode = inputs(text)
        body, words = program(text)
        name = f.stem
        (out / (name + ".body")).write_text(body, encoding="utf-8", newline="")
        lines.append(" ".join([name, f"{regs:x}", f"{decode:x}"] + [f"{x:x}" for x in words]))
    (out / "programs.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(lines)} programs")


if __name__ == "__main__":
    main()
