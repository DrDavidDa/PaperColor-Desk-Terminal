# -*- coding: utf-8 -*-
# 通过 USB 串口上传文件到设备 SD 卡（分块200字节 + ACK 流控）
# 用法: python upload_file.py <源文件> [/目标路径]
import serial, time, sys, os

PORT = 'COM4'

def wait_ack(s, ack, timeout):
    end = time.time() + timeout
    buf = b''
    while time.time() < end:
        d = s.read(512)
        if d:
            buf += d
            if ack in buf:
                return True
    return False

def main():
    if len(sys.argv) < 2:
        print('用法: python upload_file.py <源文件> [/目标路径]')
        sys.exit(1)
    src = sys.argv[1]
    if len(sys.argv) >= 3:
        dst = sys.argv[2]
    else:
        dst = '/qr/' + os.path.basename(src)
    if not dst.startswith('/'):
        dst = '/' + dst

    if not os.path.exists(src):
        print('[FAIL] 源文件不存在:', src)
        sys.exit(1)
    data = open(src, 'rb').read()
    print('源文件: %s (%d 字节) -> %s' % (src, len(data), dst))

    s = serial.Serial(PORT, 115200, timeout=0.5)
    time.sleep(0.3)

    # 等设备就绪
    ready = False
    for _ in range(20):
        s.reset_input_buffer()
        s.write(b'#STATUS\n')
        time.sleep(0.4)
        d = s.read(256)
        if d:
            print('ready:', d.decode('utf-8', 'replace').strip())
            ready = True
            break
    if not ready:
        print('[FAIL] 设备未就绪')
        s.close()
        sys.exit(1)

    # 上传前先触发一次智谱轮询并等待完成，确保设备 loop 空闲（避免 WiFi 阻塞串口导致溢出）
    print('[upload] 等待设备轮询完成（确保空闲）...', flush=True)
    s.reset_input_buffer()
    s.write(b'#POLL\n')
    end = time.time() + 75
    while time.time() < end:
        d = s.read(512)
        if d:
            txt = d.decode('utf-8', 'replace')
            if '今日token' in txt:
                print('[upload] 轮询完成，开始上传', flush=True)
                break

    # 发起上传
    s.reset_input_buffer()
    s.write(('#FILEUPLOAD|%s|%d\n' % (dst, len(data))).encode())
    if not wait_ack(s, b'file start', 10):
        print('[FAIL] 未收到 file start（SD卡/路径问题）')
        s.close()
        sys.exit(1)

    # 分块发送（每块200字节，块间固定延时让设备读走，防 USB CDC 缓冲溢出）
    CHUNK = 200
    GAP = 0.03   # 30ms/块：约 269KB/200 = 1350 块 × 30ms ≈ 40 秒
    for i in range(0, len(data), CHUNK):
        s.write(data[i:i + CHUNK])
        time.sleep(GAP)

    if not wait_ack(s, b'file done', 10):
        print('[FAIL] 未收到 file done')
        s.close()
        sys.exit(1)
    print('[OK] 上传完成: %s' % dst)
    s.close()

if __name__ == '__main__':
    main()
