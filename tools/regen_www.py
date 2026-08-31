#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""regen_www.py — 把 vite online2 构建产物重组为 WebView file:// 可加载的 www 目录。
功能：
  1. index.html 去 type="module"（file:// 下 ESM 被 CORS 拦）、去 crossorigin、
     路径 ../assets/→./assets/；
  2. dist-online/assets/* 平铺复制到 www/assets/；
  3. 校验产物 JS 无 import.meta / export / module（单 chunk 应无）。
用法：python3 tools/regen_www.py
"""
import os
import re
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST_DIR = os.path.join(ROOT, 'ui-preview', 'dist-online', 'online')
DIST_ASSETS = os.path.join(ROOT, 'ui-preview', 'dist-online', 'assets')
WWW = os.path.join(ROOT, 'app', 'src', 'main', 'assets', 'www')

def main():
    idx = os.path.join(DIST_DIR, 'index.html')
    with open(idx, 'r', encoding='utf-8') as f:
        html = f.read()
    # 去 module（file:// 下 ESM 被 CORS 拦）→ 替换为 defer：
    #   module 脚本天然延迟执行（等 DOM 解析完，#app 容器已存在）；
    #   直接删 type 变普通同步脚本会在 head 解析时立即执行 → mount('#app') 找不到容器 → 黑屏。
    #   故必须保留延迟语义。去 crossorigin + ../assets 相对化。
    html = re.sub(r'\btype="module"\s*', 'defer ', html)
    html = html.replace('crossorigin', '').replace('../assets/', './assets/')
    os.makedirs(WWW, exist_ok=True)
    # 清空旧 www（仅 index/assets）
    for name in os.listdir(WWW):
        p = os.path.join(WWW, name)
        if os.path.isdir(p):
            shutil.rmtree(p)
        else:
            os.remove(p)
    with open(os.path.join(WWW, 'index.html'), 'w', encoding='utf-8') as f:
        f.write(html)
    shutil.rmtree(os.path.join(WWW, 'assets'), ignore_errors=True)
    shutil.copytree(DIST_ASSETS, os.path.join(WWW, 'assets'))
    # 校验
    js_files = [f for f in os.listdir(os.path.join(WWW, 'assets')) if f.endswith('.js')]
    assert len(js_files) == 1, f'expected 1 js, got {js_files}'
    js = open(os.path.join(WWW, 'assets', js_files[0]), encoding='utf-8').read()
    bad = [k for k in ('import.meta', '\nexport ', 'module.exports') if k in js]
    if bad:
        raise SystemExit(f'JS 仍有 ESM 残留: {bad}')
    print(f'www 重组完成: {WWW}')
    print(f'  index.html: {open(os.path.join(WWW, "index.html"), encoding="utf-8").read().strip()}')

if __name__ == '__main__':
    main()