"""
生成中文点阵字库（供 ESP32 TFT 使用）。

用 PIL 渲染系统中文字体成 1bpp 位图，打包成紧凑二进制文件，
以 EMBED_FILES 放进固件 flash，运行时按需读取（不占用 RAM）。

ASCII 槽位直接复用 font9x13.h 的原始字节（9x13，底部对齐到 16x16 单元格），
保证中英混排时西文与中文共享基线，且西文字形与 ASCII 字体完全一致。

输出:
  components/ili9341/include/font_cjk16.bin      字形数据
  components/ili9341/include/font_cjk16_cp.h     码位表

字库格式（小端）:
    header (16 bytes)
      uint16 magic       = 0x4331 ("C1")
      uint16 version     = 1
      uint16 glyph_w     = 16
      uint16 glyph_h     = 16
      uint16 bytes_per_glyph = 2  (16 px -> 2 bytes/row)
      uint16 first_cp
      uint16 count
      uint16 reserved
    glyph table: count * (glyph_h * bytes_per_glyph) bytes
      中文: 整个 16x16 单元格
      ASCII: 9x13 字形放在单元格底部 (行 3..15), 推进宽度 9 px

由 tools/gen_cjk_font.py 自动生成，请勿手工编辑。
"""
import os
import re
import struct
from PIL import Image, ImageDraw, ImageFont

OUT_DIR = "/Users/freyrandy/Projects/ESP32/firmware/components/ili9341/include"
BIN_OUT = os.path.join(OUT_DIR, "font_cjk16.bin")
CP_OUT = os.path.join(OUT_DIR, "font_cjk16_cp.h")
FONT9X13 = os.path.join(OUT_DIR, "font9x13.h")

GLYPH_W, GLYPH_H = 16, 16
BPR = 2
ASCII_W, ASCII_H = 9, 13
Y_OFF = GLYPH_H - ASCII_H

CJK_FONTS = [
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
]

# Which characters to include:
#   "l1"   - GB2312 level 1 hanzi only (3755 chars, 124 KB)
#   "l1l2" - level 1 + level 2 hanzi (6763 chars, ~215 KB) - needs a
#             partition of at least 2 MB
CHAR_SET = "l1l2"

EXTRA = (
    "　、。〃々〈〉《》「」『』【】〔〕〖〗〝〞・ー"
    "！＂＃＄％＆＇（）＊＋，－．／：；＜＝＞？＠"
    "［＼］＾＿｀｛｜｝～￥"
    "０１２３４５６７８９"
    "ＡＢＣＤＥＦＧＨＩＪＫＬＭＮＯＰＱＲＳＴＵＶＷＸＹＺ"
    "ａｂｃｄｅｆｇｈｉｊｋｌｍｎｏｐｑｒｓｔｕｖｗｘｙｚ"
)


def pick_cjk_font(size):
    for p in CJK_FONTS:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size), p
            except Exception:
                continue
    raise SystemExit("no CJK font found")


def gb2312_hanzi(level="all"):
    """GB2312 汉字。

    level="l1"  一级汉字 3755 个（区 16-55），按拼音排序，最常用
    level="l1l2" 一级 + 二级 6763 个（区 16-87），覆盖全部常用字
    level="all" 全部汉字 6763 个 + 84 个图形符号（区 16-87）
    """
    out = []
    zones = range(16, 56) if level == "l1" else range(16, 88)
    for zone in zones:
        for pos in range(1, 95):
            try:
                out.append(bytes([zone + 0xA0, pos + 0xA0]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return out


def pack(img):
    px = img.load()
    out = bytearray()
    for y in range(GLYPH_H):
        row = bytearray(BPR)
        for x in range(GLYPH_W):
            if px[x, y]:
                row[x // 8] |= 1 << (7 - (x % 8))
        out.extend(row)
    return bytes(out)


def render_cjk(ch, font):
    img = Image.new("1", (GLYPH_W, GLYPH_H), 0)
    ImageDraw.Draw(img).text((GLYPH_W // 2, GLYPH_H // 2), ch, fill=1,
                              font=font, anchor="mm")
    return pack(img)


def load_font9x13():
    """读出 font9x13.h 的原始字形字节。"""
    h = open(FONT9X13).read()
    i = h.index("= {")
    j = h.index("\n};", i)
    glyphs = re.findall(r"\{([^{}]*)\}", h[i:j])
    rows = [[int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", g)]
            for g in glyphs]
    if len(rows) != 95 or len(rows[0]) != ASCII_H * 2:
        raise SystemExit("unexpected font9x13 layout")
    return rows


def main():
    cjk_font, path = pick_cjk_font(13)
    print("CJK font:", path)

    chars = []
    seen = set()

    def add(ch):
        cp = ord(ch)
        if cp not in seen:
            seen.add(cp)
            chars.append(ch)

    for ch in gb2312_hanzi(CHAR_SET):
        add(ch)
    for ch in EXTRA:
        add(ch)
    for c in range(0x20, 0x7F):
        add(chr(c))
    chars.sort(key=ord)
    print("glyphs:", len(chars))

    blobs = []
    for ch in chars:
        if ord(ch) < 0x80:
            blobs.append(None)          # 稍后用 font9x13 填充
        else:
            blobs.append(render_cjk(ch, cjk_font))

    # ASCII: 复用 font9x13 字形, 底部对齐
    f9 = load_font9x13()
    for idx, ch in enumerate(chars):
        if ord(ch) >= 0x80:
            continue
        src = f9[ord(ch) - 0x20]
        cell = bytearray(GLYPH_H * BPR)
        for y in range(ASCII_H):
            cell[(Y_OFF + y) * BPR + 0] = src[y * 2 + 0]
            cell[(Y_OFF + y) * BPR + 1] = src[y * 2 + 1]
        blobs[idx] = bytes(cell)

    hdr = struct.pack("<HHHHHHHH", 0x4331, 1, GLYPH_W, GLYPH_H, BPR,
                       ord(chars[0]), len(chars), 0)
    with open(BIN_OUT, "wb") as f:
        f.write(hdr)
        for b in blobs:
            f.write(b)
    print("bin:", os.path.getsize(BIN_OUT), "bytes")

    lines = [
        "/*",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        " * font_cjk16_cp.h: Unicode code points present in font_cjk16.bin",
        " * 由 tools/gen_cjk_font.py 自动生成，请勿手工编辑。",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "#define FONT_CJK16_COUNT     %d" % len(chars),
        "#define FONT_CJK16_GLYPH_W   %d" % GLYPH_W,
        "#define FONT_CJK16_GLYPH_H   %d" % GLYPH_H,
        "#define FONT_CJK16_BPR       %d" % BPR,
        "#define FONT_CJK16_ASCII_W   %d" % ASCII_W,
        "#define FONT_CJK16_ASCII_H   %d" % ASCII_H,
        "",
        "static const uint16_t font_cjk16_cps[FONT_CJK16_COUNT] = {",
    ]
    cpv = [ord(c) for c in chars]
    for i in range(0, len(cpv), 12):
        lines.append("    " + ", ".join("0x%04X" % c for c in cpv[i:i + 12]) + ",")
    lines += ["};", ""]
    with open(CP_OUT, "w") as f:
        f.write("\n".join(lines))
    print("cp header written")


main()
