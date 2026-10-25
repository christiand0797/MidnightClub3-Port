import importlib.util
from fractions import Fraction
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np
from PIL import Image

spec=importlib.util.spec_from_file_location("visual",Path(__file__).with_name("sample_visual_quality.py"))
visual=importlib.util.module_from_spec(spec);spec.loader.exec_module(visual)


class SamplingTests(unittest.TestCase):
    def test_strict_filter_never_adopts_legacy_or_pending(self):
        good={"movie":"reward/test.bik","status":"verified","output_validated":True,"recorded_model":"realesrgan-x4plus",
              "origin":"original-bink","historical_source_and_weights_verified":True}
        rows=[good,dict(good,movie="old.bik",historical_source_and_weights_verified=False),dict(good,movie="pending.bik",status="pending")]
        report={"requested_model":"realesrgan-x4plus","require_original_provenance":True,"details":rows}
        self.assertEqual(visual.strict_movies(report),["reward/test.bik"])
        report["require_original_provenance"]=False
        with self.assertRaises(ValueError):visual.strict_movies(report)

    def test_paths_reject_traversal_and_output_outside_local(self):
        for name in ("../test.bik","/test.bik","a\\test.bik","C:/test.bik","a//test.bik","test.mp4"):
            with self.subTest(name=name),self.assertRaises(ValueError):visual.safe_movie(name)
        with self.assertRaises(ValueError):visual.safe_output(visual.ROOT/"templates/game/assets")

    def test_matching_is_exact_rational_pts_not_unrelated_frame_numbers(self):
        a={"times":{Fraction(0):0,Fraction(333333,10_000_000):1},"durations":{Fraction(0):Fraction(333333,10_000_000),Fraction(333333,10_000_000):Fraction(333333,10_000_000)}}
        b={"times":{Fraction(0):0,Fraction(333333,10_000_000):333333},"durations":dict(a["durations"])}
        self.assertEqual(visual.matching_times(a,b),[Fraction(0),Fraction(333333,10_000_000)])
        b["times"]={Fraction(0):0,Fraction(1,30):1}
        with self.assertRaises(ValueError):visual.matching_times(a,b)

    def test_nonblack_and_roi_are_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/"source.png";Image.new("RGB",(720,480)).save(p)
            self.assertFalse(visual.meaningful(visual.nonblack_stats(p)))
            rgb=np.zeros((480,720,3),np.uint8);rgb[200:300,300:430]=np.indices((100,130))[1][...,None]%2*255
            image=Image.fromarray(rgb);image.save(p)
            self.assertTrue(visual.meaningful(visual.nonblack_stats(p)))
            x0,y0,x1,y1=visual.detail_box(image)
            self.assertEqual((x1-x0,y1-y0),(128,96));self.assertTrue(0<=x0<x1<=720 and 0<=y0<y1<=480)

    def test_selected_pts_are_distinct_and_on_observed_grid(self):
        times=[Fraction(i,30) for i in range(540)];used={times[108]}
        values=visual.candidates(times,Fraction(1,5),used)
        self.assertNotIn(times[108],values);self.assertTrue(set(values)<=set(times));self.assertEqual(len(values),len(set(values)))

    def test_decoder_pts_mismatch_fails_closed(self):
        completed=mock.Mock(stderr="[Parsed_showinfo] config in time_base: 1/30, frame_rate: 30/1\n[Parsed_showinfo] n:0 pts:11 pts_time:0.3",returncode=0)
        with mock.patch.object(visual.subprocess,"run",return_value=completed),self.assertRaisesRegex(ValueError,"PTS"):
            visual.extract(Path("ff"),Path("source.bik"),10,Fraction(1,30),Path("unused.png"))


if __name__=="__main__":unittest.main()
