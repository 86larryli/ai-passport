#!/usr/bin/env python3
"""Generate an LVGL 9.5 bitmap font (PLAIN 4 bpp) from a TTF/OTF and verify it.

Why an in-repo generator instead of lv_font_conv: lv_font_conv 1.5.3 writes
corrupt bitmaps under current Node.js (see
docs/reference/shinku-chen/lvgl-cjk-font-subsets.md), and a large sparse CJK
subset must use the FORMAT0_TINY + SPARSE_TINY cmap layout LVGL can resolve.

The generator:
  1. rasterizes each requested code point with FreeType (freetype-py),
  2. quantizes coverage to 4 bpp and packs it the way LVGL's PLAIN reader
     decodes it (one continuous nibble stream per glyph, high nibble first,
     each glyph starting on a byte boundary, stride 0),
  3. writes a C source using one FORMAT0_TINY cmap for printable ASCII and one
     SPARSE_TINY cmap for everything else,
  4. re-parses the written file and checks every glyph pixel by pixel and every
     cmap lookup against the rasterization. Any mismatch fails the run.

Run it through uv so freetype-py stays out of the system Python, for example:

  uv run --no-project --with freetype-py==2.5.1 python -I tools/gen_lvgl_font.py \
      --font NotoSansSC-Medium.otf --size 16 --sets ascii,cjk-dynamic \
      --name cc_font_cjk_16 --line-height 20 --base-line 4 \
      --output assets/fonts/cc_font_cjk_16.c

The exact commands used for this application are recorded in assets/README.md.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

GENERATOR_VERSION = "1"
ASCII_FIRST = 0x20
ASCII_LAST = 0x7E
MAX_BITMAP_BYTES = (1 << 20) - 1  # lv_font_fmt_txt_glyph_dsc_t.bitmap_index is 20 bits.


# ---------------------------------------------------------------------------
# Character sets
# ---------------------------------------------------------------------------

def _gb2312() -> list[int]:
    """Every character GB2312 defines (symbols, kana, Greek, Cyrillic, 6763 hanzi)."""
    out = []
    for row in range(0xA1, 0xF8):
        for cell in range(0xA1, 0xFF):
            try:
                ch = bytes((row, cell)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            out.append(ord(ch))
    return out


def _span(first: int, last: int) -> list[int]:
    return list(range(first, last + 1))


# Punctuation and symbols that commonly appear in Claude Code output, paths,
# commands, and Chinese prose, beyond GB2312.
_EXTRA_SYMBOLS = (
    _span(0x00A0, 0x00FF)            # Latin-1 supplement
    + _span(0x2010, 0x2027)          # dashes, quotes, bullet, ellipsis
    + _span(0x2030, 0x203B)          # per mille, primes, guillemets, reference mark
    + _span(0x2190, 0x2199)          # arrows
    + [0x21B5, 0x21D2, 0x21D4]       # return, implies, iff
    + [0x2212, 0x2248, 0x2260, 0x2264, 0x2265, 0x221E, 0x22EF]
    + _span(0x2460, 0x2473)          # circled digits
    + _span(0x2500, 0x257F)          # box drawing
    + _span(0x25A0, 0x25FF)          # geometric shapes
    + [0x2605, 0x2606, 0x2610, 0x2611, 0x2612, 0x2713, 0x2714, 0x2717, 0x2718]
    + _span(0x3000, 0x303F)          # CJK symbols and punctuation
    + _span(0xFF01, 0xFF5E)          # full-width ASCII forms
    + _span(0xFFE0, 0xFFE6)          # full-width signs
)

# Small UI fonts only need ASCII plus the few non-ASCII symbols UI literals use.
_UI_SYMBOLS = [0x00B7, 0x2022, 0x2026, 0x2013, 0x2014, 0x2190, 0x2191, 0x2192, 0x2193,
               0x25A1, 0x25B2, 0x25BC, 0x25CF, 0x25CB, 0x2713, 0x00D7]

SETS = {
    "ascii": lambda: _span(ASCII_FIRST, ASCII_LAST),
    "digits": lambda: [0x20, 0x2D, 0x3A] + _span(0x30, 0x39),
    "ui-symbols": lambda: list(_UI_SYMBOLS),
    "gb2312": _gb2312,
    "extra-symbols": lambda: list(_EXTRA_SYMBOLS),
    "cjk-dynamic": lambda: _gb2312() + list(_EXTRA_SYMBOLS) + list(_UI_SYMBOLS),
}

# Code points that a set may request but the source font may legitimately lack
# (they are reported, not fatal). Everything in "ascii", "digits", and the
# hanzi of "gb2312" is mandatory.
def _mandatory(cps: set[int]) -> set[int]:
    hanzi = {cp for cp in cps if 0x4E00 <= cp <= 0x9FFF}
    ascii_ = {cp for cp in cps if ASCII_FIRST <= cp <= ASCII_LAST}
    return hanzi | ascii_


# ---------------------------------------------------------------------------
# Rasterization
# ---------------------------------------------------------------------------

@dataclass
class Glyph:
    cp: int
    adv_w: int          # 8.4 fixed point (pixels * 16)
    box_w: int
    box_h: int
    ofs_x: int
    ofs_y: int          # bottom of box relative to baseline, positive up
    pixels: list[int]   # 4-bit values, row-major, len == box_w * box_h


def quantize(value8: int) -> int:
    """Map 8-bit coverage to 4 bits; LVGL expands 0..15 back with x17."""
    return (value8 * 15 + 127) // 255


def rasterize(font_path: Path, size: int, cps: list[int], hinting: str):
    import freetype  # type: ignore[import-not-found]

    face = freetype.Face(str(font_path))
    face.set_pixel_sizes(0, size)
    flags = freetype.FT_LOAD_RENDER
    if hinting == "light":
        flags |= freetype.FT_LOAD_TARGET_LIGHT
    elif hinting == "none":
        flags |= freetype.FT_LOAD_NO_HINTING
    glyphs: list[Glyph] = []
    missing: list[int] = []
    for cp in cps:
        if face.get_char_index(cp) == 0:
            missing.append(cp)
            continue
        face.load_char(chr(cp), flags)
        slot = face.glyph
        bmp = slot.bitmap
        if bmp.pixel_mode != freetype.FT_PIXEL_MODE_GRAY and bmp.width * bmp.rows:
            raise SystemExit(f"U+{cp:04X}: unexpected pixel mode {bmp.pixel_mode}")
        width, rows, pitch = bmp.width, bmp.rows, bmp.pitch
        buf = bmp.buffer
        pixels = [quantize(buf[y * pitch + x]) for y in range(rows) for x in range(width)]
        # Trim fully transparent border rows/columns left by quantization.
        box = _trim(pixels, width, rows)
        adv = round(slot.advance.x / 64 * 16)
        if box is None:
            glyphs.append(Glyph(cp, adv, 0, 0, 0, 0, []))
            continue
        x0, y0, x1, y1 = box
        w, h = x1 - x0, y1 - y0
        trimmed = [pixels[(y0 + y) * width + x0 + x] for y in range(h) for x in range(w)]
        top = slot.bitmap_top - y0
        glyphs.append(Glyph(cp, adv, w, h, slot.bitmap_left + x0, top - h, trimmed))
    family = f"{face.family_name.decode(errors='replace')} {face.style_name.decode(errors='replace')}"
    return glyphs, missing, family


def _trim(pixels: list[int], width: int, rows: int):
    xs = [x for x in range(width) if any(pixels[y * width + x] for y in range(rows))]
    ys = [y for y in range(rows) if any(pixels[y * width + x] for x in range(width))]
    if not xs or not ys:
        return None
    return xs[0], ys[0], xs[-1] + 1, ys[-1] + 1


def pack(pixels: list[int]) -> bytes:
    out = bytearray()
    for i in range(0, len(pixels), 2):
        hi = pixels[i]
        lo = pixels[i + 1] if i + 1 < len(pixels) else 0
        out.append((hi << 4) | lo)
    return bytes(out)


# ---------------------------------------------------------------------------
# C emission
# ---------------------------------------------------------------------------

def emit_c(name: str, glyphs: list[Glyph], args, family: str, font_sha: str,
           missing: list[int]) -> str:
    glyphs = sorted(glyphs, key=lambda g: g.cp)
    ascii_glyphs = [g for g in glyphs if ASCII_FIRST <= g.cp <= ASCII_LAST]
    # A complete printable-ASCII run uses the dense FORMAT0_TINY cmap; a partial
    # one (e.g. a digits-only font) goes into the sparse cmap with everything else.
    has_ascii = [g.cp for g in ascii_glyphs] == list(range(ASCII_FIRST, ASCII_LAST + 1))
    if not has_ascii:
        ascii_glyphs = []
    other = [g for g in glyphs if g not in ascii_glyphs]
    ordered = ascii_glyphs + other

    bitmap = bytearray()
    dsc_lines = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */"]
    for g in ordered:
        if not (0 <= g.adv_w < 4096):
            raise SystemExit(f"U+{g.cp:04X}: advance {g.adv_w} does not fit 12 bits")
        if g.box_w > 255 or g.box_h > 255 or not (-128 <= g.ofs_x <= 127) or not (-128 <= g.ofs_y <= 127):
            raise SystemExit(f"U+{g.cp:04X}: box does not fit the glyph descriptor")
        index = len(bitmap)
        bitmap += pack(g.pixels)
        dsc_lines.append(
            f"    {{.bitmap_index = {index}, .adv_w = {g.adv_w}, .box_w = {g.box_w}, "
            f".box_h = {g.box_h}, .ofs_x = {g.ofs_x}, .ofs_y = {g.ofs_y}}} /* U+{g.cp:04X} */")
    if len(bitmap) > MAX_BITMAP_BYTES:
        raise SystemExit(f"bitmap is {len(bitmap)} bytes; LVGL's 20-bit index allows {MAX_BITMAP_BYTES}")

    cmaps = []
    unicode_list = ""
    if has_ascii:
        cmaps.append(
            f"    {{.range_start = {ASCII_FIRST}, .range_length = {ASCII_LAST - ASCII_FIRST + 1}, "
            f".glyph_id_start = 1, .unicode_list = NULL, .glyph_id_ofs_list = NULL, "
            f".list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}}")
    if other:
        start = other[0].cp
        span = other[-1].cp - start + 1
        if span > 0xFFFF or len(other) > 0xFFFF:
            raise SystemExit("sparse range exceeds uint16 limits")
        offsets = [g.cp - start for g in other]
        unicode_list = ("static const uint16_t unicode_list_sparse[] = {\n"
                        + _wrap([str(o) for o in offsets], 16) + "\n};\n\n")
        cmaps.append(
            f"    {{.range_start = {start}, .range_length = {span}, "
            f".glyph_id_start = {1 + len(ascii_glyphs)}, .unicode_list = unicode_list_sparse, "
            f".glyph_id_ofs_list = NULL, .list_length = {len(other)}, "
            f".type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}}")

    missing_note = ", ".join(f"U+{cp:04X}" for cp in missing[:40])
    if len(missing) > 40:
        missing_note += f", ... ({len(missing)} total)"
    header = f"""/*
 * GENERATED FILE - do not edit. Regenerate with tools/gen_lvgl_font.py.
 *
 * Font:       {family} (SHA-256 {font_sha})
 * License:    SIL Open Font License 1.1 (see assets/README.md)
 * Size:       {args.size} px, {args.bpp} bpp, PLAIN, hinting={args.hinting}
 * Line:       line_height={args.line_height}, base_line={args.base_line}
 * Sets:       {args.sets}
 * Glyphs:     {len(ordered)} ({len(bitmap)} bitmap bytes)
 * Requested but absent from the source font: {missing_note or "none"}
 * Generator:  gen_lvgl_font.py v{GENERATOR_VERSION}
 */
