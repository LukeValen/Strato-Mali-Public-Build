#!/usr/bin/env python3
"""Static guards for NieR-only bounded shader/RT capture, no guest rendering changes."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "app/src/main/cpp/skyline"

class NierCaptureTests(unittest.TestCase):
    def test_exact_game_and_vendor_gate(self):
        gpu = (SRC / "gpu.cpp").read_text()
        self.assertIn("state.loader->programId == 0x0100B8E016F76000ULL", gpu)
        self.assertIn("traits.vendorId == 0x13B5U", gpu)

    def test_shader_dump_safety(self):
        manager = (SRC / "gpu/shader_manager.cpp").read_text()
        self.assertIn("if (!gpu.nierCaptureEnabled || binary.empty())", manager)
        self.assertIn("if (!nierCaptureAvailable) return;", manager)
        self.assertIn("96ULL * 1024 * 1024", manager)
        self.assertIn("MaxFiles = 1600", manager)
        self.assertIn("MaxFileBytes = 1024ULL * 1024", manager)
        self.assertIn('"nier_mali_capture_{}"', manager)
        self.assertIn('CaptureNierBinary(name, spirv.cast<u8>())', manager)
        self.assertIn("gpu.nierCaptureEnabled && hash", manager)
        self.assertIn("if (DumpShaders)", manager)
        self.assertIn("nierCaptureAvailable = false", manager)

    def test_diagnostic_metadata(self):
        shader = (SRC / "gpu/shader_manager.cpp").read_text()
        for flag in ("uses_subgroup_vote", "uses_subgroup_shuffles", "uses_subgroup_mask",
                     "uses_rescaling_uniform", "uses_derivatives", "imageReads", "imageWrites"):
            self.assertIn(flag, shader)
        draw = (SRC / "gpu/interconnect/maxwell_3d/maxwell_3d.cpp").read_text()
        self.assertIn("if (ctx.gpu.nierCaptureEnabled)", draw)
        self.assertIn("NIER_CAPTURE draw={} shaderSlot={}", draw)
        self.assertIn("NIER_CAPTURE draw={} colorRT={}", draw)
        self.assertIn("NIER_CAPTURE draw={} sampled={}", draw)
        compute = (SRC / "gpu/interconnect/kepler_compute/kepler_compute.cpp").read_text()
        self.assertIn("if (ctx.gpu.nierCaptureEnabled)", compute)
        self.assertIn("NIER_CAPTURE dispatch={}", compute)

    def test_does_not_restore_failed_isberd_patch(self):
        self.assertFalse((ROOT / "experiments/nier-automata/isberd-nier-ab1.patch").exists())
        workflow = (ROOT / ".github/workflows/strato-mali-build.yml").read_text()
        self.assertIn("python3 tests/test_nier_capture.py", workflow)
        self.assertNotIn("isberd-nier-ab1", workflow)
        self.assertNotIn("shader-compiler apply \"$GITHUB_WORKSPACE/experiments/nier-automata/isberd", workflow)

if __name__ == "__main__":
    unittest.main()
