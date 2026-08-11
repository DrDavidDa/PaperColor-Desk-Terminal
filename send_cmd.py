import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
cmd = sys.argv[2] if len(sys.argv) > 2 else '#STATUS'
read_sec = float(sys.argv[3]) if len(sys.argv) > 3 else 25.0
s = serial.Serial(port, 115200, timeout=1)
time.sleep(1.5)                     # CDC 稳定
s.reset_input_buffer()
t0 = time.time()
s.write((cmd + '\n').encode())
s.flush()
buf = b''
end = time.time() + read_sec
while time.time() < end:
    d = s.read(4096)
    if d:
        buf += d
        print('[+%.1fs] %s' % (time.time() - t0, d.decode('utf-8', 'replace')), end='')
print('--- end ---')
s.close()
