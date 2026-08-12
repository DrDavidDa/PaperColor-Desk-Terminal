# -*- coding: utf-8 -*-
"""PaperColor 二次审查修复验证：启动 / 联网(RSS/WX) / 串口诊断路径(#PLAY #CFG)"""
import serial, time, sys, subprocess

PORT = 'COM4'
BAUD = 115200

def hard_reset():
    """用 esptool 硬复位设备（DTR/RTS 时序在 ESP32-S3 USB-JTAG 上不可靠）。
    复位后设备从零启动重新计时（3 分钟待机窗口），保证回归不被 light sleep 打断。"""
    subprocess.run(['py', '-m', 'esptool', '--port', PORT, 'hard_reset'],
                   capture_output=True, timeout=30)

def open_serial():
    """hard_reset 后 USB 重新枚举有延迟，轮询等待 COM4 可打开（最多 20 秒）。"""
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            return serial.Serial(PORT, BAUD, timeout=2)
        except Exception:
            time.sleep(1.0)
    raise SystemExit('!! COM4 打开失败（设备可能进入 light sleep，需按键唤醒后重试）')

def main():
    hard_reset()
    ser = open_serial()
    time.sleep(0.5)
    # 清空启动期输出（设备启动含 SD+TTS 约 20 秒，此处等待就绪）
    deadline = time.time() + 3
    while time.time() < deadline:
        ser.readline()

    def cmd(c, wait=3.0):
        # 心跳：先发 #STATUS 刷新设备闲置计时，防测试期间被 light sleep 打断（USB 挂起后无法串口唤醒）
        ser.write(b'#STATUS\n')
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(c.encode() + b'\n')
        out = []
        end = time.time() + wait
        while time.time() < end:
            line = ser.readline()
            if not line:
                continue
            try:
                s = line.decode('utf-8', 'ignore').strip()
            except Exception:
                continue
            if s:
                out.append(s)
            # 命令 ACK/完成标志
            if any(k in s for k in ['[OK]', '[STATUS]', '[NEWS]', '[WX]', '[PLAY]', '[CFG]', '完成', '成功']):
                pass
        return out

    # 1. 状态（等 cfg_ok 确认设备就绪；设备启动含 SD+TTS 约 20 秒，轮询 25 次×2 秒足够）
    ready = False
    for i in range(25):
        lines = cmd('#STATUS', 2.0)
        joined = ' '.join(lines)
        if 'cfg_ok=1' in joined or 'cfg_ok=0' in joined:
            print('--- #STATUS ---')
            for l in lines: print(l)
            ready = True
            break
    if not ready:
        print('!! 设备未就绪'); ser.close(); return

    # 2. RSS 联网（验证联网链路未被破坏）
    print('--- #NEWS (RSS) ---')
    for l in cmd('#NEWS', 16.0): print(l)

    # 3. 天气联网
    print('--- #WX ---')
    for l in cmd('#WX', 14.0): print(l)

    # 4. config 读取（验证 #CFG 相关路径不崩溃）
    print('--- #CFGREAD ---')
    for l in cmd('#CFGREAD', 3.0): print(l)

    # 5. #PLAY 诊断（验证 break 修复：命令不卡、缓冲不残留）
    print('--- #PLAY|0 ---')
    for l in cmd('#PLAY|0', 4.0): print(l)
    print('--- 之后 #PSTATE（验证缓冲无残留）---')
    for l in cmd('#PSTATE', 3.0): print(l)

    ser.close()
    print('\n==== 验证完成 ====')

if __name__ == '__main__':
    sys.exit(main())
