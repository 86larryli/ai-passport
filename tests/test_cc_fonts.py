#!/usr/bin/env python3
"""Static glyph-coverage gate for the Claude Code Companion fonts.

Runs without FreeType or ESP-IDF: it parses the committed generated fonts
(assets/fonts/cc_font_*.c) with the generator's own reader and checks:
  - descriptor/bitmap/cmap structure matches what LVGL's PLAIN reader expects,
  - the dynamic-content font covers every GB2312 hanzi (the documented contract),
  - every non-ASCII character in the firmware's string literals is covered,
  - a known-absent code point stays absent, so the checks cannot pass vacuously.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from gen_lvgl_font import SETS, parse_c  # noqa: E402

FONTS = ROOT / "assets" / "fonts"
MAX_BITMAP = (1 << 20) - 1


def load(name: str):
    return parse_c((FONTS / f"{name}.c").read_text(encoding="utf-8"))


def c_string_literals(text: str) -> list[str]:
    """Decode the C string literals of a source file into Python strings."""
    out = []
    for raw in re.findall(r'"((?:[^"\\\n]|\\.)*)"', text):
        data = bytearray()
        i = 0
        while i < len(raw):
            ch = raw[i]
            if ch == "\\" and i + 1 < len(raw):
                nxt = raw[i + 1]
                if nxt == "x":
                    match = re.match(r"[0-9A-Fa-f]{1,2}", raw[i + 2:])
                    data.append(int(match.group(0), 16))
                    i += 2 + len(match.group(0))
                    continue
                if nxt == "u":
                    data += chr(int(raw[i + 2:i + 6], 16)).encode("utf-8")
                    i += 6
                    continue
                data += {"n": b"\n", "t": b"\t", "\\": b"\\", '"': b'"', "0": b"\0"}.get(nxt, nxt.encode())
                i += 2
                continue
            data += ch.encode("utf-8")
            i += 1
        out.append(data.decode("utf-8", errors="strict"))
    return out


class FontStructureTests(unittest.TestCase):
    def test_each_font_is_well_formed(self):
        for path in sorted(FONTS.glob("cc_font_*.c")):
            with self.subTest(font=path.name):
                font = parse_c(path.read_text(encoding="utf-8"))
                self.assertEqual(font.bpp, 4)
                self.assertLessEqual(len(font.bitmap), MAX_BITMAP)
                self.assertEqual(font.dsc[0], (0, 0, 0, 0, 0, 0))
                types = [c["type"] for c in font.cmaps]
                self.assertTrue(set(types) <= {"LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY",
                                               "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY"}, types)
                self.assertEqual(font.unicode_list, sorted(set(font.unicode_list)))
                # Glyph bitmaps are contiguous, byte aligned, and inside the array.
                for gid in range(1, len(font.dsc)):
                    index, _adv, w, h, _ox, _oy = font.dsc[gid]
                    end = font.dsc[gid + 1][0] if gid + 1 < len(font.dsc) else len(font.bitmap)
                    self.assertEqual(end - index, (w * h + 1) // 2, f"glyph {gid}")
                ids = {font.lookup(cp) for cp in font.codepoints()}
                self.assertEqual(ids, set(range(1, len(font.dsc))))
                self.assertGreater(font.line_height, font.base_line)


class CoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.body = load("cc_font_cjk_16")
        cls.small = load("cc_font_ui_12")
        cls.title = load("cc_font_ui_20")
        cls.num = load("cc_font_num_40")

    def test_dynamic_font_covers_gb2312_hanzi_and_replacement_mark(self):
        cps = self.body.codepoints()
        hanzi = [cp for cp in SETS["gb2312"]() if 0x4E00 <= cp <= 0x9FFF]
        self.assertEqual(len(hanzi), 6763)
        missing = [f"U+{cp:04X}" for cp in hanzi if cp not in cps]
        self.assertEqual(missing, [])
        for cp in (0x25A1, 0x2026, 0x3000, 0x3002, 0xFF0C, 0x201C, 0x201D):
            self.assertIn(cp, cps, f"U+{cp:04X}")
        self.assertTrue(all(cp in cps for cp in range(0x20, 0x7F)))
        # Hanzi glyphs actually contain ink.
        self.assertTrue(any(self.body.decode(self.body.lookup(0x4E2D))))

    def test_known_absent_code_points_stay_absent(self):
        # U+9F98 is outside GB2312; emoji are outside every subset. If these ever
        # resolve, the checks above would no longer prove anything.
        self.assertEqual(self.body.lookup(0x9F98), 0)
        self.assertEqual(self.body.lookup(0x1F600), 0)
        self.assertEqual(self.small.lookup(0x4E2D), 0)

    def test_firmware_literals_are_covered(self):
        literal_cps: set[int] = set()
        for source in sorted((ROOT / "main").glob("*.c")):
            for literal in c_string_literals(source.read_text(encoding="utf-8")):
                literal_cps.update(ord(ch) for ch in literal if ord(ch) >= 0x80)
        self.assertTrue(literal_cps, "expected some non-ASCII UI symbols")
        # Small and title labels fall back to the body font; body labels use it directly.
        chain = self.small.codepoints() | self.body.codepoints()
        missing = sorted(f"U+{cp:04X}" for cp in literal_cps if cp not in chain)
        self.assertEqual(missing, [])
        # Symbols used in fixed hints must exist in the 12 px font itself, so the
        # hint line keeps a single size and baseline.
        for cp in literal_cps:
            if cp in (0x25B2, 0x25BC):
                self.assertIn(cp, self.small.codepoints(), f"U+{cp:04X}")

    def test_passkey_digits(self):
        for ch in "0123456789 :":
            self.assertNotEqual(self.num.lookup(ord(ch)), 0, ch)
        self.assertTrue(all(cp in self.title.codepoints() for cp in range(0x20, 0x7F)))


if __name__ == "__main__":
    unittest.main()
