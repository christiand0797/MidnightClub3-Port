import importlib.util
import math
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location("skin",Path(__file__).with_name("analyze_ped_skinning.py"))
skin=importlib.util.module_from_spec(spec);spec.loader.exec_module(skin)


def draw(index=0, position=(0,0,0), heading=0, arm=0, scale=256, vb="0:81234560"):
    c,s=math.cos(heading),math.sin(heading)
    rotation=[[c,-s,0],[s,c,0],[0,0,1]]
    a,b=math.cos(arm),math.sin(arm)
    limb=[[1,0,0],[0,a,-b],[0,b,a]]
    bone=[[sum(rotation[i][k]*limb[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    constants={i:rotation[i]+[position[i]] for i in range(3)}
    constants.update({12+i:bone[i]+[position[i]+rotation[i][0]] for i in range(3)})
    constants[105]=[256,256,256,scale]
    lines=[f"{index:04d} draw rt 12345678 ds 01234567 prim 6 count 100 vs B0102471",
           f"     skin scale {scale} bone_rows 0 12",f"     skin vdata {vb}",
           "     skin layout 0:42:0:0 1:12:0:16 12:40:0:20", "     verts",
           "     v0 r0( 1 2 3 1) r1( 0) r12(FF0C0000)"]
    for reg,row in sorted(constants.items()):lines.append(f"     c{reg-96} "+" ".join(str(v) for v in row))
    return "\n".join(lines)+"\n"


class SkinCaptureTests(unittest.TestCase):
    def read(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"frame.txt";path.write_text(text,encoding="utf-8")
            return skin.read_capture(path)

    def test_rigid_world_motion_is_not_limb_animation(self):
        before=self.read(draw(position=(50,2,80),heading=.2))
        after=self.read(draw(position=(51,2,80),heading=.8))
        pair=skin.compare(before,after)[0]
        self.assertFalse(pair["relative_pose_changed"])
        self.assertLess(pair["bone_changes"][0]["max_relative_delta"],1e-10)
        self.assertEqual(pair["observed_nonroot_bones"],1)

    def test_relative_limb_pose_advances_despite_world_motion(self):
        before=self.read(draw(position=(50,2,80),heading=.2,arm=-.4))
        after=self.read(draw(position=(51,2,80),heading=.8,arm=.4))
        self.assertTrue(skin.compare(before,after)[0]["relative_pose_changed"])

    def test_shared_mesh_pairs_nearest_root_without_reusing_person(self):
        before=self.read(draw(0,(10,0,10))+draw(1,(30,0,10)))
        after=self.read(draw(0,(30.5,0,10),arm=.5)+draw(1,(10.5,0,10)))
        pairs=skin.compare(before,after)
        self.assertEqual([(p["before_draw"],p["after_draw"]) for p in pairs],[(0,1),(1,0)])
        self.assertEqual([p["relative_pose_changed"] for p in pairs],[False,True])

    def test_ambiguous_shared_geometry_is_not_identity_claim(self):
        before=self.read(draw(0,(10,0,10)))
        after=self.read(draw(0,(10.5,0,10))+draw(1,(10.51,0,10)))
        self.assertFalse(skin.compare(before,after))

    def test_zero_c9_is_reported(self):
        result=self.read(draw(scale=0))
        self.assertIn("skin index scale differs from original256",result[0]["issues"])

    def test_full_snapshot_resets_omitted_zero_constants(self):
        result=self.read(draw()+draw(1).replace("     c9 256 256 256 256\n",""))
        self.assertEqual(result[1]["scale"],256) # Explicit diagnostic still records the actual scale independently.
        self.assertEqual(result[1]["constants"][105],[0,0,0,0])
        self.assertIn("diagnostic scale disagrees with c9 constant snapshot",result[1]["issues"])

    def test_different_geometry_and_distant_person_are_unmatched(self):
        before=self.read(draw())
        self.assertFalse(skin.compare(before,self.read(draw(vb="0:81234570"))))
        self.assertFalse(skin.compare(before,self.read(draw(position=(3,0,0)))))

    def test_non_skin_frame_is_empty(self):
        self.assertFalse(self.read("0000 draw rt 12345678 ds 01234567 prim 6 count 100 vs 00000142\n"))


if __name__=="__main__":unittest.main()
