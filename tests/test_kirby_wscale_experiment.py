#!/usr/bin/env python3
"""Guardrails for a reversible Kirby-only packed WScale A/B experiment."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/skyline"
SHADER = ROOT / "app/libraries/shader-compiler"


class KirbyWscaleExperimentTests(unittest.TestCase):
    def test_only_kirby_program_id_enables_patch(self):
        gpu = (CPP / "gpu.cpp").read_text()
        self.assertIn("state.loader->programId == 0x01004D300C5AE000ULL", gpu)
        self.assertIn("kirbyDiagnosticsEnabled ? \"_kirby_wscale_ab1\" : \"\"", gpu)
        manager = (CPP / "gpu/shader_manager.cpp").read_text()
        self.assertIn("kirby_packed_wscale_test.store(gpu.kirbyDiagnosticsEnabled", manager)

    def test_shared_flag_cross_translation_units(self):
        settings = (CPP.parent / "shader_compiler_overrides/shader_compiler/common/settings.h").read_text()
        self.assertIn("inline std::atomic<bool> kirby_packed_wscale_test{false}", settings)
        self.assertNotIn("static inline std::atomic<bool> kirby_packed_wscale_test", settings)

    def test_baseline_remains_exact_for_other_games(self):
        s2r = (SHADER / "frontend/maxwell/translate/impl/move_special_register.cpp").read_text()
        self.assertEqual(s2r.count("return ir.Imm32(0x00ff0000u);"), 2)
        self.assertEqual(s2r.count("return ir.Imm32(Common::BitCast<u32>(1.0f));"), 2)
        self.assertEqual(s2r.count("kirby_packed_wscale_test.load(std::memory_order_relaxed)"), 2)

    def test_shader_cache_isolated(self):
        gpu = (CPP / "gpu.cpp").read_text()
        self.assertIn('"vk_graphics_pipeline_cache/" + shaderCacheKey', gpu)
        self.assertIn('"graphics_pipeline_cache/" + shaderCacheKey', gpu)

    def test_submodule_patch_is_packaged_for_actions(self):
        patch = (ROOT / "experiments/kirby-wscale-ab1/shader-compiler.patch").read_text()
        workflow = (ROOT / ".github/workflows/strato-mali-build.yml").read_text()
        self.assertIn("frontend/maxwell/translate/impl/move_special_register.cpp", patch)
        self.assertNotIn("a/common/settings.h", patch)
        self.assertIn("kirby_packed_wscale_test", (CPP.parent / "shader_compiler_overrides/shader_compiler/common/settings.h").read_text())
        self.assertIn("git -C app/libraries/shader-compiler apply", workflow)
        self.assertLess(workflow.index("Apply isolated Kirby shader experiment"),
                        workflow.index("Validate isolated Kirby WScale experiment"))

    def test_shader_hashes_traced_for_rt_correlation(self):
        pm = (CPP / "gpu/interconnect/maxwell_3d/pipeline_manager.h").read_text()
        draw = (CPP / "gpu/interconnect/maxwell_3d/maxwell_3d.cpp").read_text()
        self.assertIn("const auto &GetShaderHashes() const", pm)
        self.assertIn("KIRBY_EXPERIMENT pipeline", draw)
        self.assertIn("if (ctx.gpu.kirbyDiagnosticsEnabled)", draw)


if __name__ == "__main__":
    unittest.main()