"""
    hex_lines = _wrap([f"0x{b:02x}" for b in bitmap], 24, sep=",")
    return (
        header
        + '\n#include "lvgl.h"\n\n'
        + f"static const uint8_t glyph_bitmap[] = {{\n{hex_lines}\n}};\n\n"
        + "static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n"
        + ",\n".join(dsc_lines) + "\n};\n\n"
        + unicode_list
        + "static const lv_font_fmt_txt_cmap_t cmaps[] = {\n" + ",\n".join(cmaps) + "\n};\n\n"
        + "static const lv_font_fmt_txt_dsc_t font_dsc = {\n"
        + "    .glyph_bitmap = glyph_bitmap,\n"
        + "    .glyph_dsc = glyph_dsc,\n"
        + "    .cmaps = cmaps,\n"
        + "    .kern_dsc = NULL,\n"
        + "    .kern_scale = 0,\n"
        + f"    .cmap_num = {len(cmaps)},\n"
        + f"    .bpp = {args.bpp},\n"
        + "    .kern_classes = 0,\n"
        + "    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,\n"
        + "    .stride = 0,\n"
        + "};\n\n"
        + f"const lv_font_t {name} = {{\n"
        + "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n"
        + "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n"
        + f"    .line_height = {args.line_height},\n"
        + f"    .base_line = {args.base_line},\n"
        + "    .subpx = LV_FONT_SUBPX_NONE,\n"
        + "    .underline_position = -2,\n"
        + "    .underline_thickness = 1,\n"
        + "    .dsc = &font_dsc,\n"
        + "    .fallback = NULL,\n"
        + "    .user_data = NULL,\n"
        + "};\n"
    )


def _wrap(items: list[str], per_line: int, sep: str = ", ") -> str:
    lines = []
    for i in range(0, len(items), per_line):
        lines.append("    " + sep.join(items[i:i + per_line]) + ("," if sep == "," else ""))
    if sep != ",":
        return ",\n".join(lines)
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Readback verification (also used by tests/test_cc_fonts.py)
# ---------------------------------------------------------------------------

@dataclass
class ParsedFont:
    bitmap: bytes
    dsc: list[tuple[int, int, int, int, int, int]]   # index, adv, w, h, ox, oy
    cmaps: list[dict]
    unicode_list: list[int]
    line_height: int
    base_line: int
    bpp: int

    def lookup(self, cp: int) -> int:
        """Return the glyph id LVGL would resolve for cp, or 0."""
        for cmap in self.cmaps:
            rcp = cp - cmap["range_start"]
            if rcp < 0 or rcp >= cmap["range_length"]:
                continue
            if cmap["type"] == "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY":
                return cmap["glyph_id_start"] + rcp
            if cmap["type"] == "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY":
                lo, hi = 0, cmap["list_length"] - 1
                while lo <= hi:
                    mid = (lo + hi) // 2
                    val = self.unicode_list[mid]
                    if val == rcp:
                        return cmap["glyph_id_start"] + mid
                    if val < rcp:
                        lo = mid + 1
                    else:
                        hi = mid - 1
                return 0
            raise ValueError(f"unsupported cmap type {cmap['type']}")
        return 0

    def decode(self, gid: int) -> list[int]:
        """Decode a glyph exactly like lv_font_get_bitmap_fmt_txt (PLAIN, 4 bpp, stride 0)."""
        index, _adv, w, h, _ox, _oy = self.dsc[gid]
        out = []
        pos = index
        i = 0
        for _y in range(h):
            for _x in range(w):
                i &= 1
                if i == 0:
                    out.append(self.bitmap[pos] >> 4)
                else:
                    out.append(self.bitmap[pos] & 0x0F)
                    pos += 1
                i += 1
        return out

    def codepoints(self) -> set[int]:
        cps: set[int] = set()
        for cmap in self.cmaps:
            if cmap["type"] == "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY":
                cps.update(range(cmap["range_start"], cmap["range_start"] + cmap["range_length"]))
            else:
                cps.update(cmap["range_start"] + o for o in self.unicode_list[:cmap["list_length"]])
        return cps


def parse_c(text: str) -> ParsedFont:
    def array(name: str) -> str:
        match = re.search(rf"{name}\[\]\s*=\s*\{{(.*?)\n\}};", text, re.S)
        if not match:
            raise ValueError(f"array {name} not found")
        return match.group(1)

    bitmap = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", array("glyph_bitmap")))
    dsc = []
    for m in re.finditer(r"\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
                         r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)", array("glyph_dsc")):
        dsc.append(tuple(int(v) for v in m.groups()))
    unicode_list = []
    if "unicode_list_sparse[]" in text:
        unicode_list = [int(v) for v in re.findall(r"\d+", array("unicode_list_sparse"))]
    cmaps = []
    for m in re.finditer(r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+), "
                         r"\.unicode_list = \w+, \.glyph_id_ofs_list = \w+, \.list_length = (\d+), "
                         r"\.type = (\w+)", array("cmaps")):
        cmaps.append({"range_start": int(m.group(1)), "range_length": int(m.group(2)),
                      "glyph_id_start": int(m.group(3)), "list_length": int(m.group(4)),
                      "type": m.group(5)})
    line_height = int(re.search(r"\.line_height = (\d+)", text).group(1))
    base_line = int(re.search(r"\.base_line = (\d+)", text).group(1))
    bpp = int(re.search(r"\.bpp = (\d+)", text).group(1))
    return ParsedFont(bitmap, dsc, cmaps, unicode_list, line_height, base_line, bpp)


def verify(text: str, glyphs: list[Glyph]) -> None:
    parsed = parse_c(text)
    if len(parsed.dsc) != len(glyphs) + 1:
        raise SystemExit(f"readback: {len(parsed.dsc)} descriptors for {len(glyphs)} glyphs")
    for g in glyphs:
        gid = parsed.lookup(g.cp)
        if gid == 0:
            raise SystemExit(f"readback: U+{g.cp:04X} does not resolve through the cmaps")
        _index, adv, w, h, ox, oy = parsed.dsc[gid]
        if (adv, w, h, ox, oy) != (g.adv_w, g.box_w, g.box_h, g.ofs_x, g.ofs_y):
            raise SystemExit(f"readback: U+{g.cp:04X} metrics differ")
        if parsed.decode(gid) != g.pixels:
            raise SystemExit(f"readback: U+{g.cp:04X} pixels differ")
    absent = 0x10FFFF
    if parsed.lookup(absent) != 0:
        raise SystemExit("readback: an absent code point resolved to a glyph")


# ---------------------------------------------------------------------------
# Preview (simulates LVGL compositing white text on a dark background)
# ---------------------------------------------------------------------------

def preview(text_c: str, sample: str, path: Path, scale: int = 3) -> None:
    parsed = parse_c(text_c)
    lines = sample.split("\n")
    width = 240
    height = parsed.line_height * len(lines) + 8
    canvas = [[0] * width for _ in range(height)]
    for row, line in enumerate(lines):
        x = 4
        top = 4 + row * parsed.line_height
        for ch in line:
            gid = parsed.lookup(ord(ch))
            if gid == 0:
                x += 8
                continue
            _i, adv, w, h, ox, oy = parsed.dsc[gid]
            pix = parsed.decode(gid)
            y1 = top + (parsed.line_height - parsed.base_line) - h - oy
            for yy in range(h):
                for xx in range(w):
                    px, py = x + ox + xx, y1 + yy
                    if 0 <= px < width and 0 <= py < height:
                        canvas[py][px] = max(canvas[py][px], pix[yy * w + xx] * 17)
            x += (adv + 8) // 16
    raw = bytearray()
    for y in range(height * scale):
        raw.append(0)
        for x in range(width * scale):
            v = canvas[y // scale][x // scale]
            raw += bytes((20 + v * 220 // 255, 20 + v * 218 // 255, 19 + v * 211 // 255))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width * scale, height * scale, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    path.write_bytes(png)


# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--font", type=Path, required=True)
    parser.add_argument("--size", type=int, required=True)
    parser.add_argument("--bpp", type=int, default=4, choices=[4])
    parser.add_argument("--sets", required=True, help="comma-separated: " + ", ".join(SETS))
    parser.add_argument("--symbols", default="", help="extra literal characters to include")
    parser.add_argument("--name", required=True)
    parser.add_argument("--line-height", type=int, required=True)
    parser.add_argument("--base-line", type=int, required=True)
    parser.add_argument("--hinting", choices=["normal", "light", "none"], default="normal")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--preview", type=Path, help="write a PNG preview of --sample")
    parser.add_argument("--sample", default="Claude Code \\u5de5\\u4f5c\\u4e2d")
    args = parser.parse_args(argv)

    cps: set[int] = set()
    for name in args.sets.split(","):
        if name not in SETS:
            parser.error(f"unknown set {name}")
        cps.update(SETS[name]())
    cps.update(ord(ch) for ch in args.symbols)
    cps = {cp for cp in cps if cp >= 0x20 and not (0x7F <= cp < 0xA0)}

    font_sha = hashlib.sha256(args.font.read_bytes()).hexdigest()
    glyphs, missing, family = rasterize(args.font, args.size, sorted(cps), args.hinting)
    mandatory_missing = sorted(set(missing) & _mandatory(cps))
    if mandatory_missing:
        raise SystemExit("mandatory code points absent from the font: "
                         + ", ".join(f"U+{cp:04X}" for cp in mandatory_missing[:20]))
    text = emit_c(args.name, glyphs, args, family, font_sha, missing)
    verify(text, glyphs)
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    if args.preview:
        sample = args.sample.encode("ascii", "backslashreplace").decode("unicode_escape")
        preview(text, sample, args.preview)
    bitmap_bytes = len(parse_c(text).bitmap)
    print(f"{args.name}: {len(glyphs)} glyphs, {bitmap_bytes} bitmap bytes, "
          f"{len(missing)} requested code points absent from {family}; readback verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
