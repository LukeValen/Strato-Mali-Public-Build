#!/usr/bin/env python3
"""Visual-only UI regression checks: never replace the game library itself."""
from pathlib import Path
import struct
import subprocess
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
RES = ROOT / "app/src/main/res"
JAVA = ROOT / "app/src/main/java/org/stratoemu/strato/settings"
ANDROID = "{http://schemas.android.com/apk/res/android}"


class MaliUIConsistencyTests(unittest.TestCase):
    def test_launcher_icon_opaque_and_in_app_logo_transparent(self):
        logo = RES / "drawable/strato_mali_logo_transparent.png"
        raw = logo.read_bytes()
        self.assertTrue(raw.startswith(b"\x89PNG\r\n\x1a\n"))
        width, height, depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", raw[16:29])
        self.assertEqual((width, height, depth, color_type), (1536, 1536, 8, 6))
        # The launcher uses the user's glossy four-orb asset, kept opaque on
        # black; the in-app header deliberately retains transparent RGBA PNG.
        import hashlib
        launcher = (RES / "drawable/strato_mali_launcher_exact.webp").read_bytes()
        self.assertTrue(launcher.startswith(b"RIFF"))
        self.assertEqual(launcher[8:12], b"WEBP")
        self.assertEqual(hashlib.sha256(launcher).hexdigest(),
                         "f7e4c3edf580645df18d3f1c158d65e08d1cb9feecce85e46f63b1608a9135bd")
        manifest = ET.parse(ROOT / "app/src/main/AndroidManifest.xml").getroot()
        app = manifest.find("application")
        self.assertEqual(app.attrib[ANDROID + "icon"], "@drawable/strato_mali_launcher_exact")
        self.assertEqual(app.attrib[ANDROID + "roundIcon"], "@drawable/strato_mali_launcher_exact")

    def test_library_layout_changes_only_icon(self):
        layout = ROOT / "app/src/main/res/layout/main_activity.xml"
        live = layout.read_text()
        # SHA256 of the unchanged library XML from before this design patch.
        # Use a fixed baseline, because CI tests the committed HEAD.
        import hashlib
        original = live.replace(
            'android:src="@drawable/strato_mali_logo_transparent"',
            'android:src="@drawable/strato_mali_logo_separada_azul"')
        self.assertEqual(hashlib.sha256(original.encode()).hexdigest(),
                         "046a0e164310ec75955e26429d22f65c6fbfd00e50f5f8f34ba492f4773f5a1c")
        self.assertIn('android:id="@+id/featured_panel"', live)
        self.assertIn('android:id="@+id/search_icon"', live)

    def test_setup_functional_buttons_preserved(self):
        doc = ET.parse(RES / "layout/setup_activity.xml").getroot()
        ids = {element.attrib.get(ANDROID + "id") for element in doc.iter()}
        for name in ("keys_button", "firmware_button", "games_button", "start_button",
                     "back_button", "keys_status", "firmware_status", "games_status", "progress", "status"):
            self.assertIn("@+id/" + name, ids)
        self.assertEqual(doc.attrib[ANDROID + "background"], "@drawable/mali_setup_background")
        self.assertTrue(any(x.attrib.get(ANDROID + "src") == "@drawable/strato_mali_logo_transparent"
                            for x in doc.iter()))

    def test_settings_tabs_and_fragment_inflation_preserved(self):
        doc = ET.parse(RES / "layout/settings_activity.xml").getroot()
        ids = {x.attrib.get(ANDROID + "id") for x in doc.iter()}
        self.assertTrue({"@+id/settings", "@+id/settings_header", "@+id/settings_tabs",
                         "@+id/section_description", "@+id/titlebar"}.issubset(ids))
        self.assertTrue(any(x.attrib.get(ANDROID + "src") == "@drawable/strato_mali_logo_transparent"
                            for x in doc.iter()))
        for frag in ("GlobalSettingsFragment.kt", "GameSettingsFragment.kt"):
            self.assertIn("MaliPreferenceCards(requireContext())", (JAVA / frag).read_text())

    def test_drawable_resources_well_formed(self):
        for file in ("mali_settings_header_background.xml", "mali_settings_background.xml",
                     "mali_setup_background.xml"):
            ET.parse(RES / "drawable" / file)
        for file in ("setup_button_background.xml", "setup_button_text.xml"):
            ET.parse(RES / "color" / file)


if __name__ == "__main__":
    unittest.main()
