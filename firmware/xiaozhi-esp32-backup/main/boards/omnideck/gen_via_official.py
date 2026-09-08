#!/usr/bin/env python3
"""
gen_via_official.py — 用官方 lv_font_conv (78 fork, 与 xiaozhi-fonts 组件同款)
生成 OmniDeck 粗体 cbin 字体。

参数镜像组件 build.py:
  --no-compress --no-prefilter --force-fast-kern-format --format cbin
14px 镜像官方 14_1 profile: 1bpp + --autohint-off + lh=16/base=2 + fit_line_box
16/20px 用工具自然 metrics (与官方 16_4/20_4 相同字体家族, 行高一致)
42px 数字: 仅 0-9 : 空格 天, lh=64/base=12 以适配倒计时区域
"""

import json
import os
import struct
import subprocess

ROOT = r'D:/Brandon/Documents/GitHub/OmniDeck/firmware/xiaozhi-esp32-backup'
NODE = r'C:/Program Files/nodejs/node.exe'
TOOL = os.path.join(ROOT, r'managed_components/78__xiaozhi-fonts/build/tools/lv_font_conv/lv_font_conv.js')
BOLD_TTF = os.path.join(ROOT, r'main/boards/omnideck/fonts/_NotoSansSC-Bold.ttf')
# 倒计时大数字: Arial Black (900 字重), 对应效果图的 Arial Narrow 900 风格
ARIAL_BLACK = r'C:/Windows/Fonts/ariblk.ttf'
CHARSET = os.path.join(ROOT, r'managed_components/78__xiaozhi-fonts/charsets/common.json')
OUT_DIR = os.path.join(ROOT, r'main/boards/omnideck/fonts')

# (输出名, 源字体, 字号, bpp, autohint-off?, 行高, 基线, fit_line_box)
CONFIGS = [
    ('font_omni_14_bold.bin', BOLD_TTF, 14, 1, True, 16, 2, True),
    ('font_omni_16_bold.bin', BOLD_TTF, 16, 4, False, None, None, False),
    ('font_omni_20_bold.bin', BOLD_TTF, 20, 4, False, None, None, False),
    ('font_omni_42_digits.bin', ARIAL_BLACK, 42, 4, False, 64, 12, False),
]


def load_symbols():
    cps = json.load(open(CHARSET, encoding='utf-8'))['codepoints']
    cps = sorted(c for c in cps if 0x20 <= c <= 0x10FFFF and not (0xD800 <= c <= 0xDFFF))
    return ''.join(chr(c) for c in cps)


def postprocess_cbin(path, line_height, base_line, fit_line_box):
    """镜像 78__xiaozhi-fonts/scripts/build.py 的 postprocess_cbin_font"""
    if line_height is None and base_line is None and not fit_line_box:
        return
    data = bytearray(open(path, 'rb').read())
    if len(data) < 28:
        raise RuntimeError('Invalid CBIN font header')
    if line_height is not None:
        struct.pack_into('<i', data, 12, line_height)
    if base_line is not None:
        struct.pack_into('<i', data, 16, base_line)
    if fit_line_box:
        if line_height is None or base_line is None:
            raise RuntimeError('glyph positioning requires line_height and base_line')
        descriptor = struct.unpack_from('<I', data, 24)[0]
        _, glyph_offset, cmap_offset = struct.unpack_from('<III', data, descriptor)
        glyph_start = descriptor + glyph_offset
        glyph_end = descriptor + cmap_offset
        if glyph_start > glyph_end or (glyph_end - glyph_start) % 16 != 0:
            raise RuntimeError('Invalid CBIN glyph descriptor table')
        for offset in range(glyph_start, glyph_end, 16):
            box_h = struct.unpack_from('<H', data, offset + 10)[0]
            ofs_y = struct.unpack_from('<h', data, offset + 14)[0]
            if box_h > line_height:
                continue    # 高于行框的字形保持原值 (官方此处直接报错, 我们宽容处理)
            fitted = min(max(ofs_y, -base_line), line_height - base_line - box_h)
            struct.pack_into('<h', data, offset + 14, fitted)
    open(path, 'wb').write(data)


def postprocess_bottom_align(path, line_height, base_line):
    """42px 数字专用: 统一墨迹底边。

    LVGL 9.5 绘制公式 (lv_draw_label.c):
        letter_coords.y1 = 行顶 + (line_height - base_line) - box_h - ofs_y
    即 墨底 = 行顶 + (line_height - base_line) - ofs_y, 故 ofs_y = 基线 - 墨底。
    所有字形统一 ofs_y = 1 → 墨底 = 基线 - 1, 底边完全对齐。
    """
    data = bytearray(open(path, 'rb').read())
    descriptor = struct.unpack_from('<I', data, 24)[0]
    _, glyph_offset, cmap_offset = struct.unpack_from('<III', data, descriptor)
    glyph_start = descriptor + glyph_offset
    glyph_end = descriptor + cmap_offset
    for offset in range(glyph_start, glyph_end, 16):
        box_h = struct.unpack_from('<H', data, offset + 10)[0]
        if box_h == 0:
            continue
        struct.pack_into('<h', data, offset + 14, 1)
    open(path, 'wb').write(data)


def main():
    symbols = load_symbols()
    print(f'common 字符集: {len(symbols)} 字')
    for out_name, src_ttf, size, bpp, autohint_off, lh, base, fit in CONFIGS:
        chars = '0123456789:-' if '42' in out_name else symbols
        out_path = os.path.join(OUT_DIR, out_name)
        cmd = [NODE, TOOL, '--no-compress', '--no-prefilter',
               '--force-fast-kern-format', '--font', src_ttf]
        if autohint_off:
            cmd.append('--autohint-off')
        cmd += ['--format', 'cbin', '--bpp', str(bpp), '--size', str(size),
                '--symbols', chars, '-o', out_path]
        print(f'生成 {out_name}: {size}px/{bpp}bpp, {len(chars)} 字形 ...')
        r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8')
        if r.returncode != 0:
            print('  失败:', (r.stdout + r.stderr)[-800:])
            return 1
        postprocess_cbin(out_path, lh, base, fit)
        if '42' in out_name:
            postprocess_bottom_align(out_path, lh, base)
        print(f'  -> {out_name} ({os.path.getsize(out_path) // 1024} KB)')
    print('全部完成')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
