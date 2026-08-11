# -*- coding: utf-8 -*-
# 综合验证：清旧录音 + 触发智谱轮询（额度 + 今日token）
import serial, time, sys

s = serial.Serial('COM4', 115200, timeout=0.5)
time.sleep(0.3)

def send(cmd, listen_sec):
    s.reset_input_buffer()
    print('[test] >> %s' % cmd, flush=True)
    s.write((cmd + '\n').encode())
    end = time.time() + listen_sec
    got = b''
    while time.time() < end:
        d = s.read(1024)
        if d:
            got += d
            sys.stdout.write(d.decode('utf-8', 'replace'))
            sys.stdout.flush()
    print('\n[test] << %s 完成' % cmd, flush=True)

# 等设备就绪
ready = False
for _ in range(20):
    s.reset_input_buffer()
    s.write(b'#STATUS\n')
    time.sleep(0.4)
    d = s.read(256)
    if d:
        print('ready:', d.decode('utf-8', 'replace').strip(), flush=True)
        ready = True
        break
if not ready:
    print('[FAIL] 设备未就绪')
    s.close()
    raise SystemExit(1)

send('#CLEARREC', 3)
send('#POLL', 75)
s.close()
print('[test] 结束', flush=True)
