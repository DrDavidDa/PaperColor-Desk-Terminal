# -*- coding: utf-8 -*-
# 合成 M5Burner 用的 16MB 整片 flash 镜像（与 M5Burner「导出固件」产物等价）
# 布局 (tts_16MB.csv):
#   0x0       bootloader.bin   (pio build 产物)
#   0x8000    partitions.bin   (pio build 产物)
#   0x10000   firmware.bin     (pio build 产物 app0)
#   0xC10000  voice_data       (build_voice_data.bin，从设备实读的 TTS 模型)
# NVS/otadata 不写入（空→首次启动自动格式化），确保无任何个人配置泄露。
# 用法: python merge_firmware_image.py
import subprocess, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, '.pio', 'build', 'm5stack-papercolor')
VOICE = os.path.join(HERE, 'build_voice_data.bin')
OUT = os.path.join(HERE, 'm5burner_image.bin')
FLASH_SIZE = 16 * 1024 * 1024


def check(path, label):
    if not os.path.exists(path):
        print('[FAIL] 缺少 %s: %s' % (label, path))
        sys.exit(1)


def main():
    bootloader = os.path.join(BUILD, 'bootloader.bin')
    partitions = os.path.join(BUILD, 'partitions.bin')
    firmware = os.path.join(BUILD, 'firmware.bin')
    for p, l in [(bootloader, 'bootloader'), (partitions, 'partitions'),
                 (firmware, 'firmware（先 pio run -j 1）'), (VOICE, 'voice_data（先从设备读取）')]:
        check(p, l)

    # 先铺 0xFF 底板，再按偏移写入各段
    img = bytearray([0xFF]) * FLASH_SIZE

    def put(path, offset):
        data = open(path, 'rb').read()
        assert offset + len(data) <= FLASH_SIZE, '%s 越界' % path
        img[offset:offset + len(data)] = data
        print('[OK] %-14s %8d B @ 0x%06X' % (os.path.basename(path), len(data), offset))

    put(bootloader, 0x0)
    put(partitions, 0x8000)
    put(firmware, 0x10000)
    put(VOICE, 0xC10000)

    open(OUT, 'wb').write(bytes(img))
    print('[DONE] %s (%.1f MB)' % (OUT, os.path.getsize(OUT) / 1048576))


if __name__ == '__main__':
    main()
