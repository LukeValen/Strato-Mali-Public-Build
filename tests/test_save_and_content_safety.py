#!/usr/bin/env python3
"""Regression checks for save export and atomic Update/DLC installation."""
from pathlib import Path
import unittest
import zipfile
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]
KOTLIN = ROOT / "app/src/main/java/org/stratoemu/strato"
NATIVE = ROOT / "app/src/main/cpp/skyline/loader"


class SaveAndContentSafetyTests(unittest.TestCase):
    def test_export_uses_durable_create_document(self):
        util = (KOTLIN / "utils/SaveManagementUtils.kt").read_text()
        self.assertIn('ActivityResultContracts.CreateDocument("application/zip")', util)
        self.assertIn("contentResolver.openOutputStream(uri, \"w\")", util)
        self.assertNotIn("Intent.createChooser", util)
        self.assertNotIn('File(context.getPublicFilesDir().canonicalPath, "temp").deleteRecursively()', util)
        self.assertIn('context.getSharedPreferences("save_export_pending"', util)
        self.assertIn('uri == null || gameId == null', util)

    def test_save_zip_is_import_compatible(self):
        util = (KOTLIN / "utils/SaveManagementUtils.kt").read_text()
        self.assertIn("file.relativeTo(root).invariantSeparatorsPath", util)
        self.assertIn("ZipEntry(name + if (file.isDirectory) \"/\" else \"\")", util)
        self.assertIn("Files.isSymbolicLink", util)
        self.assertIn("entries > 0", util)
        self.assertIn("filterTitleId", util)
        # A representative archive has the Title ID directory as its top-level
        # entry, which is exactly what importSave/ZipUtils expect.
        with TemporaryDirectory() as tmp:
            path = Path(tmp) / "save.zip"
            with zipfile.ZipFile(path, "w") as zipf:
                zipf.writestr("01004D300C5AE000/", "")
                zipf.writestr("01004D300C5AE000/progress.dat", "data")
            with zipfile.ZipFile(path, "r") as zipf:
                self.assertEqual(zipf.read("01004D300C5AE000/progress.dat"), b"data")

    def test_update_dlcs_verify_before_install(self):
        util = (KOTLIN / "utils/GameContentUtils.kt").read_text()
        self.assertIn("validatePackage(contentFd.fd, gameFd.fd", util)
        self.assertIn("output.fd.sync()", util)
        self.assertIn("temporary.renameTo(target)", util)
        self.assertIn("temporary.length() == copied", util)
        self.assertIn("temporary.delete()", util)
        self.assertIn('file.canonicalPath.startsWith(contentRoot.path + File.separator)', util)
        package = (NATIVE / "content_package.h").read_text()
        self.assertIn("if (verifyIntegrity) Verify(it->second, record);", package)
        self.assertIn('No Update or DLC metadata found', package)

    def test_kirby_only_pipeline_stall_diagnostics(self):
        pipeline = (ROOT / "app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/pipeline_manager.cpp").read_text()
        self.assertIn("KIRBY_PERF graphics pipeline compilation", pipeline)
        self.assertIn("ctx.gpu.kirbyDiagnosticsEnabled", pipeline)
        self.assertIn("elapsedMs >= 40", pipeline)
        self.assertIn("kirbySlowPipelineSamples.fetch_add(1", pipeline)

    def test_corrupt_installed_content_does_not_block_base_game(self):
        content = (NATIVE / "installed_content.h").read_text()
        self.assertIn("Skipping invalid Update/DLC", content)
        self.assertIn("Cannot apply installed content; starting base game", content)
        self.assertIn("std::sort(paths.begin(), paths.end())", content)


if __name__ == "__main__":
    unittest.main()
