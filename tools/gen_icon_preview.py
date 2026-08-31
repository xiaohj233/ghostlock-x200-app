#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 GhostLock X200 图标 PNG 预览（与 res/drawable/ic_launcher_fg.xml 同款设计）
    用法: python3 tools/gen_icon_preview.py [输出路径] [尺寸px]
    默认输出 docs/icon-preview.png 512x512（透明底），另存 docs/icon-preview-dark.png 深底合成版。
    仅用于 README/文档预览；APK 内图标为 adaptive icon（vector），无需 PNG。"""
import math
import sys
import os
from PIL import Image, ImageDraw

BG = (28, 27, 34, 255)          # #1c1b22
GHOST = (232, 234, 240, 255)    # #E8EAF0
EYE = (28, 27, 34, 255)         # 背景色镂空
ACCENT = (96, 205, 255, 255)    # #60CDFF


def ghost_points(cx, cy, r, bottom_y, wave_amp, waves=3, step_deg=5, wave_step=6):
    """幽灵轮廓点列：上半圆弧（左→右）+ 底部波浪（右→左采样，polygon 自动闭合左侧直边）。
    返回按顺时针闭合的点列表。"""
    pts = []
    # 上半圆：theta 180°..0°（从左到右）
    for deg in range(180, -1, -step_deg):
        t = math.radians(deg)
        pts.append((cx + r * math.cos(t), cy - r * math.sin(t)))
    # 底部波浪：从右端点 (cx+r, bottom_y) 向左采样 3 个 sin 周期（波谷 bottom_y，波峰 bottom_y-wave_amp）
    span = 2 * r
    per = span / waves
    steps = waves * wave_step
    for i in range(steps, -1, -1):
        xx = cx - r + span * i / steps
        yy = bottom_y - wave_amp * (0.5 - 0.5 * math.cos(2 * math.pi * (xx - (cx - r)) / per))
        pts.append((xx, yy))
    return pts  # polygon 自动闭合 (26,78)→(26,52) 左侧直边


def render(size, transparent=True, bg_round=None):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = size / 108.0  # vector 视口 108 → 像素缩放

    def P(x, y):
        return (x * s, y * s)

    # 幽灵主体
    pts = ghost_points(cx=54 * s, cy=52 * s, r=28 * s,
                       bottom_y=78 * s, wave_amp=7 * s)
    d.polygon(pts, fill=GHOST)
    # 眼睛
    d.ellipse([P(42, 48), P(50, 56)], fill=EYE)
    d.ellipse([P(58, 48), P(66, 56)], fill=EYE)
    # 钥匙孔圆
    d.ellipse([P(49.5, 55), P(58.5, 64)], fill=ACCENT)
    # 钥匙孔三角
    d.polygon([P(54, 63), P(51.3, 70), P(56.7, 70)], fill=ACCENT)

    if not transparent:
        # 深底合成（圆角矩形背景）
        canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        cd = ImageDraw.Draw(canvas)
        r = int(size * 0.22)
        cd.rounded_rectangle([0, 0, size - 1, size - 1], radius=r, fill=BG)
        canvas.alpha_composite(img)
        return canvas
    return img


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "docs/icon-preview.png"
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 512
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    render(size, transparent=True).save(out)
    dark = out.replace(".png", "-dark.png")
    render(size, transparent=False).save(dark)
    print(f"wrote {out} + {dark}")


if __name__ == "__main__":
    main()
