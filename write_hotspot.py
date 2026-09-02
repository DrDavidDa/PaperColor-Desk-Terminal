# -*- coding: utf-8 -*-
# 手动把手机热点 WiFi 写入设备 config（绕过 netsh 检测）
# 用法: py write_hotspot.py "<热点SSID>" "<热点密码>" [家里SSID 家里密码]
# 凭据只走命令行参数，不写进代码（避免泄露）
import serial, time, os, sys

PORT = 'COM4'
HERE = os.path.dirname(os.path.abspath(__file__))

def main():
    if len(sys.argv) >= 3:
        hot_ssid, hot_pass = sys.argv[1], sys.argv[2]
    else:
        print('用法: py write_hotspot.py "<热点SSID>" "<热点密码>" [家里SSID 家里密码]')
        sys.exit(1)
    home_ssid, home_pass = (sys.argv[3], sys.argv[4]) if len(sys.argv) >= 5 else ('', '')
    print('热点: %s' % hot_ssid)

    token_file = os.path.join(HERE, 'zhipu_token.txt')
    token = open(token_file, encoding='utf-8').read().strip()
    if not token:
        print('[FAIL] zhipu_token.txt 为空')
        sys.exit(1)

    config_lines = [
        "use_wifi=true",
        "poll_interval_sec=300",
        "warn_threshold=70",
        "alert_threshold=90",
        "sd_log_enabled=true",
        "wifi_ssid=%s" % hot_ssid,          # 第1组 = 热点
        "wifi_pass=%s" % hot_pass,
    ]
    if home_ssid:
        config_lines += [
            "wifi_ssid2=%s" % home_ssid,    # 第2组 = 家里
            "wifi_pass2=%s" % home_pass,
        ]
    config_lines.append("zhipu_cookie=%s" % token)
    config = '\n'.join(config_lines) + '\n'

    s = serial.Serial(PORT, 115200, timeout=1)

    # 等待设备就绪（循环发 #STATUS 直到有回复）
    ready = False
    for _ in range(30):
        s.reset_input_buffer()
        s.write(b'#STATUS\n')
        time.sleep(0.5)
        d = s.read(256)
        if d:
            print('设备就绪:', d.decode('utf-8', 'replace').strip())
            ready = True
            break
    if not ready:
        print('[FAIL] 设备未就绪（无串口回复）')
        s.close(); sys.exit(1)

    def send_ack(cmd, ack, timeout=4):
        s.reset_input_buffer()
        s.write((cmd + '\n').encode())
        end = time.time() + timeout
        buf = b''
        while time.time() < end:
            d = s.read(1024)
            if d:
                buf += d
                if ack in buf:
                    return True
        return False

    ok = send_ack('#CFGCLEAR', b'cfg cleared')
    print('clear:', 'OK' if ok else 'TIMEOUT')
    for line in config.splitlines():
        if not line or line.startswith('zhipu_cookie='):
            continue
        ok = send_ack('#CFGLINE|' + line, b'[OK] line')
        if not ok:
            print('line TIMEOUT:', line)
            break
    for i in range(0, len(token), 80):
        ok = send_ack('#CFGTOK|' + token[i:i + 80], b'[OK] tok')
        if not ok:
            print('tok TIMEOUT at', i)
            break
    ok = send_ack('#CFGDONE', b'config saved', 6)
    print('done:', 'OK' if ok else 'TIMEOUT')

    s.reset_input_buffer()
    s.write(b'#STATUS\n')
    time.sleep(0.5)
    d = s.read(256)
    print('验证:', d.decode('utf-8', 'replace').strip() if d else '(no reply)')
    s.close()
    print('[OK] config.ini 已写入（第1组=%s%s）' % (hot_ssid, (' 第2组=' + home_ssid) if home_ssid else ''))

if __name__ == '__main__':
    main()
