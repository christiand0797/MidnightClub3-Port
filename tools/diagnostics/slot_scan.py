"""Find every instruction that addresses a per-player slot array of the race config or the in-race player manager.

Phase 2 of docs/notes/cruise_player_cap_resize_plan.md moves those arrays to the end of bigger allocations. Every
instruction that steps from a struct's base into an array (a displacement such as `+0x114`, an `add reg, 0x114`, or a
constant register such as `mov edi, 8` used as an offset) has to be patched with a [[patch]] entry. Instructions that
use a pointer already inside an array (the accessors' results, a loop pointer) need nothing.

Input is the recompiled code in templates/game/generated/game (the disassembly in its comments). The scan is a
per-function forward data flow over registers and stack slots, with function arguments, `this` (ecx) and return
values carried between functions until nothing changes.

Usage: python tools/diagnostics/slot_scan.py [--generated DIR] [--out sites.tsv] [--slots N] [--toml patches.toml]
"""
from __future__ import annotations

import argparse
import collections
import dataclasses
import pathlib
import re
import sys

# --- the structs -------------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Array:
    name: str
    start: int
    count: int
    stride: int

    @property
    def end(self) -> int:
        return self.start + self.count * self.stride


@dataclasses.dataclass(frozen=True)
class Struct:
    name: str
    size: int
    globals: tuple[int, ...]
    arrays: tuple[Array, ...]

    def array_at(self, off: int) -> Array | None:
        for a in self.arrays:
            if a.start <= off < a.end:
                return a
        return None


CONFIG = Struct("cfg", 0x3E0, (0x53F2E0, 0x53F2E4, 0x53F2E8),
                (Array("players", 0x114, 8, 0x2C), Array("players2", 0x274, 8, 0x2C)))
MANAGER = Struct("mgr", 0x74, (0x558814,), (Array("racers", 0x08, 8, 4),))
STRUCTS = (CONFIG, MANAGER)
GLOBAL_STRUCT = {g: s for s in STRUCTS for g in s.globals}

# --- abstract values ---------------------------------------------------------------------------------------------
# ("B", struct, off, var, origin)  pointer to struct + off; var: plus an unknown (index) amount; origin: where it came
#                                  from ("global", "param", "ret", "this", "may")
# ("C", value, origins)            a constant, with the instructions that set it
# None                             anything else


def B(s, off=0, var=False, origin="global"):
    return ("B", s.name, off, var, origin)


STRUCT_BY_NAME = {s.name: s for s in STRUCTS}
REGS = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp")
SUBREGS = {
    "al": "eax", "ah": "eax", "ax": "eax", "bl": "ebx", "bh": "ebx", "bx": "ebx", "cl": "ecx", "ch": "ecx",
    "cx": "ecx", "dl": "edx", "dh": "edx", "dx": "edx", "si": "esi", "di": "edi", "bp": "ebp",
}


def merge(a, b):
    if a == b:
        return a
    if a is None:
        return b if b and b[0] == "B" else None  # may: keep the pointer
    if b is None:
        return a if a[0] == "B" else None
    if a[0] == b[0] == "C":
        return ("C", None, a[2] | b[2])  # a loop counter: value unknown, origins kept
    if a[0] == b[0] == "B" and a[1] == b[1]:
        s = STRUCT_BY_NAME[a[1]]
        if a[2] == b[2]:
            return ("B", a[1], a[2], a[3] or b[3], a[4] if a[4] == b[4] else "may")
        ra, rb = s.array_at(a[2]), s.array_at(b[2])
        if ra is not None and ra == rb:
            return ("E", a[1])  # stepping inside one array: past the site
        return None
    if a[0] == b[0] == "E" and a[1] == b[1]:
        return a
    if {a[0], b[0]} == {"B", "E"} and a[1] == b[1]:
        e = a if a[0] == "E" else b
        p = b if a[0] == "E" else a
        if STRUCT_BY_NAME[p[1]].array_at(p[2]):
            return e
    return None


# --- parsing -----------------------------------------------------------------------------------------------------

