import serial, time

s = serial.Serial('COM4', 115200, timeout=1)
time.sleep(0.5)
s.reset_input_buffer()
# 复位触发设备重启
s.setDTR(False); s.setRTS(True); time.sleep(0.1)
s.setDTR(True);  time.sleep(0.1)
s.setDTR(False); time.sleep(0.3)

# 持续读取 8 秒
buf = b''
end = time.time() + 8
while time.time() < end:
    d = s.read(4096)
    if d:
        buf += d
txt = buf.decode('utf-8', 'replace')
print('=== RAW ===')
print(repr(txt))
print('=== 可见文本 ===')
print(txt if txt.strip() else '(nothing)')
s.close()
