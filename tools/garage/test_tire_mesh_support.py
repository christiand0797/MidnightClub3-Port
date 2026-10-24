#!/usr/bin/env python3
"""Synthetic malformed-format/affine support tests, plus optional checks of the user's original tire pool."""
import importlib.util
import contextlib
import io
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parent))
import tire_mesh_support as tire


def synthetic(positions=((.25,-.5,0.),(-.25,0.,0.),(0.,.5,0.)), indices=(0,1,2)):
    """Minimal supported record, with index resource/count pointer independently placed before index data."""
    b=bytearray(0x300)
    base=0x82A00020
    def p(offset): return base+offset-0x20
    def w(offset,*values): struct.pack_into('<'+'I'*len(values),b,offset,*values)
    w(0,base,0,0,len(b))
    w(0x20,0x608EC4)
    w(0x30,p(0xA0))
    w(0xA4,1<<16,p(0xB0))
    w(0xB0,p(0xC0))
    w(0xC0,0x5DC6F8,28<<24,1,0)
    w(0xD0,p(0xE0),0,p(0xF0),p(0x240))
    w(0xF0,1,p(0x100)&0x0FFFFFFF,0)
    for i,point in enumerate(positions): struct.pack_into('<3f',b,0x100+28*i,*point)
    ib=0x100+28*len(positions)
    w(0xE0,p(ib+4))
    w(ib,1,len(indices)<<16,0)
    struct.pack_into('<'+'H'*len(indices),b,0x240,*indices)
    return b,tire.Record(0,0,'tire_test','drwShaderModel',0,len(b))


def pool_bytes(blob):
    data=bytearray(0x11000)
    struct.pack_into('<4sIII',data,0,b'pf05',1,4096,(2<<20)|32)
    data[0x10000:0x10000+len(blob)]=blob
    return data,f'# pageno,offset,name,type,size\n0,0,tire_test,drwShaderModel,{len(blob)}\n'


class FormatTests(unittest.TestCase):
    def test_supported_record_and_nontrivial_index_header_gap(self):
        b,r=synthetic()
        g,=tire.decode_record(b,r)
        self.assertEqual((g.slot,g.stride,len(g.positions),g.indices),(0,28,3,(0,1,2)))
        self.assertEqual(g.positions[0],(.25,-.5,0.))
        # The index header is at0x154 but index data at0x240. Guessing data-minus16 cannot decode this fixture.
        self.assertEqual(g.index_offset,0x240)

    def test_unreferenced_position_is_not_in_support_set(self):
        b,r=synthetic(((0.,0.,0.),(0.,-1.,0.),(1.,0.,0.),(0.,-100.,0.)))
        g,=tire.decode_record(b,r)
        self.assertEqual(len(g.positions),4)
        self.assertEqual(tire.support(g.used_positions,((1,0,0),(0,1,0),(0,0,1)))['support_from_hub_m'],1.)

    def test_bad_geometry_pointer_rejected(self):
        b,r=synthetic();struct.pack_into('<I',b,0xB0,0xFFFFFFFF)
        with self.assertRaises(tire.FormatError):tire.decode_record(b,r)

    def test_pointer_into_record_header_rejected(self):
        b,r=synthetic();struct.pack_into('<I',b,0x30,struct.unpack_from('<I',b)[0]-32)
        with self.assertRaises(tire.FormatError):tire.decode_record(b,r)

    def test_truncated_record_rejected(self):
        b,r=synthetic()
        with self.assertRaises(tire.FormatError):tire.decode_record(b[:-1],r)

    def test_unsupported_stride_rejected_without_searching_for_floats(self):
        b,r=synthetic();struct.pack_into('<I',b,0xC4,32<<24)
        with self.assertRaises(tire.FormatError):tire.decode_record(b,r)

    def test_index_count_extent_rejected(self):
        b,r=synthetic();ib=0x154;struct.pack_into('<I',b,ib+4,65535<<16)
        with self.assertRaises(tire.FormatError):tire.decode_record(b,r)

    def test_out_of_range_index_rejected(self):
        b,r=synthetic(indices=(0,1,3))
        with self.assertRaises(tire.FormatError):tire.decode_record(b,r)

    def test_vertex_physical_address_must_be_local_to_record(self):
        b,r=synthetic();struct.pack_into('<I',b,0xF4,0x01000000)
        with self.assertRaises(tire.FormatError):tire.decode_record(b,r)

    def test_nonfinite_vertex_rejected(self):
        b,r=synthetic();struct.pack_into('<f',b,0x100,float('nan'))
        with self.assertRaises(tire.FormatError):tire.decode_record(b,r)

    def test_geometry_count_cannot_read_past_table(self):
        b,r=synthetic();struct.pack_into('<I',b,0xA4,4096<<16)
        with self.assertRaises(tire.FormatError):tire.decode_record(b,r)

    def test_map_record_size_must_match(self):
        b,_=synthetic();data,text=pool_bytes(b)
        self.assertEqual(tire.validate_pool(data,text)[0].start,0x10000)
        with self.assertRaises(tire.FormatError):tire.validate_pool(data,text.replace(',768',',767'))

    def test_map_record_cannot_cross_page(self):
        b,_=synthetic();data,text=pool_bytes(b)
        with self.assertRaises(tire.FormatError):tire.validate_pool(data,text.replace('0,0,tire','0,4000,tire'))

    def test_map_page_and_duplicates_rejected(self):
        b,_=synthetic();data,text=pool_bytes(b)
        with self.assertRaises(tire.FormatError):tire.validate_pool(data,text.replace('0,0,tire','1,0,tire'))
        with self.assertRaises(tire.FormatError):tire.validate_pool(data,text+text.splitlines()[1]+'\n')

    def test_truncated_page_table_and_bad_page_extent(self):
        with self.assertRaises(tire.FormatError):tire.validate_pool(b'pf05','')
        b,_=synthetic();data,text=pool_bytes(b)
        struct.pack_into('<I',data,12,(2<<20)|0xFFFFF)
        with self.assertRaises(tire.FormatError):tire.validate_pool(data,text)

    def test_texture_zero_baked_base_is_accepted_but_model_zero_is_rejected(self):
        b,_=synthetic();struct.pack_into('<I',b,0,0);data,text=pool_bytes(b)
        with self.assertRaises(tire.FormatError):tire.validate_pool(data,text)
        self.assertEqual(tire.validate_pool(data,text.replace('drwShaderModel','mip0'))[0].kind,'mip0')

    def test_cli_refuses_to_replace_an_existing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            output=Path(directory)/'existing.json';output.write_text('keep these bytes',encoding='utf-8')
            with patch.object(sys,'argv',['tire_mesh_support.py','--output',str(output)]), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(tire.main(),1)
            self.assertEqual(output.read_text(encoding='utf-8'),'keep these bytes')


