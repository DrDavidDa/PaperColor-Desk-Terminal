# -*- coding: utf-8 -*-
# 设备 WiFi + 智谱 token 自动同步脚本
# 用法：
#   1. 当前连接 WiFi 的设备在 USB 上（COM4）
#   2. 运行 python write_config.py
#   3. 脚本自动：检测电脑当前 WiFi → 读密码 → 拼 config.ini → 串口 #CFG| 写入设备 SD /config.ini
# 换办公环境后：插上 USB 重跑本脚本，设备 WiFi 自动跟随电脑（智谱 token 不变，存 zhipu_token.txt）
import serial, time, subprocess, re, os, sys

PORT = 'COM4'

def get_current_wifi():
    out = subprocess.run(['netsh', 'wlan', 'show', 'interfaces'],
                         capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    m = re.search(r'SSID\s*:\s*(.+)', out)
    return m.group(1).strip() if m else None

def get_wifi_pass(ssid):
    out = subprocess.run(['netsh', 'wlan', 'show', 'profile', 'name=' + ssid, 'key=clear'],
                         capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    m = re.search(r'关键内容\s*:\s*(.+)', out) or re.search(r'Key Content\s*:\s*(.+)', out)
    return m.group(1).strip() if m else None

def main():
    # 智谱 token（浏览器 Cookie 里 bigmodel_token_production 的值）
    token_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'zhipu_token.txt')
    if not os.path.exists(token_file):
        print('[FAIL] 缺少 zhipu_token.txt（放入智谱 bigmodel_token_production token）')
        sys.exit(1)
    token = open(token_file, encoding='utf-8').read().strip()
    if not token:
        print('[FAIL] zhipu_token.txt 为空')
        sys.exit(1)

    ssid = get_current_wifi()
    if not ssid:
        print('[FAIL] 未检测到已连接的 WiFi')
        sys.exit(1)
    passwd = get_wifi_pass(ssid)
    if not passwd:
        print('[FAIL] 无法读取 %s 的密码（该 WiFi 未在电脑保存密码？）' % ssid)
        sys.exit(1)

    print('当前 WiFi: %s' % ssid)
    print('密码: %s' % passwd)

    # 生成 config.ini（第一组=当前 WiFi；wifi_extra.txt 追加公司等额外网络，每行 SSID=密码）
    config_lines = [
        "use_wifi=true",
        "poll_interval_sec=300",
        "warn_threshold=70",
        "alert_threshold=90",
        "sd_log_enabled=true",
        "wifi_ssid=%s" % ssid,
        "wifi_pass=%s" % passwd,
    ]
    extra_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wifi_extra.txt')
    seen = {ssid}
    idx = 2
    if os.path.exists(extra_file):
        for ln in open(extra_file, encoding='utf-8'):
            ln = ln.strip()
            if not ln or '=' not in ln:
                continue
            es, ep = ln.split('=', 1)
            es = es.strip()
            ep = ep.strip()
            if not es or es in seen:
                continue
            seen.add(es)
            if idx <= 3:
                config_lines.append('wifi_ssid%d=%s' % (idx, es))
                config_lines.append('wifi_pass%d=%s' % (idx, ep))
                idx += 1
                print('额外 WiFi: %s' % es)
    # SiliconFlow API Key（可选，用于语音待办转文字；不存在则跳过）
    sf_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sf_api_key.txt')
    if os.path.exists(sf_file):
        sfk = open(sf_file, encoding='utf-8').read().strip()
        if sfk:
            config_lines.append('sf_api_key=%s' % sfk)
            print('SiliconFlow Key: %s...%s' % (sfk[:6], sfk[-4:]))
    # 天气多城市（可选 weather.txt：每行 lat=39.9042 / lon=116.4074 / city=上海；不存在则默认北京）
    wx_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'weather.txt')
    if os.path.exists(wx_file):
        for ln in open(wx_file, encoding='utf-8'):
            ln = ln.strip()
            if '=' in ln:
                k, v = ln.split('=', 1)
                k = k.strip()
                if k in ('weather_lat', 'weather_lon', 'weather_city') and v.strip():
                    config_lines.append('%s=%s' % (k, v.strip()))
                    print('天气配置: %s=%s' % (k, v.strip()))
    config_lines.append('zhipu_cookie=%s' % token)
    config = '\n'.join(config_lines) + '\n'

    try:
        s = serial.Serial(PORT, 115200, timeout=1)
    except Exception as e:
        print('[FAIL] 无法打开 %s：%s' % (PORT, e))
        sys.exit(1)

    # 等待设备就绪：循环发 #STATUS 直到有回复（设备 boot 慢，墨水屏初始化需数秒）
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
        s.close()
        sys.exit(1)

    # 逐条发送并等待 ACK（真流控，防止 USB CDC 缓冲积压溢出丢数据）
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
            continue  # token 用分块 #CFGTOK 发送
        ok = send_ack('#CFGLINE|' + line, b'[OK] line')
        if not ok:
            print('line TIMEOUT:', line)
            break
    # token 分块发送（每块 ~80 字符）
    token = [kv for kv in config.splitlines() if kv.startswith('zhipu_cookie=')][0][len('zhipu_cookie='):]
    for i in range(0, len(token), 80):
        ok = send_ack('#CFGTOK|' + token[i:i + 80], b'[OK] tok')
        if not ok:
            print('tok TIMEOUT at', i)
            break
    ok = send_ack('#CFGDONE', b'config saved', 6)
    print('done:', 'OK' if ok else 'TIMEOUT')

    # 验证加载结果
    s.reset_input_buffer()
    s.write(b'#STATUS\n')
    time.sleep(0.5)
    d = s.read(256)
    print('验证:', d.decode('utf-8', 'replace').strip() if d else '(no reply)')
    s.close()
    print('[OK] config.ini 已写入设备 SD 卡（SSID=%s）' % ssid)

if __name__ == '__main__':
    main()
