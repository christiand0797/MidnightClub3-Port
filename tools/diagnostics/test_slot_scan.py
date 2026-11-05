import io
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import slot_scan as ss  # noqa: E402


def fn(lines):
    out = []
    for line in lines:
        va, text = line.split(" ", 1)
        out.append(ss.parse_insn(int(va, 16), text))
    return out


def scan(funcs):
    sc = ss.Scanner({k: fn(v) for k, v in funcs.items()})
    sc.run(log=io.StringIO())
    return sc, {(s.va, s.kind) for s in sc.sites.values()}


class SlotScanTest(unittest.TestCase):
    def test_parse_mem(self):
        self.assertEqual(ss.parse_mem("dword ptr [eax+ecx*1+0x114]"), ([("eax", 1), ("ecx", 1)], 0x114))
        self.assertEqual(ss.parse_mem("[ebp-0x28]"), ([("ebp", 1)], -0x28))
        self.assertEqual(ss.parse_mem("[0x0053F2E8]"), ([], 0x53F2E8))
        self.assertIsNone(ss.parse_mem("eax"))

    def test_accessor(self):
        # 002693D0: imul eax, 0x2C; lea eax, [eax+ecx*1+0x114] with ecx = the live config
        _, sites = scan({0x2693D0: [
            "002693D0 mov eax, [esp+0x04]",
            "002693D4 mov ecx, [0x0053F2E0]",
            "002693DA imul eax, eax, 0x2C",
            "002693DD lea eax, [eax+ecx*1+0x114]",
            "002693E4 ret",
        ]})
        self.assertEqual(sites, {(0x2693DD, "disp")})

    def test_fields_outside_arrays_are_not_sites(self):
        _, sites = scan({0x10: [
            "00000010 mov eax, [0x0053F2E8]",
            "00000015 mov ecx, [eax+0x4C]",
            "00000018 mov edx, [eax+0x3D8]",
            "0000001E mov [eax+0x50], ecx",
            "00000021 ret",
        ]})
        self.assertEqual(sites, set())

    def test_loop_pointer_is_one_site(self):
        # 0026B9E0-like: lea edi, [esi+0x114] then step by 0x2C; esi is `this`
        sc, sites = scan({
            0x100: [
                "00000100 mov ecx, [0x0053F2E4]",
                "00000106 call 0x00000200",
                "0000010B ret",
            ],
            0x200: [
                "00000200 push esi",
                "00000201 mov esi, ecx",
                "00000203 lea edi, [esi+0x114]",
                "00000209 mov ebx, 0x08",
                "0000020E mov [edi], eax",
                "00000210 add edi, 0x2C",
                "00000213 dec ebx",
                "00000214 jnz 0x0000020E",
                "00000216 mov [esi+0x279], al",
                "0000021C pop esi",
                "0000021D ret",
            ],
        })
        self.assertEqual(sites, {(0x203, "disp"), (0x216, "disp")})
        self.assertEqual(sc.sites[(0x216, "disp")].array, "players2")
        self.assertEqual(sc.sites[(0x203, "disp")].origin, "this")

    def test_stack_argument_and_local(self):
        _, sites = scan({
            0x100: [
                "00000100 mov eax, [0x0053F2E0]",
                "00000105 push eax",
                "00000106 call 0x00000200",
                "0000010B add esp, 0x04",
                "0000010E ret",
            ],
            0x200: [
                "00000200 push ebp",
                "00000201 mov ebp, esp",
                "00000203 sub esp, 0x08",
                "00000206 mov eax, [ebp+0x08]",
                "00000209 mov [ebp-0x04], eax",
                "0000020C mov ecx, [ebp-0x04]",
                "0000020F mov edx, [ecx+0x124]",
                "00000215 mov esp, ebp",
                "00000217 pop ebp",
                "00000218 ret",
            ],
        })
        self.assertEqual(sites, {(0x20F, "disp")})

    def test_return_value(self):
        _, sites = scan({
            0x100: ["00000100 mov eax, [0x00558814]", "00000105 ret"],
            0x200: [
                "00000200 call 0x00000100",
                "00000205 mov ecx, [esp+0x04]",
                "00000209 mov eax, [eax+ecx*4+0x08]",
                "0000020D ret 0x04",
            ],
        })
        self.assertEqual(sites, {(0x209, "disp")})

    def test_manager_constant_offset_loop(self):
        # 001CC7B0-like: edi = 8, 12, ... as the offset of racer i in the manager
        sc, sites = scan({0x300: [
            "00000300 mov ecx, [0x00558814]",
            "00000306 xor esi, esi",
            "00000308 mov edi, 0x08",
            "0000030D mov ecx, [0x00558814]",
            "00000313 cmp dword ptr [edi+ecx*1], 0x00",
            "00000317 add edi, 0x04",
            "0000031A inc esi",
            "0000031B cmp esi, [ecx+0x28]",
            "0000031E jl 0x0000030D",
            "00000320 ret",
        ]})
        self.assertIn((0x308, "const"), sites)
        self.assertNotIn((0x31B, "disp"), sites)

    def test_add_immediate(self):
        _, sites = scan({0x400: [
            "00000400 mov eax, [0x0053F2E8]",
            "00000405 add eax, 0x274",
            "0000040A mov [eax], ecx",
            "0000040C ret",
        ]})
        self.assertEqual(sites, {(0x405, "add")})

    def test_constructor_seeded_from_global_store(self):
        # 00268D00 allocates, calls the constructor 0026B9E0 with ecx = the allocation and stores its result
        sc, sites = scan({
            0x268D00: [
                "00268D00 push 0x3E0",
                "00268D05 call 0x001B3B90",
                "00268D0A add esp, 0x04",
                "00268D11 mov ecx, eax",
                "00268D13 call 0x0026B9E0",
                "00268D18 mov [0x0053F2E4], eax",
                "00268D1D ret",
            ],
            0x1B3B90: ["001B3B90 ret"],
            0x26B9E0: [
                "0026B9E0 push esi",
                "0026B9E2 mov esi, ecx",
                "0026B9E5 lea edi, [esi+0x114]",
                "0026B9EB mov eax, esi",
                "0026B9ED pop esi",
                "0026B9EE ret",
            ],
        })
        self.assertEqual(sites, {(0x26B9E5, "disp")})
        self.assertEqual(sc.sites[(0x26B9E5, "disp")].origin, "this")

    def test_jump_table_targets(self):
        # an earlier counter loop must not blur a later constant through the switch: only the case targets follow
        lines = [
            "00000500 xor edi, edi",
            "00000502 jmp [eax*4+0x1D055C]",
            "00000509 inc edi",
            "0000050A ret",
            "0000050B mov edi, 0x08",
            "00000510 mov ecx, [0x00558814]",
            "00000516 cmp dword ptr [edi+ecx*1], 0x00",
            "0000051A ret",
        ]
        insns = fn(lines)
        insns[1].targets = [0x50B]
        sc = ss.Scanner({0x500: insns})
        sc.run(log=io.StringIO())
        self.assertIn((0x50B, "const"), {(s.va, s.kind) for s in sc.sites.values()})

    def test_case_lines_parsed(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            pathlib.Path(d, "x86_recomp.0.cpp").write_text(
                "".join(line + chr(10) for line in [
                    "X86_FUNC(__imp__sub_00000500) {",
                    chr(9) + "// 00000500 jmp [eax*4+0x1D055C]",
                    chr(9) + "switch (X) {",
                    chr(9) + "case 0x50Bu: goto loc_0000050B;",
                    chr(9) + "case 0x509u: goto loc_00000509;",
                    chr(9) + "}",
                    "}",
                ]), encoding="utf-8")
            funcs = ss.load_functions(pathlib.Path(d))
        self.assertEqual(funcs[0x500][0].targets, [0x50B, 0x509])

    def test_cmov_keeps_struct_pointer(self):
        # 00026E70: ecx = menu config, or the saved one when a flag is set; then a slot field is written
        _, sites = scan({0x26E70: [
            "00026E91 mov ecx, [0x0053F2E4]",
            "00026E97 test edx, edx",
            "00026EA0 cmovnz ecx, [0x0053F2E0]",
            "00026EB0 imul eax, eax, 0x2C",
            "00026EB3 mov [eax+ecx*1+0x13A], bl",
            "00026EBA ret",
        ]})
        self.assertEqual(sites, {(0x26EB3, "disp")})

    def test_new_offset(self):
        s = ss.Site(0, 0, "disp", "cfg", "players", 0x120, 0x120, "global", "")
        self.assertEqual(ss.new_offset(s, 16), 0x3E0 + 0xC)
        s2 = ss.Site(0, 0, "disp", "cfg", "players2", 0x279, 0x279, "global", "")
        self.assertEqual(ss.new_offset(s2, 16), 0x3E0 + 16 * 0x2C + 5)
        s3 = ss.Site(0, 0, "disp", "mgr", "racers", 0x08, 0x08, "global", "")
        self.assertEqual(ss.new_offset(s3, 16), 0x74)


if __name__ == "__main__":
    unittest.main()
