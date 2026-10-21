#!/usr/bin/env python3
"""Guard NieR performance release: preserve visual fixes but remove expensive telemetry."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
GPU=ROOT/"app/src/main/cpp/skyline"
def source(p):
    return (GPU/p).read_text()
class NierReleaseTest(unittest.TestCase):
    def test_release_version(self):
        gradle=(ROOT/"app/build.gradle").read_text()
        self.assertIn('versionName "0.0.5 RC3 fix"',gradle)
        self.assertIn("versionCode 504", gradle)
        self.assertIn("buildConfigField 'String', 'RELEASE_VERSION', '\"0.0.5 RC3 fix\"'", gradle)

    def test_patch_remains_active_without_capture(self):
        gpu=source("gpu.cpp")
        mgr=source("gpu/shader_manager.cpp")
        self.assertIn("nierDiagnosticsEnabled = state.loader",gpu)
        self.assertIn("traits.vendorId == 0x13B5U",gpu)
        self.assertIn("nierCaptureEnabled = std::filesystem::exists(",gpu)
        self.assertIn('"nier_gpu_capture.enable"',gpu)
        self.assertIn("Shader::Settings::nier_mali_geometry_invocation_fix.store(gpu.nierDiagnosticsEnabled",mgr)
        self.assertIn('hash == 0xF4215778B3A6FB13ULL',mgr)
        self.assertIn("nier_bloom::SuppressBrightPass(nierBloomPatched)",mgr)
        self.assertIn("_nier_mali_geom_invocation_v1_bloom_ab1",gpu)
        # Pre-shader setup must not dump or write to storage in normal gameplay.
        self.assertIn("if (gpu.nierCaptureEnabled) {",mgr)
        self.assertIn("if (nierCaptureEnabled) {",gpu)

    def test_hot_paths_not_logging_on_every_frame(self):
        for file in (
            "gpu/interconnect/maxwell_3d/maxwell_3d.cpp",
            "gpu/interconnect/kepler_compute/kepler_compute.cpp",
            "gpu/interconnect/common/state_updater.h",
            "gpu/interconnect/command_executor.cpp",
            "gpu/texture/texture.cpp",
        ):
            with self.subTest(file=file):
                content=source(file)
                self.assertIn("nierCaptureEnabled",content)
                self.assertNotIn("nierDiagnosticsEnabled",content)

    def test_compatibility_uses_reviewed_site_reports(self):
        game=(ROOT/"app/src/main/java/org/stratoemu/strato/data/GameCompatibility.kt").read_text()
        repository=(ROOT/"app/src/main/java/org/stratoemu/strato/data/GameCompatibilityRepository.kt").read_text()
        self.assertNotIn('"0100B8E016F76000" to Playable',game)
        self.assertNotIn('"01007EF00011E000" to Unplayable',game)
        self.assertIn('"Nothing", R.color.compat_nothing',game)
        self.assertIn('"Unplayable", R.color.compat_unplayable',game)
        self.assertIn('fun fromSiteStatus(status: String)',game)
        self.assertIn('Strato-Mali-Compatibility/data/reviews.json',repository)
        self.assertNotIn('filter { it.gpuModel == gpu }',repository)
        self.assertIn('?: GameCompatibility.Unknown',repository)
if __name__=="__main__":
    unittest.main()
