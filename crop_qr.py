# -*- coding: utf-8 -*-
# 裁剪二维码图片：去掉四周多余白边，保留二维码本身 + 四周留 quiet zone
from PIL import Image, ImageOps
import sys

def main():
    src = sys.argv[1] if len(sys.argv) > 1 else r'D:\Download\Wcode.png'
    out = sys.argv[2] if len(sys.argv) > 2 else r'D:\Download\Wcode_crop.png'

    img = Image.open(src).convert('RGB')
    gray = img.convert('L')

    # 阈值：亮度 < 235 视为内容（二维码黑色模块）
    bw = gray.point(lambda p: 0 if p < 235 else 255, '1')
    inv = ImageOps.invert(bw)
    bbox = inv.getbbox()
    if not bbox:
        print('[FAIL] 未检测到二维码内容')
        sys.exit(1)

    # quiet zone（标准二维码四周需留白，防止误扫）
    m = 16
    left = max(0, bbox[0] - m)
    top = max(0, bbox[1] - m)
    right = min(img.width, bbox[2] + m)
    bottom = min(img.height, bbox[3] + m)

    cropped = img.crop((left, top, right, bottom))
    cropped.save(out)
    print('[OK] 原图 %dx%d, 内容区域 %s, 裁剪后 %dx%d, 保存到 %s'
          % (img.width, img.height, bbox, cropped.width, cropped.height, out))

if __name__ == '__main__':
    main()
