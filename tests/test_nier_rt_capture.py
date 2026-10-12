#!/usr/bin/env python3
"""Guards against enabling NieR raw RT readback in any other game."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
S=ROOT/"app/src/main/cpp/skyline"
class Capture(unittest.TestCase):
    def test_only_target_and_driver(self):
        g=(S/"gpu.cpp").read_text()
        self.assertIn("0x0100B8E016F76000ULL",g)
        self.assertIn("traits.vendorId == 0x13B5U",g)
    def test_exact_shader_and_stage(self):
        d=(S/"gpu/interconnect/maxwell_3d/maxwell_3d.cpp").read_text()
        for phrase in ("0xF4215778B3A6FB13ULL","0x5739229BBF0DF100ULL",
                       "01_hdr_source","02_bright_pass","03_bloom_blur",
                       "nierDiagnosticsEnabled","std::chrono::seconds{20}"):
            self.assertIn(phrase,d)
    def test_safe_vk_layout_and_fence_readback(self):
        e=(S/"gpu/interconnect/command_executor.cpp").read_text()
        for phrase in ("eTransferSrcOptimal","copyImageToBuffer","previousLayout",
                       "waiterThread.Queue(cycle","eHostRead","MaxRawBytes",
                       "eB10G11R11UfloatPack32","std::ios::binary","vmaInvalidateAllocation"):
            self.assertIn(phrase,e)
    def test_no_isberd_and_preserve_ci(self):
        w=(ROOT/".github/workflows/strato-mali-build.yml").read_text()
        self.assertNotIn("isberd-nier-ab1",w)
        self.assertIn("tests/test_nier_rt_capture.py",w)
if __name__=="__main__":
    unittest.main()