FUNC_RE = re.compile(r"^X86_FUNC\(__imp__sub_([0-9A-F]{8})\) \{")
INSN_RE = re.compile(r"^\t// ([0-9A-F]{8}) (.*)$")
LABEL_RE = re.compile(r"^loc_([0-9A-F]{8}):")


@dataclasses.dataclass
class Insn:
    va: int
    text: str
    mn: str
    ops: list[str]
    targets: list[int] | None = None  # an indirect jmp's case targets, as the generated switch lists them


CASE_RE = re.compile(r"^\tcase 0x([0-9A-Fa-f]+)u: goto loc_")


def split_ops(s: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def parse_insn(va: int, text: str) -> Insn:
    parts = text.split(" ", 1)
    mn = parts[0]
    rest = parts[1] if len(parts) > 1 else ""
    if mn in ("rep", "repe", "repne", "lock") and rest:
        sub = rest.split(" ", 1)
        mn = mn + " " + sub[0]
        rest = sub[1] if len(sub) > 1 else ""
    return Insn(va, text, mn, split_ops(rest))


def load_functions(gen: pathlib.Path) -> dict[int, list[Insn]]:
    funcs: dict[int, list[Insn]] = {}
    for path in sorted(gen.glob("x86_recomp.*.cpp")):
        cur = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = FUNC_RE.match(line)
                if m:
                    cur = funcs.setdefault(int(m.group(1), 16), [])
                    continue
                if cur is None:
                    continue
                if line.startswith("}"):
                    cur = None
                    continue
                m = INSN_RE.match(line)
                if m and not m.group(2).startswith("patched, was:"):
                    cur.append(parse_insn(int(m.group(1), 16), m.group(2)))
                    continue
                m = CASE_RE.match(line)
                if m and cur and cur[-1].mn == "jmp" and imm(cur[-1].ops[0]) is None:
                    if cur[-1].targets is None:
                        cur[-1].targets = []
                    cur[-1].targets.append(int(m.group(1), 16))
    return funcs


MEM_RE = re.compile(r"^(?:(?:byte|word|dword|qword|tbyte|xmmword|fword) ptr )?(?:(\w\w):)?\[([^\]]*)\]$")


def parse_mem(op: str):
    """[base+index*scale+disp] -> (terms [(reg, scale)], disp) or None when not memory."""
    m = MEM_RE.match(op)
    if not m:
        return None
    if m.group(1) == "fs":
        return ([], None)
    expr = m.group(2).replace("-", "+-")
    terms, disp = [], 0
    for t in expr.split("+"):
        t = t.strip()
        if not t:
            continue
        neg = t.startswith("-")
        t = t.lstrip("-")
        if "*" in t:
            r, sc = t.split("*")
            terms.append((r, int(sc)))
        elif re.fullmatch(r"0x[0-9A-Fa-f]+|\d+", t):
            v = int(t, 0)
            disp += -v if neg else v
        else:
            terms.append((t, 1))
    return terms, disp


def imm(op: str):
    if re.fullmatch(r"-?0x[0-9A-Fa-f]+|-?\d+", op):
        return int(op, 0) & 0xFFFFFFFF
    return None


def signed(v: int) -> int:
    return v - (1 << 32) if v & 0x80000000 else v


# --- the scan ----------------------------------------------------------------------------------------------------


@dataclasses.dataclass
class Site:
    va: int
    func: int
    kind: str      # disp | add | const | copy
    struct: str
    array: str
    offset: int    # struct-relative offset the instruction lands on
    disp: int      # the operand to patch (displacement or immediate)
    origin: str
    text: str


class State:
    __slots__ = ("regs", "slots", "esp", "ebp")

    def __init__(self):
        self.regs = {}
        self.slots = {}
        self.esp = 0       # esp relative to the entry esp (the return address)
        self.ebp = None    # ebp relative to the entry esp, when ebp is a frame pointer

    def copy(self):
        s = State()
        s.regs = dict(self.regs)
        s.slots = dict(self.slots)
        s.esp, s.ebp = self.esp, self.ebp
        return s

    def key(self):
        return (tuple(sorted(self.regs.items())), tuple(sorted(self.slots.items())), self.esp, self.ebp)


def merge_states(a: State | None, b: State) -> State:
    if a is None:
        return b.copy()
    s = State()
    for r in set(a.regs) | set(b.regs):
        v = merge(a.regs.get(r), b.regs.get(r))
        if v is not None:
            s.regs[r] = v
    for k in set(a.slots) | set(b.slots):
        v = merge(a.slots.get(k), b.slots.get(k))
        if v is not None:
            s.slots[k] = v
    s.esp = a.esp if a.esp == b.esp else a.esp
    s.ebp = a.ebp if a.ebp == b.ebp else None
    return s


class Scanner:
    def __init__(self, funcs: dict[int, list[Insn]]):
        self.funcs = funcs
        self.pops = {f: self._pops(body) for f, body in funcs.items()}
        self.params: dict[int, dict] = collections.defaultdict(dict)   # func -> {slot or "ecx": value}
        self.returns: dict[int, tuple] = {}
        self.sites: dict[tuple, Site] = {}
        self.notes: dict[int, set] = collections.defaultdict(set)
        self.seeds: dict[int, Struct] = {}   # constructors found from stores of their result into a struct global

    @staticmethod
    def _pops(body):
        for i in body:
            if i.mn == "ret":
                return imm(i.ops[0]) if i.ops else 0
        return 0

    # value of an operand
    def value(self, st: State, op: str, insn: Insn):
        if op in REGS:
            return st.regs.get(op)
        if op == "esp":
            return None
        v = imm(op)
        if v is not None:
            return ("C", v, frozenset([insn.va]))
        mem = parse_mem(op)
        if mem is not None:
            terms, disp = mem
            if disp is None:
                return None
            if not terms and disp in GLOBAL_STRUCT:
                return B(GLOBAL_STRUCT[disp])
            slot = self.slot_of(st, terms, disp)
            if slot is not None:
                return st.slots.get(slot)
        return None

    @staticmethod
    def slot_of(st: State, terms, disp):
        if len(terms) == 1 and terms[0][1] == 1:
            if terms[0][0] == "esp":
                return st.esp + disp
            if terms[0][0] == "ebp" and st.ebp is not None:
                return st.ebp + disp
        return None

    # the address a memory operand or lea computes, with site detection
    def address(self, st: State, terms, disp, insn: Insn, func: int, record=True):
        base = None
        var = False
        consts = []
        for r, sc in terms:
            v = st.regs.get(r) if r in REGS else None
            if v and v[0] == "B" and sc == 1 and base is None:
                base = v
            elif v and v[0] == "C" and v[1] is not None:
                consts.append((v[1] * sc, v[2]))
            elif v and v[0] == "C":
                consts.append((None, v[2]))
                var = True
            elif v and v[0] in ("B", "E"):
                return None  # two struct pointers or a scaled pointer: not an address into one struct
            else:
                var = True
        if base is None:
            return None
        s = STRUCT_BY_NAME[base[1]]
        off = base[2]
        if s.array_at(off):
            return ("E", s.name)
        total = off + signed(disp)
        const_origins = set()
        unknown_const = False
        for k, origins in consts:
            if k is None:
                unknown_const = True
            else:
                total += signed(k)
            const_origins |= set(origins)
        arr = s.array_at(total)
        if record and arr is not None and not s.array_at(off + signed(disp)) and const_origins:
            for o in const_origins:
                self.add_site(Site(o, func, "const", s.name, arr.name, total, total - off, base[4], insn.text))
        elif record and arr is not None:
            self.add_site(Site(insn.va, func, "disp", s.name, arr.name, total, signed(disp), base[4], insn.text))
        elif record and unknown_const and const_origins:
            # a loop register stepping from a constant: check the constant itself
            for o in const_origins:
                self.notes[func].add(f"loop register from {o:08X} used with {s.name} at {insn.va:08X}")
        if arr is not None:
            return ("E", s.name)
        return ("B", s.name, total, base[3] or var or unknown_const, base[4])

    def add_site(self, site: Site):
        self.sites.setdefault((site.va, site.kind), site)

    def write(self, st: State, op: str, v):
        if op in REGS:
            if v is None:
                st.regs.pop(op, None)
            else:
                st.regs[op] = v
        elif op in SUBREGS:
            st.regs.pop(SUBREGS[op], None)
        elif op == "esp":
            pass
        else:
            mem = parse_mem(op)
            if mem and mem[1] is not None:
                slot = self.slot_of(st, mem[0], mem[1])
                if slot is not None:
                    if v is None:
                        st.slots.pop(slot, None)
                    else:
                        st.slots[slot] = v

    def check_mem_ops(self, st: State, insn: Insn, func: int):
        for op in insn.ops:
            mem = parse_mem(op)
            if mem and mem[1] is not None and mem[0]:
                if insn.mn == "lea":
                    continue
                self.address(st, mem[0], mem[1], insn, func)

    def step(self, st: State, insn: Insn, func: int, calls: list):
        mn, ops = insn.mn, insn.ops
        self.check_mem_ops(st, insn, func)
        if mn == "mov" and len(ops) == 2 and ops == ["ebp", "esp"]:
            st.ebp = st.esp
            st.regs.pop("ebp", None)
        elif mn == "mov" and len(ops) == 2 and ops == ["esp", "ebp"]:
            if st.ebp is not None:
                st.esp = st.ebp
        elif mn == "mov" and len(ops) == 2:
            if ops[0] == "ebp":
                st.ebp = None
            v = self.value(st, ops[1], insn)
            dst = parse_mem(ops[0])
            if v and v[0] == "R" and dst and not dst[0] and dst[1] in GLOBAL_STRUCT:
                # a struct's global set to a function's result: that function is its constructor, `this` the struct
                self.seeds[v[1]] = GLOBAL_STRUCT[dst[1]]
            self.write(st, ops[0], v)
        elif mn == "lea":
            mem = parse_mem(ops[1])
            v = self.address(st, mem[0], mem[1], insn, func) if mem and mem[1] is not None else None
            if v is None and mem and mem[1] is not None and len(mem[0]) == 1 and mem[0][0][1] == 1:
                src = st.regs.get(mem[0][0][0])
                if src and src[0] == "C" and src[1] is not None:
                    v = ("C", (src[1] + mem[1]) & 0xFFFFFFFF, src[2])
            if mem and ops[0] == "esp":
                slot = self.slot_of(st, mem[0], mem[1])
                if slot is not None:
                    st.esp = slot
            elif mem and ops[0] == "ebp":
                slot = self.slot_of(st, mem[0], mem[1])
                st.ebp = slot
                self.write(st, "ebp", v)
            else:
                self.write(st, ops[0], v)
        elif mn in ("add", "sub") and len(ops) == 2:
            dst = ops[0]
            k = imm(ops[1])
            if dst == "esp":
                if k is not None:
                    st.esp += signed(k) if mn == "add" else -signed(k)
                return
            a = self.value(st, dst, insn)
            b = self.value(st, ops[1], insn)
            v = None
            if a and a[0] == "B" and b and b[0] == "C" and b[1] is not None:
                delta = signed(b[1]) if mn == "add" else -signed(b[1])
                s = STRUCT_BY_NAME[a[1]]
                total = a[2] + delta
                arr = s.array_at(total)
                if arr is not None and not s.array_at(a[2]):
                    kind = "add" if k is not None else "const"
                    origins = [insn.va] if k is not None else sorted(b[2])
                    for o in origins:
                        self.add_site(Site(o, func, kind, s.name, arr.name, total, delta, a[4], insn.text))
                    v = ("E", s.name)
                elif s.array_at(a[2]):
                    v = ("E", s.name)
                else:
                    v = ("B", a[1], total, a[3], a[4])
            elif a and a[0] == "B" and mn == "add" and (b is None or b[0] != "B"):
                if b and b[0] == "C":
                    v = ("B", a[1], a[2], True, a[4])  # loop register added: offset checked at the use
                    self.notes[func].add(f"{dst} = {a[1]} + loop register at {insn.va:08X}")
                else:
                    v = ("B", a[1], a[2], True, a[4])
            elif b and b[0] == "B" and mn == "add" and (a is None or a[0] == "C"):
                v = ("B", b[1], b[2], True, b[4])
                if a and a[0] == "C" and a[1] is not None:
                    s = STRUCT_BY_NAME[b[1]]
                    total = b[2] + signed(a[1])
                    if s.array_at(total) and not s.array_at(b[2]):
                        for o in sorted(a[2]):
                            self.add_site(Site(o, func, "const", s.name, s.array_at(total).name, total,
                                               signed(a[1]), b[4], insn.text))
                        v = ("E", s.name)
                    else:
                        v = ("B", b[1], total, b[3], b[4])
            elif a and a[0] == "E" and (b is None or b[0] == "C"):
                v = a
            elif a and a[0] == "C" and b and b[0] == "C" and a[1] is not None and b[1] is not None:
                v = ("C", (a[1] + b[1] if mn == "add" else a[1] - b[1]) & 0xFFFFFFFF, a[2] | b[2])
            elif a and a[0] == "C" and b and b[0] == "C":
                v = ("C", None, a[2] | b[2])
            self.write(st, dst, v)
        elif mn in ("inc", "dec") and ops:
            a = self.value(st, ops[0], insn)
            if a and a[0] == "C":
                v = ("C", None if a[1] is None else (a[1] + (1 if mn == "inc" else -1)) & 0xFFFFFFFF, a[2])
                self.write(st, ops[0], v)
            elif a and a[0] == "B":
                self.write(st, ops[0], ("B", a[1], a[2] + (1 if mn == "inc" else -1), a[3], a[4]))
            else:
                self.write(st, ops[0], None)
        elif mn == "xor" and len(ops) == 2 and ops[0] == ops[1]:
            self.write(st, ops[0], ("C", 0, frozenset([insn.va])))
        elif mn == "push" and ops:
            st.esp -= 4
            st.slots[st.esp] = self.value(st, ops[0], insn) if ops[0] != "esp" else None
            if st.slots[st.esp] is None:
                del st.slots[st.esp]
        elif mn == "pop" and ops:
            v = st.slots.get(st.esp)
            st.esp += 4
            if ops[0] == "esp":
                return
            self.write(st, ops[0], v)
        elif mn == "leave":
            if st.ebp is not None:
                st.esp = st.ebp
            st.esp += 4
            st.regs.pop("ebp", None)
            st.ebp = None
        elif mn == "call" and ops:
            t = imm(ops[0])
            args = {"ecx": st.regs.get("ecx")}
            for i in range(8):
                v = st.slots.get(st.esp + 4 * i)
                if v is not None:
                    args[4 + 4 * i] = v
            if t is not None:
                calls.append((t, args))
                st.esp += self.pops.get(t, 0)
                ret = self.returns.get(t) or ("R", t)  # ("R", t): t's result, for constructor seeding
            else:
                ret = None
            for r in ("eax", "ecx", "edx"):
                st.regs.pop(r, None)
            if ret is not None:
                st.regs["eax"] = ret
        elif mn in ("cmp", "test", "jmp", "ret") or mn.startswith("j") or mn.startswith("set") or mn.startswith(
                "f") or mn.startswith("comis") or mn.startswith("ucomis") or mn == "nop" or mn.startswith("prefetch"):
            if mn.startswith("set") and ops:
                self.write(st, ops[0], None)
            elif mn.startswith("f") and mn.startswith("fn") and ops and ops[0] in REGS:
                self.write(st, ops[0], None)
        elif mn.startswith("rep") or mn in ("movsd", "movsb", "movsw", "stosd", "stosb", "stosw"):
            for r in ("esi", "edi"):
                v = st.regs.get(r)
                if v and v[0] == "B":
                    s = STRUCT_BY_NAME[v[1]]
                    self.add_site(Site(insn.va, func, "copy", s.name, "-", v[2], 0, v[4], insn.text))
            for r in ("esi", "edi", "ecx"):
                st.regs.pop(r, None)
        elif mn.startswith("cmov") and len(ops) == 2:
            # either value: a struct pointer on one side is kept (00026EA0 cmovnz ecx, [0x0053F2E0])
            self.write(st, ops[0], merge(self.value(st, ops[0], insn), self.value(st, ops[1], insn)))
        elif mn == "xchg" and len(ops) == 2:
            a, b = self.value(st, ops[0], insn), self.value(st, ops[1], insn)
            self.write(st, ops[0], b)
            self.write(st, ops[1], a)
        else:
            # anything else writes its first operand (when it has one) with an unknown value
            if ops and mn not in ("cdq",):
                self.write(st, ops[0], None)
            if mn in ("cdq", "mul", "div", "idiv") or (mn == "imul" and len(ops) == 1):
                st.regs.pop("edx", None)
                if mn != "cdq":
                    st.regs.pop("eax", None)
            if mn in ("cpuid", "rdtsc"):
                for r in ("eax", "ebx", "ecx", "edx"):
                    st.regs.pop(r, None)

    def analyze(self, func: int):
        body = self.funcs[func]
        if not body:
            return [], None
        index = {i.va: n for n, i in enumerate(body)}
        entry = State()
        for k, v in self.params.get(func, {}).items():
            origin = "this" if k == "ecx" else "param"
            v = ("B", v[1], v[2], v[3], origin) if v[0] == "B" else v
            if k == "ecx":
                entry.regs["ecx"] = v
            else:
                entry.slots[k] = v
        states: dict[int, State] = {0: entry}
        work = [0]
        seen_keys: dict[int, tuple] = {}
        calls: list = []
        ret_val = []
        visits = collections.Counter()
        while work:
            n = work.pop()
            visits[n] += 1
            if visits[n] > 40:
                continue
            st = states[n].copy()
            while True:
                insn = body[n]
                self.step(st, insn, func, calls)
                succ = []
                fall = True
                if insn.mn == "ret":
                    ret_val.append(st.regs.get("eax"))
                    fall = False
                elif insn.mn == "jmp":
                    fall = False
                    t = imm(insn.ops[0]) if insn.ops else None
                    if t is not None and t in index:
                        succ.append(index[t])
                    elif t is not None and t in self.funcs:
                        args = {"ecx": st.regs.get("ecx")}
                        for i in range(8):
                            v = st.slots.get(st.esp + 4 + 4 * i)
                            if v is not None:
                                args[4 + 4 * i] = v
                        calls.append((t, args))
                        ret_val.append(self.returns.get(t))
                    elif t is None and insn.targets:  # a jump table
                        succ.extend(index[x] for x in insn.targets if x in index)
                    elif t is None:  # unknown targets: any instruction of the body may follow
                        succ.extend(range(len(body)))
                elif insn.mn.startswith("j") and insn.ops:
                    t = imm(insn.ops[0])
                    if t is not None and t in index:
                        succ.append(index[t])
                if fall and n + 1 < len(body):
                    succ.append(n + 1)
                if len(succ) == 1 and succ[0] == n + 1 and (n + 1) not in states and fall and not insn.mn.startswith("j"):
                    n += 1
                    continue
                for s in succ:
                    merged = merge_states(states.get(s), st) if s in states else st.copy()
                    k = merged.key()
                    if seen_keys.get(s) != k:
                        seen_keys[s] = k
                        states[s] = merged
                        work.append(s)
                break
        ret = None
        if ret_val and all(v is not None and v[0] == "B" and v[2] == 0 for v in ret_val):
            ret = ("B", ret_val[0][1], 0, False, "ret")
        return calls, ret

    def interesting(self, func: int) -> bool:
        if func in self.params:
            return True
        for i in self.funcs[func]:
            for op in i.ops:
                m = parse_mem(op)
                if m and not m[0] and m[1] in GLOBAL_STRUCT:
                    return True
            if i.mn in ("call", "jmp") and i.ops:
                t = imm(i.ops[0])
                if t in self.returns:
                    return True
        return False

    def run(self, rounds=8, log=sys.stderr):
        for r in range(rounds):
            self.sites.clear()
            self.notes.clear()
            new_params: dict[int, dict] = collections.defaultdict(dict)
            new_returns = dict(self.returns)
            todo = [f for f in self.funcs if self.interesting(f)]
            for f in todo:
                calls, ret = self.analyze(f)
                if ret is not None:
                    new_returns[f] = ret
                for t, args in calls:
                    if t not in self.funcs:
                        continue
                    for k, v in args.items():
                        if v is not None and v[0] == "B" and v[2] == 0:
                            new_params[t][k] = ("B", v[1], 0, False, "param")
            for t, st in self.seeds.items():
                if t in self.funcs:
                    new_params[t]["ecx"] = B(st)
            changed = dict(new_params) != {k: v for k, v in self.params.items() if v} or new_returns != self.returns
            self.params = collections.defaultdict(dict, new_params)
            self.returns = new_returns
            print(f"round {r + 1}: {len(todo)} functions, {len(self.params)} with struct arguments, "
                  f"{len(self.returns)} returning one, {len(self.sites)} sites", file=log)
            if not changed:
                break


# --- output ------------------------------------------------------------------------------------------------------


def new_offset(site: Site, slots: int) -> int | None:
    """The patched operand for a site once the arrays move to the end of their structs."""
    s = STRUCT_BY_NAME[site.struct]
    arr = next((a for a in s.arrays if a.name == site.array), None)
    if arr is None:
        return None
    tail = s.size
    for a in s.arrays:
        if a == arr:
            break
        tail += slots * a.stride
    return site.disp + (tail - arr.start)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    root = pathlib.Path(__file__).resolve().parents[2]
    ap.add_argument("--generated", type=pathlib.Path, default=root / "templates/game/generated/game")
    ap.add_argument("--out", type=pathlib.Path, help="sites as tab separated values")
    ap.add_argument("--toml", type=pathlib.Path, help="[[patch]] entries for the disp and add sites")
    ap.add_argument("--slots", type=int, default=16)
    ap.add_argument("--notes", type=pathlib.Path, help="loop registers used as struct offsets, for manual review")
    a = ap.parse_args(argv)
    funcs = load_functions(a.generated)
    print(f"{len(funcs)} functions", file=sys.stderr)
    sc = Scanner(funcs)
    sc.run()
    sites = sorted(sc.sites.values(), key=lambda s: (s.struct, s.va))
    by = collections.Counter((s.struct, s.kind) for s in sites)
    for k, v in sorted(by.items()):
        print(f"{k[0]} {k[1]}: {v}", file=sys.stderr)
    lines = ["va\tfunction\tkind\tstruct\tarray\toffset\toperand\tnew\torigin\tinstruction"]
    for s in sites:
        n = new_offset(s, a.slots)
        lines.append(f"{s.va:08X}\t{s.func:08X}\t{s.kind}\t{s.struct}\t{s.array}\t0x{s.offset:X}\t0x{s.disp & 0xFFFFFFFF:X}\t"
                     f"{'' if n is None else hex(n)}\t{s.origin}\t{s.text}")
    if a.out:
        a.out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    else:
        print("\n".join(lines))
    if a.toml:
        out = [f"# generated by tools/diagnostics/slot_scan.py --slots {a.slots}; review before use"]
        for s in sites:
            n = new_offset(s, a.slots)
            if s.kind not in ("disp", "add") or n is None:
                continue
            key = "disp" if s.kind == "disp" else "imm"
            out.append(f'\n[[patch]]\nat = 0x{s.va:08X}\nexpect = "{s.text}"\n{key} = 0x{n:X}\n'
                       f'why = "{s.struct}.{s.array} moved ({s.origin})"')
        a.toml.write_text("\n".join(out) + "\n", encoding="utf-8")
    if a.notes:
        a.notes.write_text("".join(f"{f:08X}\t{n}\n" for f in sorted(sc.notes) for n in sorted(sc.notes[f])),
                           encoding="utf-8")
    notes = sum(len(v) for v in sc.notes.values())
    print(f"{notes} loop-register notes in {len(sc.notes)} functions", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
