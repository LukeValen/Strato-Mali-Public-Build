#!/usr/bin/env python3
"""Verify the optional Ko-fi welcome dialog cannot interrupt first-run setup."""
import unittest
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1] / "app/src/main"
JAVA = ROOT / "java/org/stratoemu/strato/MainActivity.kt"
RES = ROOT / "res"
ANDROID = "{http://schemas.android.com/apk/res/android}"


class KofiStartupTest(unittest.TestCase):
    def test_first_run_setup_is_not_interrupted(self):
        code = JAVA.read_text()
        self.assertIn("if (appSettings.searchLocation.isBlank()) {", code)
        setup, normal = code.split("if (appSettings.searchLocation.isBlank()) {", 1)[1].split("loadRoms(!appSettings.refreshRequired)", 1)
        self.assertIn("setupCallback.launch(", setup)
        self.assertNotIn("showKofiSupportDialog()", setup)
        self.assertIn("if (savedInstanceState == null)", normal)
        self.assertIn("showKofiSupportDialog()", normal)
        self.assertIn("if (!isFinishing && !isDestroyed)", normal)

    def test_external_link_and_dismiss(self):
        code = JAVA.read_text()
        self.assertIn("https://ko-fi.com/lukevalen/donate", code)
        self.assertIn("Intent.ACTION_VIEW", code)
        self.assertIn("R.id.kofi_not_now", code)
        self.assertIn("dialog.dismiss()", code)

    def test_blue_dialog_and_english_labels(self):
        layout = ET.parse(RES / "layout/mali_kofi_dialog.xml").getroot()
        bg = ET.parse(RES / "drawable/mali_kofi_dialog_background.xml").getroot()
        strings = ET.parse(RES / "values/strings.xml").getroot()
        names = {v.attrib.get("name"): (v.text or "") for v in strings.findall("string")}
        self.assertEqual(names["mali_kofi_button"], "Ko-fi")
        self.assertEqual(names["mali_kofi_not_now"], "Not now")
        self.assertIn("coffee", names["mali_kofi_message"])
        self.assertTrue(any(v.attrib.get(ANDROID + "id") == "@+id/kofi_button" for v in layout.iter()))
        self.assertTrue(any(v.attrib.get(ANDROID + "id") == "@+id/kofi_not_now" for v in layout.iter()))
        gradient = bg.find("gradient")
        self.assertIsNotNone(gradient)
        self.assertTrue(gradient.attrib[ANDROID + "endColor"].startswith("#17"))


if __name__ == "__main__":
    unittest.main()
