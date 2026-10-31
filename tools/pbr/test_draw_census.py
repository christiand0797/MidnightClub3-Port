import importlib.util
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location("census",Path(__file__).with_name("draw_census.py"))
census=importlib.util.module_from_spec(spec);spec.loader.exec_module(census)

FIXED_NORMAL="    float4 v0 : ATTR0;\n    uint v2 : ATTR2;\n    float4 cp = mul(ip, wvp);\n"
FIXED_PRELIT="    float4 v0 : ATTR0;\n    float4 v3 : ATTR3;\n    float4 cp = mul(ip, wvp);\n"
PROGRAM="    float4 v0 : ATTR0;\n    uint v2 : ATTR2;\n    R0 = mac_mul(v0, c[4]);\n"


def draw(index,prim=5,vs="B0105701",blend="0 0 0",zwrite=1,stages=1):
    tex="".join(f" | t{i} 81000000 fmt F 64x64 addr 11 filt 221 cop 4" for i in range(stages))
    return (f"{index:04d} draw rt 03E9E000 ds 03BFA000 prim {prim} count 30 vs {vs} ps 00000000 blend {blend} "
            f"zen 1 203 zwrite {zwrite} zbias 0 0.00 cull 1 cw F c0 0 0 0 0 op 8006 sten 1 207 FF FF{tex}\n")


class Census(unittest.TestCase):
    def capture(self,root,name,frame,shaders):
        cap=Path(root)/name;cap.mkdir()
        (cap/"frame.txt").write_text(frame,encoding="utf-8")
        for file,text in shaders.items():(cap/file).write_text(text,encoding="utf-8")
        return cap

    def test_funnel_follows_gate_order_and_reuses_first_translation(self):
        with tempfile.TemporaryDirectory() as root:
            frame=(draw(0)+"     vb0 stride 20 v0 00\n     vs_A.hlsl\n     lighting colorvertex 0\n"
                   +draw(1)+"     vb0 stride 20 v0 00\n     lighting colorvertex 0\n"  # same class, no hlsl line
                   +draw(2,prim=6)+"     vb0 stride 20 v0 00\n     lighting colorvertex 0\n"  # strip
                   +draw(3,vs="B0105721")+"     vb0 stride 28 v0 00\n     vs_B.hlsl\n"  # prelit, no normal
                   +draw(4,vs="B0102001")+"     vs_C.hlsl\n"  # programmable
                   +draw(5,vs="000001C4")+"     vs_A.hlsl\n"  # pretransformed FVF
                   +draw(6,blend="1 302 303")+"     vb0 stride 20 v0 00\n     lighting colorvertex 0\n"
                   +draw(7,stages=2)+"     vb0 stride 20 v0 00\n     lighting colorvertex 0\n"
                   +"0008 clear rt 03D4C000\n     lighting colorvertex 0\n")  # not attributed to draw 7
            cap=self.capture(root,"f1",frame,{"vs_A.hlsl":FIXED_NORMAL,"vs_B.hlsl":FIXED_PRELIT,"vs_C.hlsl":PROGRAM})
            r=census.census([cap])
        self.assertEqual(r["draws"],8)
        self.assertEqual(r["first_block"],{"candidate":2,"fixed_transform":2,"triangle_list":1,"authored_normal":1,
                                           "single_stage":1,"opaque":1})
        self.assertEqual(r["funnel"],{"fixed_transform":6,"triangle_list":5,"authored_normal":4,"ff_lighting":4,
                                      "single_stage":3,"opaque":2})

    def test_skip_lifts_named_gates_only(self):
        with tempfile.TemporaryDirectory() as root:
            frame=draw(0,prim=6)+"     vs_A.hlsl\n"+draw(1,prim=6,vs="B0105721")+"     vs_B.hlsl\n"
            cap=self.capture(root,"f1",frame,{"vs_A.hlsl":FIXED_NORMAL,"vs_B.hlsl":FIXED_PRELIT})
            r=census.census([cap],skip=("triangle_list","ff_lighting"))
        self.assertEqual(r["first_block"],{"candidate":1,"authored_normal":1})
        self.assertNotIn("triangle_list",r["funnel"])

    def test_fvf_normal_bit_counts_without_translation(self):
        with tempfile.TemporaryDirectory() as root:
            cap=self.capture(root,"f1",draw(0,vs="00000152")+"     vs_A.hlsl\n",{"vs_A.hlsl":FIXED_PRELIT})
            r=census.census([cap])
        self.assertEqual(r["funnel"]["authored_normal"],1)


if __name__=="__main__":
    unittest.main()
