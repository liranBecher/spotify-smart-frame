"""Generate the small bitmap font used by the ESP32 display.

Sources: Noto Sans Hebrew and Noto Sans, SIL Open Font License 1.1
(see OFL.txt and OFL-NotoSans.txt).
Run: python make_font.py
"""

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).parent
CHARS = sorted(set(list(range(32, 127)) + list(range(160, 384)) +
                   list(range(0x400, 0x460)) + list(range(0x590, 0x600)) +
                   [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D,
                    0x2026, 0x2032, 0x2033, 0x2116]))
OUT = [
    "// Generated from Noto Sans Hebrew and Noto Sans (OFL 1.1).\n",
    "// See OFL.txt and OFL-NotoSans.txt.\n",
    "#pragma once\n#include <Arduino.h>\n",
    "struct BitmapGlyph { uint16_t code; uint32_t offset; uint8_t width; uint8_t advance; };\n",
]

for size, height, baseline in ((18, 23, 16), (24, 30, 21), (30, 37, 27)):
    hebrew_font = ImageFont.truetype(str(ROOT / "NotoSansHebrew.ttf"), size)
    cyrillic_font = ImageFont.truetype(str(ROOT / "NotoSans.ttf"), size)
    hebrew_font.set_variation_by_axes([650, 100])
    cyrillic_font.set_variation_by_axes([650, 100])
    blob = []
    glyphs = []
    for code in CHARS:
        font = cyrillic_font if 0x400 <= code < 0x460 or code == 0x2116 else hebrew_font
        char = chr(code)
        advance = max(1, round(font.getlength(char)))
        width = min(32, max(advance + 2, font.getbbox(char, anchor="ls")[2] + 1))
        image = Image.new("1", (width, height))
        ImageDraw.Draw(image).text((0, baseline), char, font=font, fill=1, anchor="ls", stroke_width=0)
        glyphs.append((code, len(blob), width, advance))
        for y in range(height):
            for byte_x in range((width + 7) // 8):
                value = 0
                for bit in range(8):
                    x = byte_x * 8 + bit
                    if x < width and image.getpixel((x, y)):
                        value |= 0x80 >> bit
                blob.append(value)
    OUT.append(f"constexpr uint8_t font{size}Height = {height};\n")
    OUT.append(f"const BitmapGlyph font{size}Glyphs[] PROGMEM = {{\n")
    OUT.extend(f"  {{{code}, {offset}, {width}, {advance}}},\n" for code, offset, width, advance in glyphs)
    OUT.append("};\n")
    OUT.append(f"const uint8_t font{size}Bits[] PROGMEM = {{\n")
    for i in range(0, len(blob), 24):
        OUT.append("  " + ", ".join(str(n) for n in blob[i : i + 24]) + ",\n")
    OUT.append("};\n")

(ROOT / "bitmap_font.h").write_text("".join(OUT), encoding="utf-8")