class SupportTests(unittest.TestCase):
    def test_affine_support_and_witness_for_known_offcentre_triangle(self):
        points=((2.,-1.,3.),(-1.,4.,0.),(0.,0.,-2.))
        # Explicit map(x,y,z)=(2z,x,-y), then translate(0,5,0), planeY2. Lowest point is index1 atworldY4.
        result=tire.support(points,((0,0,2),(1,0,0),(0,-1,0)),(0,2,0),(0,5,0),2)
        self.assertEqual(result['support_from_hub_m'],1.)
        self.assertEqual(result['clearance_m'],2.)
        self.assertEqual(result['witness_used_vertex'],1)
        self.assertEqual(result['witness_relative_position_m'],(0.,-1.,-4.))

    def test_sloped_normal_cannot_be_replaced_by_vertical_radius(self):
        result=tire.support(((0.,-2.,0.),(3.,-1.,0.)),((1,0,0),(0,1,0),(0,0,1)),(-3,4,0))
        self.assertAlmostEqual(result['support_from_hub_m'],2.6)
        self.assertEqual(result['witness_used_vertex'],1)

    def test_extreme_finite_normals_keep_their_direction(self):
        matrix=((1,0,0),(0,1,0),(0,0,1))
        points=((0.,-1.,0.),(1.,0.,0.))
        for normal in ((1.7e308,1.7e308,1.7e308),(5e-324,5e-324,5e-324)):
            result=tire.support(points,matrix,normal)
            self.assertAlmostEqual(result['support_from_hub_m'],1/math.sqrt(3))
            self.assertAlmostEqual(sum(x*x for x in result['unit_normal']),1.)

    def test_transformed_overflow_is_rejected(self):
        with self.assertRaises(ValueError):
            tire.support(((1e308,1e308,0.),),((1e308,0,0),(0,1,0),(0,0,1)))

    def test_stated_side_orientation_and_width_vs_diameter_normalization(self):
        left=tire.wheel_matrix(.4,.8,0,'left',.01745)
        right=tire.wheel_matrix(.4,.8,0,'right',.01745)
        point=((.25,.5,.5),)
        self.assertEqual(tire.support(point,left)['witness_relative_position_m'],(.2,.4,.4))
        rp=tire.support(point,right)['witness_relative_position_m']
        self.assertAlmostEqual(rp[0],-.2,places=6)
        self.assertAlmostEqual(rp[1],.4,places=6)
        self.assertAlmostEqual(rp[2],-.4,places=6)

    def test_all_sides_and_camber_signs_generate_finite_support(self):
        points=((.25,-.5,0.),(-.18,-.45,.1))
        for side in ('left','right'):
            for camber in (-25,0,25):
                matrix=tire.wheel_matrix(.755,.65,camber,side,.01745,roll=7,steer=20,spin=33)
                result=tire.support(points,matrix,normal=(.2,1,.1))
                self.assertTrue(math.isfinite(result['support_from_hub_m']))
                self.assertAlmostEqual(sum(x*x for x in result['unit_normal']),1.)

    def test_invalid_plane_dimensions_or_values_rejected(self):
        matrix=((1,0,0),(0,1,0),(0,0,1))
        for points,normal in [((),(0,1,0)),(((0,0,0),),(0,0,0)),(((0,float('inf'),0),),(0,1,0))]:
            with self.assertRaises(ValueError):tire.support(points,matrix,normal)
        with self.assertRaises(ValueError):tire.wheel_matrix(0,.7,0,'left',.01745)
        with self.assertRaises(ValueError):tire.support(((0,0,0),),((1,0),(0,1)),(0,1,0))


