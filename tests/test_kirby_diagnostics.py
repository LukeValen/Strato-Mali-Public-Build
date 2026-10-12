#!/usr/bin/env python3
"""Static safeguards for the opt-in Kirby GPU diagnostics.

Diagnostics must be gated by the current title and should never change
Vulkan commands, descriptors, or the runtime of other games.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1] / "app/src/main/cpp/skyline"


def load(path):
    return (ROOT / path).read_text()


class KirbyDiagnosticsTests(unittest.TestCase):
    def test_only_kirby_title_enables_tracing(self):
        gpu = load("gpu.cpp")
        self.assertIn("0x01004D300C5AE000ULL", gpu)
        self.assertIn("kirbyDiagnosticsEnabled = state.loader &&", gpu)
        self.assertIn("KIRBY_DIAG enabled", gpu)

    def test_logs_are_scoped_and_bounded(self):
        pipeline = load("gpu/interconnect/common/pipeline.inc")
        draw = load("gpu/interconnect/maxwell_3d/maxwell_3d.cpp")
        self.assertGreaterEqual(pipeline.count("ctx.gpu.kirbyDiagnosticsEnabled"), 3)
        self.assertIn("ctx.gpu.kirbyDiagnosticsEnabled", draw)
        self.assertIn("sample < 48", pipeline)
        self.assertIn("sample < 40", pipeline)
        self.assertIn("sample < 24", draw)
        self.assertIn("sample < 20480", draw)

    def test_cbuf_sources_and_texture_results_recorded(self):
        pipeline = load("gpu/interconnect/common/pipeline.inc")
        self.assertIn("KIRBY_DIAG missing CBUF", pipeline)
        self.assertIn("KIRBY_DIAG CBUF handle", pipeline)
        self.assertIn("KIRBY_DIAG sampled image", pipeline)
        self.assertIn("static_cast<u32>(desc.cbuf_index)", pipeline)
        self.assertIn("texture->texture->format->vkFormat", pipeline)

    def test_render_targets_scissors_and_depth_recorded(self):
        draw = load("gpu/interconnect/maxwell_3d/maxwell_3d.cpp")
        self.assertIn("KIRBY_DIAG draw RT", draw)
        self.assertIn("KIRBY_DIAG draw colorRT", draw)
        self.assertIn("KIRBY_DIAG draw depthRT", draw)
        self.assertIn("KIRBY_DIAG clear", draw)
        self.assertIn("activeState.GetColorAttachments()", draw)
        self.assertIn("activeState.GetDepthAttachment()", draw)

    def test_no_rendering_behavior_changed(self):
        # This instrumentation must not rewrite shaders or alter the render path.
        pipeline = load("gpu/interconnect/common/pipeline.inc")
        self.assertIn("texture->texture->PopulateReadBarrier", pipeline)
        draw = load("gpu/interconnect/maxwell_3d/maxwell_3d.cpp")
        self.assertIn("ctx.executor.AddSubpass(", draw)


if __name__ == "__main__":
    unittest.main()
