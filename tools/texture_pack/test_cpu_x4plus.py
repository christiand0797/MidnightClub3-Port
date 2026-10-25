import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np
from PIL import Image

spec=importlib.util.spec_from_file_location("cpu_x4plus",Path(__file__).with_name("cpu_x4plus.py"))
cpu=importlib.util.module_from_spec(spec);spec.loader.exec_module(cpu)


class FakeEngine:
    """File-publication test double only; real architecture/inference is exercised separately below."""
    def __init__(self,weights):
        self.calls=0
        self.metadata={"device":"cpu","intra_op_threads":2,"inter_op_threads":2,
                       "model_sha256":cpu.digest(weights),"module_sha256":cpu.digest(Path(cpu.__file__))}

    def upscale_rgb(self,rgb,**kwargs):
        self.calls+=1
        return np.repeat(np.repeat(rgb,4,axis=0),4,axis=1)


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix="cpu-x4plus-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.root=Path(self.temporary.name)
        self.patch=mock.patch.object(cpu,"ROOT",self.root);self.patch.start();self.addCleanup(self.patch.stop)
        self.inputs=self.root/".local/original-derived/network-input";self.inputs.mkdir(parents=True)
        self.source=self.inputs/"original.png"
        self.output=self.root/".local/candidates/network-output/original.png"
        rgba=np.random.default_rng(12).integers(0,256,(9,11,4),dtype=np.uint8)
        Image.fromarray(rgba).save(self.source)
        self.weights=self.root/"test-weights.pth";self.weights.write_bytes(b"publication-test-double")
        self.engine=FakeEngine(self.weights)

    def run_image(self,**kwargs):
        return cpu.upscale_original(self.source,self.output,self.weights,original_root=self.inputs,
            expected_source_sha256=kwargs.pop("expected",cpu.digest(self.source)),engine=self.engine,**kwargs)

    def test_source_pixels_alpha_and_provenance_roundtrip(self):
        source_bytes=self.source.read_bytes();record=self.run_image()
        with Image.open(self.source) as original,Image.open(self.output) as result:
            self.assertEqual(result.size,(44,36));self.assertEqual(result.mode,"RGBA")
            self.assertTrue(np.array_equal(np.asarray(result.getchannel("A")),np.asarray(
                original.getchannel("A").resize((44,36),Image.Resampling.LANCZOS))))
        self.assertEqual(self.source.read_bytes(),source_bytes)
        self.assertEqual(record["source_sha256"],cpu.digest(self.source))
        self.assertEqual(record["output_sha256"],cpu.digest(self.output))
        self.assertFalse(record["active_replacements_modified"])

    def test_changed_source_hash_never_runs_model(self):
        with self.assertRaisesRegex(ValueError,"pinned plan"):self.run_image(expected="0"*64)
        self.assertEqual(self.engine.calls,0);self.assertFalse(self.output.exists())

    def test_existing_candidate_is_preserved(self):
        self.output.parent.mkdir(parents=True);self.output.write_bytes(b"existing candidate")
        with self.assertRaisesRegex(ValueError,"existing outputs"):self.run_image()
        self.assertEqual(self.output.read_bytes(),b"existing candidate");self.assertEqual(self.engine.calls,0)

    def test_active_upscale_cannot_be_original_input(self):
        active=self.root/"templates/game/out/native/textures/replace";active.mkdir(parents=True)
        source=active/"existing.png";source.write_bytes(self.source.read_bytes())
        with self.assertRaisesRegex(ValueError,"active upscale"):
            cpu.validate_paths(source,self.output,active)

    def test_output_and_source_root_boundaries(self):
        for output in (self.root/"outside.png",self.source,self.inputs/"another.png"):
            with self.subTest(output=output),self.assertRaises(ValueError):
                cpu.validate_paths(self.source,output,self.inputs)
        with self.assertRaises(ValueError):cpu.validate_paths(self.source,self.output,self.root/"other")

    def test_model_hash_mismatch_fails_before_inference(self):
        self.weights.write_bytes(b"changed weights")
        with self.assertRaisesRegex(ValueError,"requested weights"):self.run_image()
        self.assertEqual(self.engine.calls,0)

    def test_device_threads_and_module_mismatch_fail_closed(self):
        for key,value in (("device","cuda"),("intra_op_threads",3),("inter_op_threads",3),("module_sha256","0"*64)):
            with self.subTest(key=key):
                engine=FakeEngine(self.weights);engine.metadata[key]=value
                with mock.patch.object(self,"engine",engine),self.assertRaisesRegex(ValueError,"provenance"):
                    self.run_image()
                self.assertEqual(engine.calls,0)

    def test_changed_source_during_inference_is_not_published(self):
        def inference(rgb,**kwargs):
            self.source.write_bytes(b"changed after decode")
            return np.repeat(np.repeat(rgb,4,axis=0),4,axis=1)
        self.engine.upscale_rgb=inference
        with self.assertRaisesRegex(ValueError,"changed during inference"):self.run_image()
        self.assertFalse(self.output.exists())

    def test_bad_dimensions_and_interruption_do_not_publish(self):
        self.engine.upscale_rgb=lambda rgb,**kwargs:rgb
        with self.assertRaisesRegex(ValueError,"dimensions"):self.run_image()
        self.assertFalse(self.output.exists())
        def interrupted(*args,**kwargs):raise InterruptedError("requested stop")
        self.engine.upscale_rgb=interrupted
        with self.assertRaises(InterruptedError):self.run_image()
        self.assertFalse(self.output.exists())

    def test_concurrent_candidate_cannot_be_overwritten(self):
        def concurrent(_temporary,destination):
            Path(destination).write_bytes(b"other validated candidate")
            raise FileExistsError("concurrent publication")
        with mock.patch.object(cpu.os,"link",side_effect=concurrent),self.assertRaises(FileExistsError):self.run_image()
        self.assertEqual(self.output.read_bytes(),b"other validated candidate")
        self.assertFalse(list(self.output.parent.glob(".cpu-x4plus-*")))

    def test_pixel_bound_rejects_before_model(self):
        with self.assertRaisesRegex(ValueError,"pixel bound"):self.run_image(max_pixels=1)
        self.assertEqual(self.engine.calls,0)


