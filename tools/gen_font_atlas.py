#!/usr/bin/env python3
"""
DeadSpeak Font Atlas Generator
Bakes smooth anti-aliased sans-serif vector fonts into a compact RLE-compressed
OpenGL ES 2.0 texture atlas and C23 metrics table.
"""

import os
import sys
from PIL import Image, ImageFont, ImageDraw

def find_font(paths):
    for p in paths:
        if os.path.exists(p):
            return p
    return None

reg_font_path = find_font([
    "/tmp/Roboto-Regular.ttf",
    "/usr/share/fonts/TTF/Roboto-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/gnu-free/FreeSans.otf",
])

bold_font_path = find_font([
    "/usr/share/fonts/TTF/Roboto-Bold.ttf",
    "/tmp/Roboto-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/gnu-free/FreeSansBold.otf",
])

if not reg_font_path or not bold_font_path:
    print("Error: Could not locate required TrueType fonts", file=sys.stderr)
    sys.exit(1)

font_reg = ImageFont.truetype(reg_font_path, 28)
font_bold = ImageFont.truetype(bold_font_path, 28)

atlas_w = 512
atlas_h = 256
img = Image.new('L', (atlas_w, atlas_h), 0)
draw = ImageDraw.Draw(img)

# Measure baseline reference using 'H'
bbox_H = font_reg.getbbox('H')
cap_top = bbox_H[1]
cap_height = float(bbox_H[3] - bbox_H[1])

styles = [('REGULAR', font_reg), ('BOLD', font_bold)]

cur_x = 1
cur_y = 1
max_row_h = 0

glyph_metrics = []

for sname, font in styles:
    metrics = []
    for code in range(32, 127):
        ch = chr(code)
        bbox = font.getbbox(ch)
        adv = float(font.getlength(ch))
        if code == 32 or not bbox:
            metrics.append((code, 0, 0, 0, 0, 0, 0, adv))
            continue
        gw = bbox[2] - bbox[0]
        gh = bbox[3] - bbox[1]

        if cur_x + gw + 2 >= atlas_w:
            cur_x = 1
            cur_y += max_row_h + 2
            max_row_h = 0

        gx = cur_x + 1
        gy = cur_y + 1

        draw.text((gx - bbox[0], gy - bbox[1]), ch, font=font, fill=255)

        ox = bbox[0]
        oy = bbox[1] - cap_top

        metrics.append((code, gx, gy, gw, gh, ox, oy, adv))

        cur_x += gw + 3
        if gh > max_row_h:
            max_row_h = gh

    glyph_metrics.append((sname, metrics))

total_y = cur_y + max_row_h
if total_y >= atlas_h:
    print(f"Error: Atlas overflow {total_y} >= {atlas_h}", file=sys.stderr)
    sys.exit(1)

raw = img.tobytes()
rle = bytearray()
i = 0
n = len(raw)
while i < n:
    b = raw[i]
    run = 1
    while i + run < n and raw[i + run] == b and run < 255:
        run += 1
    rle.extend([run, b])
    i += run

header_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../app/src/main/cpp/font_atlas.h"))
source_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../app/src/main/cpp/font_atlas.c"))

with open(header_path, "w", encoding="utf-8") as out:
    out.write("/* DeadSpeak Native Anti-Aliased Font Atlas (Auto-generated) */\n")
    out.write("#pragma once\n\n")
    out.write("#include <stddef.h>\n")
    out.write("#include <stdint.h>\n\n")
    out.write("enum {\n")
    out.write(f"    DS_FONT_ATLAS_WIDTH = {atlas_w},\n")
    out.write(f"    DS_FONT_ATLAS_HEIGHT = {atlas_h},\n")
    out.write(f"    DS_FONT_ATLAS_RAW_SIZE = {len(raw)},\n")
    out.write(f"    DS_FONT_ATLAS_RLE_SIZE = {len(rle)},\n")
    out.write("    DS_FONT_FIRST_CHAR = 32,\n")
    out.write("    DS_FONT_NUM_CHARS = 95,\n")
    out.write("};\n\n")
    out.write(f"#define DS_FONT_CAP_HEIGHT {cap_height:.1f}f\n\n")
    out.write("typedef struct {\n")
    out.write("    uint16_t x;\n")
    out.write("    uint16_t y;\n")
    out.write("    uint8_t  w;\n")
    out.write("    uint8_t  h;\n")
    out.write("    int8_t   ox;\n")
    out.write("    int8_t   oy;\n")
    out.write("    float    adv;\n")
    out.write("} DSFontGlyph;\n\n")
    out.write("extern const DSFontGlyph ds_font_glyphs[2][DS_FONT_NUM_CHARS];\n\n")
    out.write("void ds_font_unpack_atlas(uint8_t *dest, size_t dest_size);\n")

with open(source_path, "w", encoding="utf-8") as out:
    out.write("/* DeadSpeak Native Anti-Aliased Font Atlas Data (Auto-generated) */\n")
    out.write("#include \"font_atlas.h\"\n\n")
    out.write("#include <string.h>\n\n")

    out.write("const DSFontGlyph ds_font_glyphs[2][DS_FONT_NUM_CHARS] = {\n")
    for sidx, (sname, metrics) in enumerate(glyph_metrics):
        out.write(f"    /* Style {sidx}: {sname} */\n    {{\n")
        for code, gx, gy, gw, gh, ox, oy, adv in metrics:
            char_repr = repr(chr(code)) if code != 92 and code != 34 and code != 39 else str(code)
            out.write(f"        {{{gx}, {gy}, {gw}, {gh}, {ox}, {oy}, {adv:.3f}f}}, /* {code} {char_repr} */\n")
        out.write("    },\n")
    out.write("};\n\n")

    out.write(f"static const uint8_t ds_font_atlas_rle[{len(rle)}] = {{\n")
    for row_start in range(0, len(rle), 16):
        chunk = rle[row_start:row_start + 16]
        hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
        out.write(f"    {hex_str},\n")
    out.write("};\n\n")

    out.write("void ds_font_unpack_atlas(uint8_t *dest, size_t dest_size) {\n")
    out.write("    if (dest == nullptr || dest_size < DS_FONT_ATLAS_RAW_SIZE) return;\n")
    out.write("    size_t out_idx = 0;\n")
    out.write("    for (size_t in_idx = 0; in_idx + 1 < DS_FONT_ATLAS_RLE_SIZE && out_idx < dest_size; in_idx += 2) {\n")
    out.write("        uint8_t count = ds_font_atlas_rle[in_idx];\n")
    out.write("        uint8_t val = ds_font_atlas_rle[in_idx + 1];\n")
    out.write("        size_t run = (out_idx + count <= dest_size) ? count : (dest_size - out_idx);\n")
    out.write("        memset(dest + out_idx, val, run);\n")
    out.write("        out_idx += run;\n")
    out.write("    }\n")
    out.write("}\n")

print(f"Generated {header_path} and {source_path}: {len(rle)} RLE bytes, {len(glyph_metrics[0][1]) * 2} glyph metrics.")