POOL=tire.ROOT/'templates/game/assets/resources/vehicle/tire.xbpf'
XBE=tire.ROOT/'templates/game/assets/default.xbe'
@unittest.skipUnless(POOL.exists() and XBE.exists() and importlib.util.find_spec('numpy'),
                     'original user-supplied assets and existing texture-tool NumPy dependency are required')
class OriginalAssetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data,cls.records,_=tire.load_pool(POOL)
        cls.models=[r for r in cls.records if r.kind=='drwShaderModel']

    def test_original_metadata_and_all_index_references(self):
        self.assertEqual(len(self.models),98)
        geometries=[g for r in self.models for g in tire.decode_record(self.data,r)]
        self.assertEqual(len(geometries),196)
        self.assertEqual({g.stride for g in geometries},{28})
        self.assertTrue(all(len(g.positions)==len(g.used_indices) for g in geometries))
        first=tire.decode_record(self.data,self.models[0])
        self.assertEqual([(len(g.positions),len(g.indices)) for g in first],[(180,246),(60,79)])
        bike=tire.decode_record(self.data,self.models[88])
        self.assertEqual([(len(g.positions),len(g.indices)) for g in bike],[(270,416),(180,243)])

    def test_source_constants_and_original_first_slot_position(self):
        constants=tire.xbe_constants(XBE)
        self.assertEqual(constants['width_factor_at_449004'],2.)
        self.assertEqual(constants['degree_factor_at_44B090'],0.01744999922811985)
        g=tire.decode_record(self.data,self.models[0])[0]
        self.assertAlmostEqual(g.positions[0][0],-.18000799417495728)
        self.assertAlmostEqual(max(math.hypot(y,z) for _,y,z in g.used_positions),.5002132485040647)

    def test_original_slots_remain_separate_and_rounded_shoulder_differs(self):
        g0,g1=tire.decode_record(self.data,self.models[0])
        factor=tire.xbe_constants(XBE)['degree_factor_at_44B090']
        matrix=tire.wheel_matrix(.755,.65,15,'left',factor)
        s0=tire.support(g0.used_positions,matrix)['support_from_hub_m']
        s1=tire.support(g1.used_positions,matrix)['support_from_hub_m']
        self.assertAlmostEqual(s0,.3854139388876163)
        self.assertAlmostEqual(s1,.4116162250145677)
        self.assertGreater(s1-s0,.026)
        self.assertAlmostEqual(tire.support(g0.used_positions,tire.wheel_matrix(.755,.65,0,'left',factor))
                               ['support_from_hub_m'],.3251384675502777)

    def test_report_preserves_page_identity_slots_and_original_vertex_witness(self):
        report=tire.create_report(POOL,XBE,[.755],[.65],[15.],['left'],[0.,1.,0.])
        self.assertEqual(sum(m['ordinary_car_scale_scenario'] for m in report['models']),88)
        self.assertEqual(len({m['page'] for m in report['models']}),98)
        model=report['models'][0]
        for slot,g in zip(model['draw_slots'],tire.decode_record(self.data,self.models[0])):
            sample,=slot['samples']
            self.assertIn(sample['witness_stream_vertex'],g.indices)
            point=g.positions[sample['witness_stream_vertex']]
            matrix=tire.wheel_matrix(.755,.65,15.,'left',report['source_xbe']['degree_factor_at_44B090'])
            self.assertAlmostEqual(tire.support((point,),matrix)['support_from_hub_m'],sample['support_from_hub_m'])
        self.assertTrue(all(not g['samples'] for m in report['models'][88:] for g in m['draw_slots']))


if __name__=='__main__':
    unittest.main()
