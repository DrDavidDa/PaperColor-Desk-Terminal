# -*- coding: utf-8 -*-
"""临时验证：读取 PaperColor 启动日志（RTC 同步 + WiFi + 天气）并查 #STATUS"""
import serial, time

PORT = 'COM4'
BAUD = 115200

# dtr=True：设备刚烧录已复位启动，此处直接连（诊断必须 dtr=True 才有 CDC 输出）
ser = serial.Serial(PORT, BAUD, timeout=2)
ser.dtr = True
time.sleep(0.5)
ser.reset_input_buffer()

print('=== 读取启动日志（最多 50s，等 WiFi+NTP+天气）===')
deadline = time.time() + 50
done = False
while time.time() < deadline and not done:
    line = ser.readline()
    if not line:
        continue
    try:
        s = line.decode('utf-8', 'ignore').strip()
    except Exception:
        continue
    if not s:
        continue
    if any(k in s for k in ['[RTC]', '[CP]', '[WX]', '[NEWS]', '[WAKE]', '[STATUS]', '[SLEEP]', '[STANDBY]', '[CARD]', '[TTS]']):
        print(s)
    # 天气拉取完成 = 启动流程走完
    if '[WX]' in s and '大后' in s:
        done = True

print('=== 发送 #STATUS ===')
ser.write(b'#STATUS\n')
time.sleep(1.5)
end = time.time() + 3
while time.time() < end:
    line = ser.readline()
    if not line:
        continue
    try:
        s = line.decode('utf-8', 'ignore').strip()
    except Exception:
        continue
    if s:
        print(s)
ser.close()
