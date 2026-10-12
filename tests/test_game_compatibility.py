#!/usr/bin/env python3
"""Static regression tests for manual Strato Mali compatibility badges."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[1]
APP=ROOT/"app/src/main"
SRC=APP/"java/org/stratoemu/strato"
RES=APP/"res"
ANDROID="{http://schemas.android.com/apk/res/android}"
APPNS="{http://schemas.android.com/apk/res-auto}"

class CompatibilityTests(unittest.TestCase):
    def test_curated_ids_and_levels(self):
        source=(SRC/"data/GameCompatibility.kt").read_text()
        for title_id, rating in {
            "01002B30028F6000":"Perfect",
            "01005CA01580E000":"Perfect",
            "01004B10157F2000":"Perfect",
            "0100B880154FC000":"Perfect",
            "010062B01525C000":"Playable",
            "0100B8E016F76000":"Playable",
            "010003F003A34000":"Perfect",
            "01007EF00011E000":"Unplayable",
        }.items():
            self.assertRegex(source, r'"'+title_id+r'"\s+to\s+'+rating)
        self.assertIn("return byTitleId[id] ?: Unknown",source)
        self.assertIn("return withoutTitleId[normalizedTitle] ?: Unknown",source)
        self.assertIn("id.matches(Regex(\"[0-9A-F]{16}\"))", source)
        self.assertNotIn("contains(",source)

    def test_all_library_layouts_have_one_overlay(self):
        for name in ("app_item_grid","app_item_grid_compact","app_item_linear"):
            with self.subTest(name=name):
                root=ET.parse(RES/"layout"/f"{name}.xml").getroot()
                badges=[t for t in root.iter() if t.attrib.get(ANDROID+"id")=="@+id/compatibility_badge"]
                self.assertEqual(len(badges),1)
                self.assertEqual(badges[0].attrib.get(ANDROID+"clickable"),"false")
                self.assertEqual(badges[0].attrib.get(ANDROID+"focusable"),"false")
        root=ET.parse(RES/"layout/app_item_grid.xml").getroot()
        frame=root.find(".//FrameLayout")
        self.assertIsNotNone(frame)
        self.assertEqual([v.tag for v in frame],["ImageView","TextView"])
        compact=ET.parse(RES/"layout/app_item_grid_compact.xml").getroot()
        title=next(x for x in compact.iter() if x.attrib.get(ANDROID+"id")=="@+id/text_title")
        self.assertEqual(title.attrib.get(APPNS+"layout_constraintBottom_toTopOf"),"@id/compatibility_badge")

    def test_palette_and_recycler_rebind(self):
        palette=ET.parse(RES/"values/colors.xml").getroot()
        colors={c.attrib["name"]:c.text for c in palette.findall("color")}
        for name in ("perfect","playable","ingame","nothing","unplayable","unknown"):
            self.assertRegex(colors["compat_"+name],r"^#[0-9A-F]{6}$")
        adapter=(SRC/"adapter/AppViewItem.kt").read_text()
        self.assertEqual(adapter.count("override val compatibilityBadge = binding.compatibilityBadge"),3)
        self.assertIn("GameCompatibility.fromGame(item.titleId, item.title)",adapter)
        self.assertIn("backgroundTintList = ColorStateList.valueOf",adapter)
        self.assertIn("compatibility.foregroundColor",adapter)

if __name__=="__main__":
    unittest.main()
