#!/usr/bin/env python3
"""Regression checks for the site-backed compatibility badges and report action."""
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
    def test_site_statuses_and_no_local_ratings(self):
        source=(SRC/"data/GameCompatibility.kt").read_text()
        for slug in ("playable", "ingame", "boots", "nothing", "unplayable"):
            self.assertIn('"'+slug+'" ->', source)
        self.assertNotIn("Perfect(", source)
        self.assertNotIn("byTitleId", source)
        repository=(SRC/"data/GameCompatibilityRepository.kt").read_text()
        self.assertIn("/data/reviews.json", repository)
        self.assertNotIn("gpuModel ==", repository)
        self.assertIn("GameCompatibility.Unknown", repository)
        self.assertNotIn("api.github.com", repository)

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
        for name in ("playable","ingame","boots","nothing","unplayable","unknown"):
            self.assertRegex(colors["compat_"+name],r"^#[0-9A-F]{6}$")
        adapter=(SRC/"adapter/AppViewItem.kt").read_text()
        self.assertEqual(adapter.count("override val compatibilityBadge = binding.compatibilityBadge"),3)
        self.assertIn("GameCompatibilityRepository.summaryFor(item.titleId)",adapter)
        self.assertIn("backgroundTintList = ColorStateList.valueOf",adapter)
        self.assertIn("compatibility.foregroundColor",adapter)
        strings=(RES/"values/strings.xml").read_text()
        self.assertIn('%2$s/5,0',strings)
        self.assertIn('Locale.forLanguageTag("pt-BR")',adapter)

    def test_report_action_prefills_game_and_title_id(self):
        dialog=(SRC/"AppDialog.kt").read_text()
        for parameter in ('"template"','"game-name"','"title-id"'):
            self.assertIn('appendQueryParameter('+parameter,dialog)
        self.assertIn('binding.submitCompatibility.isEnabled', dialog)
        layout=ET.parse(RES/"layout/app_dialog.xml").getroot()
        self.assertEqual(sum(el.attrib.get(ANDROID+"id")=="@+id/submit_compatibility" for el in layout.iter()),1)

if __name__=="__main__":
    unittest.main()
