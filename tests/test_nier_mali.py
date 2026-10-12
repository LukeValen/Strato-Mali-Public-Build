#!/usr/bin/env python3
"""NieR GPU crash diagnostic and geometry fallback regression guardrails."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "app/src/main/cpp/skyline"
SHADER = ROOT / "app/libraries/shader-compiler"

class NierGpuTests(unittest.TestCase):
    def test_title_guard_does_not_affect_other_games(self):
        gpu = (SRC / "gpu.cpp").read_text()
        self.assertIn("0x0100B8E016F76000ULL", gpu)
        self.assertIn("traits.vendorId == 0x13B5U", gpu)
        self.assertIn("nierDiagnosticsEnabled = state.loader", gpu)
        self.assertIn('"_nier_mali_geom_invocation_v1"', gpu)
        manager = (SRC / "gpu/shader_manager.cpp").read_text()
        self.assertIn("nier_mali_geometry_invocation_fix.store(gpu.nierDiagnosticsEnabled", manager)
        settings = (SRC.parent / "shader_compiler_overrides/shader_compiler/common/settings.h").read_text()
        self.assertIn("inline std::atomic<bool> nier_mali_geometry_invocation_fix{false}", settings)

    def test_geometry_primitive_counts_not_stubbed(self):
        shader = (SHADER / "backend/spirv/emit_spirv_context_get_set.cpp").read_text()
        start = shader.index("Id EmitInvocationInfo(")
        end = shader.index("Id EmitSampleId(", start)
        body = shader[start:end]
        self.assertIn("case Stage::Geometry:", body)
        for name, count in [("Points", 1), ("Lines", 2), ("LinesAdjacency", 4), ("Triangles", 3),
                            ("TrianglesAdjacency", 6)]:
            self.assertIn("InputTopology::" + name + ": vertices = " + str(count), body)
        self.assertIn("ctx.Const(vertices)", body)
        self.assertIn("nier_mali_geometry_invocation_fix.load(std::memory_order_relaxed)", body)
        # Original fallback must remain for all non-NieR shaders.
        self.assertIn('LOG_WARNING(Shader, "(STUBBED) called");', body)
        self.assertEqual(body.count("return ctx.Const(0x00ff0000u);"), 2)

    def test_build_applies_patch_to_pinned_shader_submodule(self):
        workflow = (ROOT / ".github/workflows/strato-mali-build.yml").read_text()
        patch = (ROOT / "experiments/nier-automata/geometry-invocation-info.patch").read_text()
        self.assertIn("case Stage::Geometry:", patch)
        self.assertIn("Fix geometry InvocationInfo for NieR compatibility", workflow)
        self.assertLess(workflow.index("Apply isolated Kirby shader experiment"),
                        workflow.index("Fix geometry InvocationInfo for NieR compatibility"))
        self.assertLess(workflow.index("Fix geometry InvocationInfo for NieR compatibility"),
                        workflow.index("Validate NieR geometry and Mali diagnostic isolation"))

    def test_gpu_crash_breadcrumbs_and_bcn_tracing(self):
        record = (SRC / "gpu/interconnect/command_executor.cpp").read_text()
        header = (SRC / "gpu/interconnect/command_executor.h").read_text()
        self.assertIn("currentNodeType = node.index()", record)
        self.assertIn("NIER_DIAG command-record fault", record)
        self.assertIn('if (gpu.nierDiagnosticsEnabled) {\n                currentNodeType = node.index();', record)
        self.assertIn("currentNodeIndex{}", header)
        self.assertIn("NIER_DIAG BCn fallback", (SRC / "gpu/texture/texture.cpp").read_text())
        self.assertIn("NIER_CAPTURE shader", (SRC / "gpu/shader_manager.cpp").read_text())

if __name__ == "__main__":
    unittest.main()