try:
    import torch
except ImportError:
    torch=None


@unittest.skipUnless(torch is not None and cpu.DEFAULT_WEIGHTS.is_file(),"real CPU model requires Python3.12 torch and local weights")
class RealModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Any request to auto-select CUDA is a failure; explicit CPU loading/inference must stand on its own.
        with mock.patch.object(torch.cuda,"is_available",side_effect=AssertionError("GPU query forbidden")):
            cls.engine=cpu.CpuX4Plus()

    def test_exact_state_key_shapes_and_cpu_limits(self):
        self.assertEqual(self.engine.metadata["validated_state_keys"],702)
        self.assertEqual(self.engine.metadata["parameters"],16697987)
        self.assertEqual(self.engine.metadata["state_key"],"params_ema")
        self.assertTrue(all(p.device.type=="cpu" and p.dtype==torch.float32 for p in self.engine.model.parameters()))
        self.assertLessEqual(torch.get_num_threads(),2);self.assertLessEqual(torch.get_num_interop_threads(),2)
        with self.assertRaises(ValueError):cpu.configure_cpu(torch,3)

    def test_real_whole_and_fully_padded_tiles_are_identical(self):
        rgb=np.random.default_rng(123).integers(0,256,(13,15,3),dtype=np.uint8)
        whole=self.engine.upscale_rgb(rgb,tile=32,tile_pad=16)
        tiled=self.engine.upscale_rgb(rgb,tile=8,tile_pad=16)
        self.assertEqual(whole.shape,(52,60,3));self.assertEqual(whole.dtype,np.uint8)
        self.assertTrue(np.array_equal(whole,tiled))

    def test_model_input_bounds_and_stop_before_tile(self):
        rgb=np.zeros((8,8,3),dtype=np.uint8)
        for kwargs in ({"tile":7},{"tile":97},{"tile_pad":33},{"max_pixels":2_000_000}):
            with self.subTest(kwargs=kwargs),self.assertRaises(ValueError):self.engine.upscale_rgb(rgb,**kwargs)
        with self.assertRaises(ValueError):self.engine.upscale_rgb(rgb.astype(np.float32))
        with tempfile.TemporaryDirectory() as tmp:
            stop=Path(tmp)/"stop";stop.write_text("user stop")
            with self.assertRaises(InterruptedError):self.engine.upscale_rgb(rgb,stop_file=stop)

    def test_weight_validation_rejects_incompatible_or_nonfinite_state(self):
        model=torch.nn.Linear(2,2);state=model.state_dict()
        accepted,key=cpu.validate_state(torch,model,{"params":state})
        self.assertIs(accepted,state);self.assertEqual(key,"params")
        with self.assertRaisesRegex(ValueError,"keys differ"):cpu.validate_state(torch,model,{"params":{"weight":state["weight"]}})
        for changed in (state["weight"].double(),torch.zeros((3,2)),torch.full((2,2),float("nan"))):
            bad=dict(state);bad["weight"]=changed
            with self.assertRaisesRegex(ValueError,"invalid x4plus tensor"):cpu.validate_state(torch,model,{"params":bad})
        with self.assertRaisesRegex(ValueError,"lacks"):cpu.validate_state(torch,model,{"not-weights":state})


if __name__=="__main__":unittest.main()
