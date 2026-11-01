"""生成 ASCII 点阵字体 C 头文件。

用 PIL 渲染系统字体成 1bpp 位图，再打包成 C 数组。
每种字号输出一个独立头文件：

  font6x8.h    -> font6x8,    6x8  (1 byte/row)
  font11x16.h  -> font11x16, 11x16 (2 bytes/row)

由 tools/gen_font.py 自动生成，请勿手工编辑。
"""
import os
from PIL import Image, ImageDraw, ImageFont

OUT_DIR = "/Users/freyrandy/Projects/ESP32/firmware/components/ili9341/include"

FONT_CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Courier New Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
]


def pick_font(size):
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                continue
    return ImageFont.load_default()


def render_char(ch, font, cell_w, cell_h, bytes_per_row):
    img = Image.new("1", (cell_w, cell_h), 0)
    d = ImageDraw.Draw(img)
    try:
        d.text((cell_w // 2, cell_h // 2), ch, fill=1, font=font, anchor="mm")
    except Exception:
        d.text((1, 0), ch, fill=1, font=font)
    px = img.load()
    rows = []
    for y in range(cell_h):
        row = bytearray(bytes_per_row)
        for x in range(cell_w):
            if px[x, y]:
                row[x // 8] |= (1 << (7 - (x % 8)))
        rows.append(list(row))
    return rows


def build(name, cell_w, cell_h, font_size, out_file, first=32, last=126):
    font = pick_font(font_size)
    bytes_per_row = (cell_w + 7) // 8
    glyphs = []
    for code in range(first, last + 1):
        ch = chr(code)
        glyphs.append((code, ch, render_char(ch, font, cell_w, cell_h, bytes_per_row)))

    lines = [
        "/*",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        f" * {name}: ASCII {first}-{last}, {cell_w}x{cell_h} 1bpp 点阵字体",
        " * 由 tools/gen_font.py 自动生成，请勿手工编辑。",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define {name}_W          {cell_w}",
        f"#define {name}_H          {cell_h}",
        f"#define {name}_BPR        {bytes_per_row}",
        f"#define {name}_FIRST      {first}",
        f"#define {name}_LAST       {last}",
        f"#define {name}_COUNT      {last - first + 1}",
        "",
        f"static const uint8_t {name}[{name}_COUNT][{name}_H * {name}_BPR] = {{",
    ]
    for code, ch, rows in glyphs:
        disp = ch if ch not in ('"', "\\") else "\\" + ch
        lines.append(f"    /* '{disp}' ({code}) */ {{")
        for i in range(0, len(rows), 4):
            flat = [b for row in rows[i:i + 4] for b in row]
            lines.append("        " + ", ".join(f"0x{b:02X}" for b in flat) + ",")
        lines.append("    },")
    lines.append("};")
    lines.append("")

    path = os.path.join(OUT_DIR, out_file)
    with open(path, "w") as f:
        f.write("\n".join(lines))
    print(f"wrote {path}: {len(glyphs)} glyphs, {cell_w}x{cell_h}, bpr={bytes_per_row}")


os.makedirs(OUT_DIR, exist_ok=True)
build("font6x8", 6, 8, 8, "font6x8.h")
build("font11x16", 11, 16, 16, "font11x16.h")
