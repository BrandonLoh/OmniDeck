#!/usr/bin/env python3
"""
gen_bold_fonts.py — 为 OmniDeck 生成思源黑体 Bold 字重的 LVGL cbin 字体
=====================================================================
输出格式与 78__xiaozhi-fonts 的 cbin 运行时加载器 (cbin_font_create) 完全对应:
  [lv_font_t 36B] [fmt_txt_dsc_t 24B] [cmap 20B + unicode_list]
  [glyph_dsc 16B × N] [bitmaps]
dsc 内的 glyph_bitmap/glyph_dsc/cmaps/kern 偏移均相对 dsc 自身位置
(加载器: bin_addr += font->dsc 后再 add_base), unicode_list 相对 cmaps 起始。
本项目 sdkconfig 打开 CONFIG_LV_FONT_FMT_TXT_LARGE=y:
  glyph_dsc(16B): u32 bitmap_index + u32 adv_w(28.4) + u16 box_w + u16 box_h
                   + i16 ofs_x + i16 ofs_y

渲染要点 (对齐官方 common 字体约定, 适配 1-bit RLCD):
  - 2x 超采样渲染后 LANCZOS 降采样: 抗锯齿斜坡更宽, 阈值化后笔画更粗更稳
  - 画布高度 = 字体 asc+desc (不裁掉底部笔画/下降部)
  - 行高/基线采用官方同尺寸字体的 profile 值 (14:16/2, 16:25/9, 20:31/11)
  - ofs_y = max(0, yTop - C): 墨迹底边落在基线附近, C 由官方字形实测标定
"""

import json
import os
import struct

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from PIL import Image, ImageDraw, ImageFont

OUT_DIR = r'D:/Brandon/Documents/GitHub/OmniDeck/firmware/xiaozhi-esp32-backup/main/boards/omnideck/fonts'
VAR_TTF = r'C:\Users\alienware\AppData\Local\Temp\NotoSansSC.ttf'
CHARSET = r'D:/Brandon/Documents/GitHub/OmniDeck/firmware/xiaozhi-esp32-backup/managed_components/78__xiaozhi-fonts/charsets/common.json'

BOLD_TTF = os.path.join(OUT_DIR, '_NotoSansSC-Bold.ttf')

# 每档字体的行高/基线/位置标定:
#   C = yTop(超采样) 与官方字体 ofs_y 的实测差 (20px: '一二字早' 均为 10; 16px: 7; 14px: 6)
#   42px 数字无官方参照: C=-13 使墨迹底边贴齐基线 (lh-base), 居中于倒计时区域
FONT_PROFILES = [
    # (输出名, 字号, bpp, line_height, base_line, C)
    ('font_omni_14_bold.bin', 14, 4, 16, 2, 6),
    ('font_omni_16_bold.bin', 16, 4, 25, 9, 7),
    ('font_omni_20_bold.bin', 20, 4, 31, 11, 10),
    ('font_omni_42_digits.bin', 42, 4, 64, 12, -13),
]


def prepare_bold_ttf():
    """从可变字重字体实例化 Bold(700) 静态字体"""
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(BOLD_TTF):
        font = TTFont(VAR_TTF)
        # 注意: instantiateVariableFont 返回新对象, 必须接住返回值再 save
        inst = instantiateVariableFont(font, {'wght': 700})
        assert inst['OS/2'].usWeightClass == 700 and 'fvar' not in inst
        inst.save(BOLD_TTF)
        print('已生成 Bold 静态字体:', BOLD_TTF)
    return BOLD_TTF


def load_charset():
    d = json.load(open(CHARSET, encoding='utf-8'))
    cps = sorted(d['codepoints'])
    # 过滤代理区等非法码点
    cps = [c for c in cps if 0x20 <= c <= 0x10FFFF and not (0xD800 <= c <= 0xDFFF)]
    return cps


