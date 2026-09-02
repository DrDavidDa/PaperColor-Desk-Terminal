# -*- coding: utf-8 -*-
# 生成 M5Burner 封面：6 页实拍轮播动图 cover.gif（1280x720）+ 静态兜底 cover.jpg
# 布局：顶部标题带（常驻）+ 左侧实拍照片 + 右侧本页功能亮点，逐页轮播
# 用法: python make_cover.py
from PIL import Image, ImageDraw, ImageFont, ImageOps
import os

HERE = os.path.dirname(os.path.abspath(__file__))
IMGDIR = os.path.join(HERE, 'docs', 'images')
W, H = 1280, 720
BAND = 180          # 顶部标题带高度
PHOTO_W = 720       # 左侧照片宽（800x600 等比缩放为 720x540）
MSYHBD = 'C:/Windows/Fonts/msyhbd.ttc'
MSYH = 'C:/Windows/Fonts/msyh.ttc'

# (实拍图, 页名, 亮点行)
PAGES = [
    ('calendar.jpg',  '日历黄历',   ['农历 · 干支 · 每日宜忌', '大字月历 + 实时天气']),
    ('news.jpg',      '资讯早报',   ['IT之家 RSS 科技精选', '每日 10 条一屏速读']),
    ('study.jpg',     '考点闪卡',   ['Anki 间隔记忆算法', '问题 + 答案 + 速记口诀']),
    ('coding.jpg',    'CodingPlan', ['智谱额度三卡仪表', 'Token / 工具用量直览']),
    ('voice.jpg',     '语音待办',   ['说句话就记下', '离线中文 TTS 全文朗读']),
    ('dashboard.jpg', '状态仪表盘', ['温湿度 · 番茄钟', '备考倒计时全景']),
]


def font(path, size):
    return ImageFont.truetype(path, size)


def gradient(w, h, top, bottom):
    base = Image.new('RGB', (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        base.putpixel((0, y), tuple(int(top[i] * (1 - t) + bottom[i] * t) for i in range(3)))
    return base.resize((w, h))


def frame(idx, photo):
    canvas = gradient(W, H, (17, 23, 32), (10, 14, 19))
    d = ImageDraw.Draw(canvas)
    # 顶部标题带（每帧常驻）
    d.text((48, 30), 'M5STACK PAPERCOLOR · 4英寸 SPECTRA 6 全彩墨水屏',
           font=font(MSYHBD, 26), fill=(255, 133, 51))
    d.text((46, 66), 'PaperColor eInk Desk Terminal',
           font=font(MSYHBD, 52), fill=(255, 255, 255))
    d.text((48, 134), '黄历天气 · 科技早报 · 考点闪卡 · 番茄钟 · 语音待办 · 仪表盘',
           font=font(MSYH, 27), fill=(154, 164, 178))
    # 左侧实拍照片（800x600 → 720x540 等比）
    ph = ImageOps.fit(photo, (PHOTO_W, H - BAND), Image.LANCZOS)
    canvas.paste(ph.convert('RGB'), (0, BAND))
    d.rectangle([PHOTO_W, BAND, PHOTO_W + 2, H], fill=(255, 102, 0))
    # 右侧功能面板
    d.rectangle([PHOTO_W + 2, BAND, W, H], fill=(9, 12, 17))
    px = 762
    _, title, lines = PAGES[idx]
    d.text((px, 214), 'PAGE %02d / %02d' % (idx + 1, len(PAGES)),
           font=font(MSYHBD, 22), fill=(107, 118, 132))
    d.text((px, 256), title, font=font(MSYHBD, 46), fill=(255, 255, 255))
    d.rectangle([px, 330, px + 72, 336], fill=(255, 102, 0))
    y = 366
    for ln in lines:
        d.text((px, y), ln, font=font(MSYH, 26), fill=(184, 192, 204))
        y += 46
    d.text((px, 662), '全离线中文语音 · 待机 ~2mA · 一键烧录',
           font=font(MSYH, 20), fill=(93, 104, 116))
    return canvas


def main():
    frames = []
    for idx, (name, _, _) in enumerate(PAGES):
        with Image.open(os.path.join(IMGDIR, name)) as im:
            frames.append(frame(idx, im))
    # 静态封面（第 1 帧，日历页）
    frames[0].convert('RGB').save(os.path.join(IMGDIR, 'cover.jpg'), quality=90)
    print('[OK] cover.jpg')
    # 动图封面：逐帧自适应调色板
    pframes = [f.convert('P', palette=Image.ADAPTIVE, colors=256) for f in frames]
    out = os.path.join(IMGDIR, 'cover.gif')
    pframes[0].save(out, save_all=True, append_images=pframes[1:],
                    duration=1800, loop=0, optimize=False)
    print('[OK] cover.gif %.1f MB' % (os.path.getsize(out) / 1048576))


if __name__ == '__main__':
    main()
