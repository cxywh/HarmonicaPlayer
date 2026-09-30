#!/usr/bin/env python3
"""把一张方形 PNG 转成 Windows 多尺寸 .ico（给 HarmonicaPlayer 换图标用）。

用法：
    python tools/make-icon.py logo.png src/ui/harmonica.ico

需要 Pillow。本机没有系统 pip 时可以用 Hermes 自带的解释器：
    "C:/Users/<你>/AppData/Local/hermes/hermes-agent/venv/Scripts/python.exe" tools/make-icon.py logo.png src/ui/harmonica.ico

要点（踩过的坑）：
  * 先按 alpha 裁掉透明外边距，**再补成正方形**再缩放 —— 裁完直接缩放会让图不再方，
    Pillow 写 ICO 时按比例适配，条目尺寸就会变成 23x24 这种非方形，Windows 上显示发虚。
  * 尺寸带齐 16/20/24/32/40/48/64/128/256：任务栏用 32、资源管理器大图标用 256。
  * 细节多的插画在 16px 必然糊；要 16px 也清楚只能单独做一版极简图。
"""
import os
import struct
import sys

from PIL import Image

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def build(source: str, target: str) -> None:
    image = Image.open(source).convert("RGBA")
    box = image.getchannel("A").getbbox()
    if box:
        image = image.crop(box)
    side = max(image.size)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(image, ((side - image.width) // 2, (side - image.height) // 2), image)
    canvas.save(target, format="ICO", sizes=[(s, s) for s in SIZES])
    count = struct.unpack("<H", open(target, "rb").read()[4:6])[0]
    print(f"已写 {target}：{os.path.getsize(target)} 字节，{count} 张（{SIZES}）")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        raise SystemExit(2)
    build(sys.argv[1], sys.argv[2])
