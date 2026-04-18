#!/usr/bin/env python3
"""Generate Adafruit GFX font header with extended Latin characters.

Replicates Adafruit's fontconvert behavior: 141 DPI, same GFXfont format.
Supports Unicode range for Hungarian and other European languages.
"""

import freetype
import sys
import os

DPI = 141  # matches Adafruit fontconvert


def generate_font(ttf_path, pt_size, first, last, font_name):
    face = freetype.Face(ttf_path)
    face.set_char_size(0, pt_size * 64, DPI, DPI)

    bitmaps = bytearray()
    glyphs = []

    for code in range(first, last + 1):
        gi = face.get_char_index(code)
        if gi == 0 and code != 0x20:
            # Glyph not in font — emit empty placeholder
            glyphs.append((len(bitmaps), 0, 0, 0, 0, 0))
            continue

        face.load_char(chr(code), freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
        g = face.glyph
        bmp = g.bitmap

        w = bmp.width
        h = bmp.rows
        advance = (g.advance.x + 32) >> 6  # 26.6 fixed to int, rounded
        x_off = g.bitmap_left
        y_off = -g.bitmap_top  # GFX uses top-relative negative offset

        offset = len(bitmaps)

        if w > 0 and h > 0:
            # Pack bitmap: FreeType mono bitmaps are byte-aligned per row
            # GFX wants tightly packed bits (no row padding)
            bit_count = 0
            current_byte = 0
            for row in range(h):
                row_start = row * bmp.pitch
                for col in range(w):
                    byte_idx = row_start + (col >> 3)
                    bit_idx = 7 - (col & 7)
                    pixel = (bmp.buffer[byte_idx] >> bit_idx) & 1
                    current_byte = (current_byte << 1) | pixel
                    bit_count += 1
                    if bit_count == 8:
                        bitmaps.append(current_byte)
                        current_byte = 0
                        bit_count = 0
            if bit_count > 0:
                bitmaps.append(current_byte << (8 - bit_count))

        glyphs.append((offset, w, h, advance, x_off, y_off))

    # Line height
    y_advance = (face.size.height + 32) >> 6

    # Generate C header
    lines = []
    lines.append(f"// Generated from {os.path.basename(ttf_path)} at {pt_size}pt, {DPI} DPI")
    lines.append(f"// Range: U+{first:04X} to U+{last:04X} ({last - first + 1} glyphs)")
    lines.append(f"// Bitmap size: {len(bitmaps)} bytes")
    lines.append("")

    # Bitmap array
    lines.append(f"const uint8_t {font_name}Bitmaps[] PROGMEM = {{")
    for i in range(0, len(bitmaps), 12):
        chunk = bitmaps[i:i+12]
        hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"  {hex_str},")
    lines.append("};")
    lines.append("")

    # Glyph array
    lines.append(f"const GFXglyph {font_name}Glyphs[] PROGMEM = {{")
    for i, (off, w, h, adv, xo, yo) in enumerate(glyphs):
        code = first + i
        ch = chr(code) if 0x20 <= code < 0x7F else ""
        comment = f"  // U+{code:04X}"
        if ch:
            comment += f" '{ch}'"
        lines.append(f"  {{ {off:5d}, {w:3d}, {h:3d}, {adv:3d}, {xo:4d}, {yo:4d} }},{comment}")
    lines.append("};")
    lines.append("")

    # Font struct
    lines.append(f"const GFXfont {font_name} PROGMEM = {{")
    lines.append(f"  (uint8_t  *){font_name}Bitmaps,")
    lines.append(f"  (GFXglyph *){font_name}Glyphs,")
    lines.append(f"  0x{first:04X}, 0x{last:04X}, {y_advance}")
    lines.append("};")
    lines.append("")

    return "\n".join(lines)


if __name__ == "__main__":
    ttf = sys.argv[1] if len(sys.argv) > 1 else r"C:\Windows\Fonts\arialbd.ttf"
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 9
    first = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x20
    last = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x17F
    name = sys.argv[5] if len(sys.argv) > 5 else "FreeSansBold9pt8b"

    header = generate_font(ttf, size, first, last, name)
    print(header)