def render_glyph(font2, ch, bpp, C):
    """2x 超采样渲染单个字形, 返回 (adv, box_w, box_h, ofs_x, ofs_y, packed_bytes)"""
    ss = 2
    adv = int(font2.getlength(ch) / ss + 0.5)
    asc, desc = font2.getmetrics()          # 2x 下的 ascender/descender
    canvas_w = (adv + 8) * ss
    canvas_h = asc + desc                    # 完整墨迹高度, 不裁底
    img = Image.new('L', (canvas_w, canvas_h), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), ch, font=font2, fill=255)
    bbox = img.getbbox()
    if bbox is None:
        return adv, 0, 0, 0, 0, b''
    x0, y0, x1, y1 = bbox
    w = max(1, (x1 - x0 + ss - 1) // ss)
    h = max(1, (y1 - y0 + ss - 1) // ss)
    box = img.crop(bbox).resize((w, h), Image.LANCZOS)
    # 中间调 gamma 提亮: 1-bit 屏阈值化(alpha≥6/15 为黑)后笔画更粗, 墨量贴近官方字体
    g_boost = 0.6
    box = box.point(lambda v: min(255, int(255 * (v / 255) ** g_boost)))
    w, h = box.size
    ofs_x = x0 // ss
    ofs_y = max(0, y0 // ss - C)             # 墨迹底边贴齐基线, 且不越出行顶
    px = box.tobytes()
    if bpp == 4:
        out = bytearray()
        for row in range(h):
            line = px[row * w:(row + 1) * w]
            for i in range(0, w, 2):
                hi = line[i] >> 4
                lo = (line[i + 1] >> 4) if i + 1 < w else 0
                out.append((hi << 4) | lo)
    else:  # 1bpp, MSB-first
        out = bytearray()
        for row in range(h):
            line = px[row * w:(row + 1) * w]
            for i in range(0, w, 8):
                byte = 0
                for b in range(8):
                    if i + b < w and line[i + b] >= 128:
                        byte |= 0x80 >> b
                out.append(byte)
    return adv, w, h, ofs_x, ofs_y, bytes(out)


def build_font(ttf_path, chars, size, bpp, line_height, base_line, C, out_name):
    """生成单个 cbin 字体文件"""
    print(f'生成 {out_name}: {len(chars)} 字形, {size}px/{bpp}bpp '
          f'lh={line_height} base={base_line} ...')
    font2 = ImageFont.truetype(ttf_path, size * 2)   # 超采样渲染

    glyphs = []          # (cp, ...)
    bitmaps = bytearray()
    dsc_list = []

    for cp in chars:
        ch = chr(cp)
        adv, bw, bh, ox, oy, data = render_glyph(font2, ch, bpp, C)
        if bh == 0:
            bm_index = 0
        else:
            bm_index = len(bitmaps)
            bitmaps += data
        dsc_list.append((bm_index, adv, bw, bh, ox, oy))
        glyphs.append(cp)

    n = len(glyphs)
    range_start = min(glyphs)
    range_length = max(glyphs) - range_start + 1

    # ---- 布局计算 ----
    FONT_SZ = 36
    DSC_SZ = 24
    CMAP_SZ = 20
    GDSC_SZ = 16    # CONFIG_LV_FONT_FMT_TXT_LARGE=y → 每条 glyph_dsc 16 字节

    off_font = 0
    off_dsc = (off_font + FONT_SZ + 3) & ~3
    off_cmap = (off_dsc + DSC_SZ + 3) & ~3
    off_ulist = off_cmap + CMAP_SZ                 # unicode_list 紧随 cmap 数组
    off_gdsc = (off_ulist + n * 2 + 3) & ~3
    off_bm = (off_gdsc + n * GDSC_SZ + 3) & ~3

    # ---- lv_font_t (36B) ----
    # 3×函数指针(12) + line_height(4) + base_line(4) + 位域字节(1)
    # + underline_pos(1) + underline_thick(1) + 对齐填充(1)
    # + dsc 指针(4, 偏移24) + fallback(4) + user_data(4)
    font_bytes = struct.pack('<IIIiiBbbxI', 0, 0, 0,   # 3 个函数指针(运行时注入)
                             line_height, base_line,
                             0,                      # subpx/kerning/static 位
                             -2, 1,                  # underline_position/thickness
                             off_dsc)                # dsc 偏移
    font_bytes += struct.pack('<II', 0, 0)           # fallback, user_data

    # ---- fmt_txt_dsc_t (24B) ----
    # 指针偏移相对 dsc 位置 (加载器先 bin_addr += font->dsc 再 rebase)
    bitfield = 1 | (bpp << 9) | (0 << 13) | (0 << 14)  # cmap_num=1, kern_classes=0, PLAIN
    dsc_bytes = struct.pack('<IIII',
                            off_bm - off_dsc, off_gdsc - off_dsc,
                            off_cmap - off_dsc, 0)  # 4 指针(kern=NULL)
    dsc_bytes += struct.pack('<HHB', 16, bitfield, 0)  # kern_scale=16(1.0), stride=0
    dsc_bytes += b'\x00' * (DSC_SZ - len(dsc_bytes))

    # ---- cmap (SPARSE_TINY, 1 条范围, 20B/条: 末尾 1 字节 padding) ----
    cmap_bytes = struct.pack('<IHH', range_start, range_length, 0)   # glyph_id_start=0
    cmap_bytes += struct.pack('<I', off_ulist - off_cmap)            # unicode_list 相对 cmaps 起始
    cmap_bytes += struct.pack('<I', 0)                               # glyph_id_ofs_list = NULL
    cmap_bytes += struct.pack('<HB', n, 3)                           # SPARSE_TINY=3
    cmap_bytes += b'\x00' * (CMAP_SZ - len(cmap_bytes))

    ulist = b''.join(struct.pack('<H', cp - range_start) for cp in glyphs)

    # ---- glyph_dsc (16B × n, LARGE 格式) ----
    gdsc_bytes = b''
    for bm_index, adv, bw, bh, ox, oy in dsc_list:
        gdsc_bytes += struct.pack('<IIHHhh', bm_index, adv * 16, bw, bh, ox, oy)

    # ---- 组装 ----
    blob = bytearray(off_bm + len(bitmaps))
    blob[off_font:off_font + FONT_SZ] = font_bytes
    blob[off_dsc:off_dsc + DSC_SZ] = dsc_bytes
    blob[off_cmap:off_cmap + CMAP_SZ] = cmap_bytes
    blob[off_ulist:off_ulist + n * 2] = ulist
    blob[off_gdsc:off_gdsc + n * GDSC_SZ] = gdsc_bytes
    blob[off_bm:] = bitmaps

    out_path = os.path.join(OUT_DIR, out_name)
    with open(out_path, 'wb') as f:
        f.write(blob)
    print(f'  -> {out_path} ({len(blob) // 1024} KB)')


def main():
    ttf = prepare_bold_ttf()
    common = load_charset()
    print(f'common 字符集: {len(common)} 字')

    digits = [ord(c) for c in '0123456789:']
    for out_name, size, bpp, lh, base, C in FONT_PROFILES:
        chars = digits if '42' in out_name else common
        build_font(ttf, chars, size, bpp, lh, base, C, out_name)
    print('全部完成')


if __name__ == '__main__':
    main()
