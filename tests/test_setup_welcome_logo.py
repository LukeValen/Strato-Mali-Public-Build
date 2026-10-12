#!/usr/bin/env python3
"""Ensure the ADM PNG changes only the Strato Mali welcome artwork."""
from pathlib import Path
import struct
import xml.etree.ElementTree as ET
import unittest
ROOT=Path(__file__).resolve().parents[1]
RES=ROOT/"app/src/main/res"
ANDROID="{http://schemas.android.com/apk/res/android}"
class WelcomeImageTest(unittest.TestCase):
    def test_exact_setup_logo_only(self):
        layout=ET.parse(RES/"layout/setup_activity.xml").getroot()
        logos=[node for node in layout.iter() if node.attrib.get(ANDROID+"src")=="@drawable/mali_welcome_logo_adm"]
        self.assertEqual(len(logos),1)
        logo=logos[0]
        self.assertEqual(logo.tag,"ImageView")
        self.assertEqual(logo.attrib[ANDROID+"layout_width"],"152dp")
        self.assertEqual(logo.attrib[ANDROID+"layout_height"],"152dp")
        self.assertEqual(logo.attrib[ANDROID+"scaleType"],"fitCenter")
        for unchanged in ("main_activity.xml","settings_activity.xml"):
            content=(RES/"layout"/unchanged).read_text()
            self.assertIn("@drawable/strato_mali_logo_transparent",content)
            self.assertNotIn("mali_welcome_logo_adm",content)
    def test_png_integrity(self):
        image=RES/"drawable-nodpi/mali_welcome_logo_adm.png"
        content=image.read_bytes()
        self.assertEqual(content[:8],b"\x89PNG\r\n\x1a\n")
        self.assertEqual(struct.unpack_from(">II",content,16),(1254,1254))
        self.assertEqual((content[24],content[25]),(8,6)) # 8-bit RGBA
        self.assertGreater(len(content),10000)
if __name__=="__main__":
    unittest.main()
