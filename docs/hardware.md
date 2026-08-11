# 硬件说明 Hardware Guide

## 设备规格

| 项 | 规格 |
|----|------|
| 主控 | ESP32-S3R8（双核 240MHz，8MB Octal PSRAM，16MB Flash） |
| 屏幕 | 4" 600×400 **Spectra 6 彩色墨水屏**（ED2208 面板） |
| 麦克风 | ES7210（4 通道 ADC，接 MIC1） |
| 扬声器 | ES8311（DAC + Class-D 功放） |
| 温湿度 | SHT40（I²C） |
| 存储 | microSD 卡（与 EPD 共享 SPI2_HOST，需切换） |
| 状态灯 | WS2812 RGB ×2（GPIO21，Adafruit NeoPixel 驱动） |
| 按键 | BtnA=GPIO10 / BtnB=GPIO9 / BtnC=GPIO1（按下=低电平） |

> ⚠️ 注意：M5Stack PaperColor 的按键实际接在 GPIO1/9/10，**不是** I²C 引脚 GPIO2/3/5。

## 引脚映射

| 外设 | 引脚 | 说明 |
|------|------|------|
| RGB LED | GPIO21 | 2 颗 WS2812（GRB） |
| BtnA | GPIO10 | 短按/长按 |
| BtnB | GPIO9 | 短按/长按 |
| BtnC | GPIO1 | 短按/长按（**全局语音命令入口**） |
| SD SCK | GPIO14 | 与 EPD 共用 SPI2_HOST |
| SD MISO | GPIO13 | |
| SD MOSI | GPIO12 | |
| SD CS | 自动穷举 | 4/13/41/5/10 |
| I²C | GPIO2/3 | SHT40 / ES8311 / ES7210 |
| Mic 使能 | GPIO45 | 与 Speaker 共用电源+时钟，**必须互斥** |

## 关键硬件约束

1. **USB 直连**：USB-C 直连 ESP32-S3 内置 USB 外设（无独立 UART 桥），串口走 USB CDC（`CDC_ON_BOOT=1`）。
2. **麦克风/扬声器互斥**：共用 GPIO45 电源+时钟。录音前必须 `M5.Speaker.end()`，否则 ES7210 收不到采样。
3. **墨水屏刷新 4 模式**（ED2208）：
   - `epd_fast` = 黑白 Bayer 抖动（彩色会变灰，日常翻页）
   - `epd_text` = RGB 彩色抖动（比 quality 快，**彩色待机页必须用它**）
   - `epd_quality` = RGB 全彩（16 秒+，30 分钟冷却限制）
4. **屏保不刷新**：待机页只在进入时画一次，无时钟不刷新 → 省电 + 延长墨水屏寿命。
