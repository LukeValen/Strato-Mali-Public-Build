#!/usr/bin/env python3
"""Check that NieR's experimental ISBERD fix is isolated and CI-portable."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHADER = ROOT / "app/libraries/shader-compiler"

class NierIsberdExperimentTests(unittest.TestCase):
    def test_title_and_vendor_gating(self):
        cpp = (ROOT / "app/src/main/cpp/skyline/gpu.cpp").read_text()
        sm = (ROOT / "app/src/main/cpp/skyline/gpu/shader_manager.cpp").read_text()
        settings = (ROOT / "app/src/main/cpp/shader_compiler_overrides/shader_compiler/common/settings.h").read_text()
        self.assertIn("state.loader->programId == 0x0100B8E016F76000ULL", cpp)
        self.assertIn("traits.vendorId == 0x13B5U", cpp)
        self.assertIn("nier_mali_isberd_ab1.store(gpu.nierDiagnosticsEnabled", sm)
        self.assertIn("inline std::atomic<bool> nier_mali_isberd_ab1{false}", settings)
        self.assertIn("_nier_mali_geom_invocation_v1_isberd_ab1", cpp)

    def test_legacy_behavior_preserved(self):
        code = (SHADER / "frontend/maxwell/translate/impl/internal_stage_buffer_entry_read.cpp").read_text()
        self.assertIn("if (Shader::Settings::nier_mali_isberd_ab1.load", code)
        self.assertIn("Legacy behavior kept completely untouched for every other game.", code)
        self.assertIn('LOG_WARNING(Shader, "(STUBBED) called");', code)
        for token in ("LoadGlobalU8", "LoadGlobalU16", "LoadGlobal32",
                      "GetAttributeIndexed", "GetPatch", "GetAttribute"):
            self.assertIn(token, code)

    def test_build_step_applies_pinned_submodule_patch(self):
        workflow = (ROOT / ".github/workflows/strato-mali-build.yml").read_text()
        patch = (ROOT / "experiments/nier-automata/isberd-nier-ab1.patch").read_text()
        self.assertIn("nier_mali_isberd_ab1.load", patch)
        self.assertLess(workflow.index("Fix geometry InvocationInfo for NieR compatibility"),
                        workflow.index("Apply experimental NieR Mali ISBERD shader correction"))
        result = subprocess.run(["git","-C",str(SHADER),"apply","--reverse","--check",
                                 str(ROOT / "experiments/nier-automata/isberd-nier-ab1.patch")],
                                capture_output=True,text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

if __name__ == "__main__":
    unittest.main()
