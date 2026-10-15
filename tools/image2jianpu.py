#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""简谱图片 → 辅助读谱报告（只给像素证据，不给结论）。

为什么要有它：视觉模型看整张简谱会“编”节奏。本项目真踩过——一张没印减时线的谱，
先被判读成“小节 7 有减时线”，回原始灰度逐像素复核才发现是看错了。所以把**能算清楚的
部分**（字形位置与包围盒、基线下的减时线层数、附点、高低八度点）交给像素，
**认字**（这个字形是几）再按裁好的小格去做——两边都留下可复核的原始证据。

它不做的事：不猜音高、不猜节奏、不为了“每小节凑满拍数”补音符。看不清就报“不确定”。

用法：
    python tools/image2jianpu.py 谱面图.png
    python tools/image2jianpu.py 谱面图.png --prefix out/父亲
    python tools/image2jianpu.py 谱面图.png --min-band 10 --max-band 80

输出（前缀默认为图片名）：
    <前缀>_report.txt    每个字形一行：带/序号/x1/y1/x2/y2/宽/高/基线/减时线层数(三档阈值)/附点/上点/下点
    <前缀>_boxes.png     把认出的字形框画回原图（红框 = 字形，蓝框 = 检出的小点），人工复核用
    <前缀>_glyphs.png    字形拼版图：每个字形放大 2 倍并标序号，用来逐格认数字
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("需要 Pillow：python -m pip install pillow")

THRESHOLDS = (128, 200, 225)      # 三档取一致结果（见技能里的像素级读谱规则）
BASE_LINE_WINDOW = range(2, 8)    # 基线（字形底边）下方 2~7 px 是减时线所在区间
LINE_MIN_RATIO = 0.55             # 一条横线至少盖住字形宽度的这个比例
DOT_MAX_W = 7                     # 小点（附点/高低八度点）的宽度上限
DOT_MAX_H = 7                     # 小点的高度上限


def build_rows(px, w, h, thr):
    """整图二值化：每行一个 bytearray，1 = 墨迹。"""
    rows = []
    for y in range(h):
        rows.append(bytearray(1 if px[x, y] < thr else 0 for x in range(w)))
    return rows


def find_bands(rows, w, h, min_band, max_band):
    """按水平投影切出“墨迹带”（音符行 / 歌词行 / 标题行）。"""
    bands = []
    y = 0
    while y < h:
        if any(rows[y]):
            y0 = y
            gap = 0
            while y < h and gap < 4:
                gap = gap + 1 if not any(rows[y]) else 0
                y += 1
            y1 = y - gap
            if min_band <= (y1 - y0) <= max_band:
                bands.append((y0, y1 - 1))
        else:
            y += 1
    return bands


def find_glyphs(rows, x0, x1, y0, y1):
    """带内按垂直投影切字，返回每个字形的紧包围盒。"""
    cols = []
    for x in range(x0, x1 + 1):
        cols.append(any(rows[y][x] for y in range(y0, y1 + 1)))
    groups = []
    x = 0
    while x < len(cols):
        if cols[x]:
            sx = x
            gap = 0
            while x < len(cols) and gap < 2:
                gap = gap + 1 if not cols[x] else 0
                x += 1
            groups.append((x0 + sx, x0 + x - gap - 1))
        else:
            x += 1
    out = []
    for gx0, gx1 in groups:
        ys = [y for y in range(y0, y1 + 1) if any(rows[y][x] for x in range(gx0, gx1 + 1))]
        xs = [xx for xx in range(gx0, gx1 + 1) if any(rows[y][xx] for y in range(y0, y1 + 1))]
        if ys and xs:
            out.append((min(xs), min(ys), max(xs), max(ys)))
    return out


def count_underlines(rows, box):
    """数基线下方 2~7 px 里的横线层数；返回 (层数, 说明)。"""
    x1, y1, x2, y2 = box
    span = x2 - x1 + 1
    hits = []
    for dy in BASE_LINE_WINDOW:
        y = y2 + dy
        if y >= len(rows):
            continue
        run = best = 0
        for x in range(x1, x2 + 1):
            run = run + 1 if rows[y][x] else 0
            best = max(best, run)
        hits.append(1 if best >= span * LINE_MIN_RATIO else 0)
    layers = 0
    prev = 0
    for h in hits:
        if h and not prev:
            layers += 1
        prev = h
    return layers


