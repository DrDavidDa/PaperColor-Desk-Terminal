# 踩坑记录 Troubleshooting

> 全部为开发过程中**实测踩过**的坑，已固化到代码/配置。

## 烧录相关

| 现象 | 根因 | 解法 |
|------|------|------|
| 上传大固件断连 | 921600 波特率写 2.6MB 固件不稳 | `upload_speed = 460800` |
| 编译 MemoryError | PlatformIO 多线程编译内存不足 | `pio run -j 1` 单线程 |
| USB 反复掉线 | USB 线/口接触不良（USB-JTAG 虚拟串口） | 换线/换口后 `read_mac` 核对 COM 口 |
| 串口无输出 | `CDC_ON_BOOT=0` 让 Serial 走 UART0（没接到电脑） | `-DARDUINO_USB_CDC_ON_BOOT=1` |
| 刷错设备 | PaperColor 与 StackChan 同为 VID:PID=303A:1001 | 上传前 `read_mac` 核对（PaperColor=`44:1b:f6:c1:7e:c8`） |

## 显示相关

| 现象 | 根因 | 解法 |
|------|------|------|
| 白屏 | PMIC 0x09 idle sleep 被改写 | Wire 写 PMIC `0x09=0x00` + `0x11=0x09` 恢复 |
| 彩色待机页变灰 | 用了 `epd_fast`（黑白 Bayer 抖动） | 彩色页面必须 `epd_text` |
| Font7 数码管缺笔画 | 7 段细笔画在彩色抖动下显示不全 | 改用 Font8（75px 实心 RLE 字体） |
| 中文字号太小 | efontCN 固定 24px | `setTextSize(3)` 整数倍放大（M5GFX 位图字体支持缩放） |
| 切页残留旧内容 | Spectra 6 快刷只更新变化的像素 | 每次 `renderScreen` 先全区域清白 |
| 全彩刷新太慢 | `epd_quality` 16 秒+ | 加 30 分钟全彩冷却，冷却期自动降级黑白快刷 |

## 音频 / 语音

| 现象 | 根因 | 解法 |
|------|------|------|
| 录音无采样 | 麦克风/扬声器共用 GPIO45 未互斥 | 录音前 `M5.Speaker.end()` |
| 录音丢采样 | 录音中触发刷屏（阻塞 2~3 秒） | 录音中绝不 `renderScreen` |
| ASR 卡死 | HTTPS 到 ASR 服务 TLS 握手卡住 | 用 HTTP(80) 明文 + `HTTP/1.1` + `Connection: close` |
| 正常录音被误删 | 含「天气/打开」等关键词被当命令 | 命令加「小彩」前缀，含前缀才走命令 |

## 联网 / 待机

| 现象 | 根因 | 解法 |
|------|------|------|
| 待机唤醒后 WiFi 不重连 | 唤醒只恢复 CPU 频率，未重连 | `wakeFromStandby()` + `reconnectWifiFromConfig()` |
| `#WX` 唤醒后立即执行失败 | 重连有延迟 | 命令内先 `reconnectWifiFromConfig()` |
| RGB LED 不亮 | PlatformIO 无 sdkconfig.h，M5 RMT 驱动空实现 | `-DM5UNIFIED_RMT_VERSION=2` + 改用 Adafruit NeoPixel |
| 待机时钟静止 | 只进待机画一次 | 待机页设计为**不刷新**（无时钟，只显示日期+天气缓存） |