def small_blobs(rows, x_lo, x_hi, y_lo, y_hi):
    """在矩形区域内找小墨块（宽高都不超过阈值）→ 附点 / 高低八度点。"""
    x_lo, x_hi = max(0, x_lo), x_hi
    y_lo, y_hi = max(0, y_lo), min(len(rows) - 1, y_hi)
    if x_hi < x_lo or y_hi < y_lo:
        return 0
    for y in range(y_lo, y_hi + 1):
        run = 0
        for x in range(x_lo, x_hi + 1):
            if rows[y][x]:
                run += 1
            else:
                if 0 < run <= DOT_MAX_W:
                    col_h = sum(1 for yy in range(y_lo, y_hi + 1) if rows[yy][x - 1])
                    if col_h <= DOT_MAX_H:
                        return 1
                run = 0
        if 0 < run <= DOT_MAX_W:
            return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description="简谱图片 → 辅助读谱报告（像素证据）")
    ap.add_argument("image")
    ap.add_argument("--prefix", default=None, help="输出前缀（默认取图片名）")
    ap.add_argument("--min-band", type=int, default=8, help="墨迹带最小高度（默认 8）")
    ap.add_argument("--max-band", type=int, default=90, help="墨迹带最大高度（默认 90）")
    args = ap.parse_args()

    if not os.path.isfile(args.image):
        sys.exit("找不到图片：" + args.image)
    prefix = args.prefix or os.path.splitext(args.image)[0]
    im = Image.open(args.image).convert("L")
    w, h = im.size
    px = im.load()
    rows = build_rows(px, w, h, THRESHOLDS[1])

    bands = find_bands(rows, w, h, args.min_band, args.max_band)
    report = []
    report.append("图片：%s  %d×%d" % (args.image, w, h))
    report.append("墨迹带 %d 条（默认只报高度 %d~%d 的带）" % (len(bands), args.min_band, args.max_band))
    report.append("列：带/序号/x1/y1/x2/y2/宽/高/基线/减时线层数/附点/上点/下点")

    draw = ImageDraw.Draw(im.convert("RGB"))
    color = Image.open(args.image).convert("RGB")
    draw = ImageDraw.Draw(color)
    crops = []
    n = 0

    for bi, (y0, y1) in enumerate(bands, start=1):
        glyphs = find_glyphs(rows, 0, w - 1, y0, y1)
        report.append("--- 带 %d：y %d~%d（高 %d），切出 %d 个字形" % (bi, y0, y1, y1 - y0 + 1, len(glyphs)))
        for gi, box in enumerate(glyphs, start=1):
            x1, y1g, x2, y2 = box
            gw, gh = x2 - x1 + 1, y2 - y1g + 1
            layers = count_underlines(rows, box)
            dot_right = small_blobs(rows, x2 + 1, x2 + 9, y1g, y2)
            dot_up = small_blobs(rows, x1, x2, y1g - 8, y1g - 2)
            dot_dn = small_blobs(rows, x1, x2, y2 + 2, y2 + 9) if layers == 0 else 0
            n += 1
            report.append("%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d" % (
                bi, gi, x1, y1g, x2, y2, gw, gh, y2, layers, dot_right, dot_up, dot_dn))
            draw.rectangle([x1 - 1, y1g - 1, x2 + 1, y2 + 1], outline=(220, 30, 30))
            note = "%d.%d L%d%s%s%s" % (bi, gi, layers,
                                        " D" if dot_right else "", " U" if dot_up else "", " B" if dot_dn else "")
            draw.text((x1, max(0, y1g - 12)), note, fill=(20, 90, 200))
            crops.append((note, color.crop((max(0, x1 - 2), max(0, y1g - 10), min(w, x2 + 10), min(h, y2 + 10)))))

    if crops:
        cw = max(c.size[0] for _, c in crops) * 2 + 12
        ch = max(c.size[1] for _, c in crops) * 2 + 20
        per_row = max(1, min(10, 1600 // max(cw, 1)))
        rows_n = (len(crops) + per_row - 1) // per_row
        sheet = Image.new("RGB", (per_row * cw, rows_n * ch), (255, 255, 255))
        sd = ImageDraw.Draw(sheet)
        for i, (note, c) in enumerate(crops):
            c = c.resize((c.size[0] * 2, c.size[1] * 2))
            ox, oy = (i % per_row) * cw, (i // per_row) * ch
            sheet.paste(c, (ox + 4, oy + 16))
            sd.text((ox + 4, oy + 2), note, fill=(180, 20, 20))
        sheet.save(prefix + "_glyphs.png")
    color.save(prefix + "_boxes.png")
    with open(prefix + "_report.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    print("\n".join(report[:3]))
    print("共 %d 个字形；输出：%s_report.txt / %s_boxes.png / %s_glyphs.png" % (n, prefix, prefix, prefix))


if __name__ == "__main__":
    main()
