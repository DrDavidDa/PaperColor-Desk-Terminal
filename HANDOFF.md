# 📟 PaperColor 项目交接清单（HANDOFF）

> **项目**：M5Stack PaperColor 桌面学习终端（eInk Desk Terminal）
> **交接时间**：2026-08-13（2026-08-27 全面梳理更新）
> **一句话**：把官方 4 寸 Spectra 6 彩色墨水屏开发板做成低功耗常亮桌面学习终端 —— 日历黄历/资讯早报/考点闪卡/番茄钟/语音待办/环境仪表盘/二维码，全部离线可用。
> **⚠️ 完整权威交接**：👉 **`docs/COMPLETE-HANDOFF.md`**（2026-08-27 全量梳理版：固件架构/全部资源/脚本清单/云端API/路线图，接手 AI 必读本文档 + 该文件）
> **配套文档**：本清单 ← `docs/COMPLETE-HANDOFF.md`（权威）← `PROGRESS.md`（进展快照+会话摘要）← 根目录 `CHANGELOG.md` / `README.md` / `docs/` ← repo 记忆 `papercolor-epd.md`（全部技术坑）
> **唤醒词**：「继续小彩」= 恢复本项目上下文（先读 repo 记忆 + docs/COMPLETE-HANDOFF.md + PROGRESS.md）

---

## 1. 硬件与平台

| 项 | 规格 |
|----|------|
| 主控 | ESP32-S3R8 双核 240MHz（16MB Flash + 8MB PSRAM，qio_opi） |
| 屏幕 | 4 寸 E-Ink **Spectra 6 全彩** 400×600（ED2208 面板，board 28） |
| 电池 | 1250mAh，待机仅 ~2mA（light sleep） |
| 语音 | ES8311 编解码（I2S_NUM_0）+ ES7210 麦克风（I2S_NUM_1）+ 1W 喇叭 |
| 传感 | SHT40 温湿度 + RX8130CE RTC |
| 扩展 | microSD + 红外 + 2×RGB（WS2812 GPIO21）+ HY2.0 |

### 引脚/总线速查
- SPI2_HOST：SCK=15 / MISO=14 / MOSI=13（3-wire）；EPD panel CS=44，RST=43，BUSY=11；**SD 卡 CS=GPIO47 与 EPD 共享 SPI2_HOST**
- **按键：BtnA=10 / BtnB=9 / BtnC=1**（⚠️ 旧记录 2/3/5 是错的，那是 I2C 引脚）
- 音频 codec 共用 GPIO45 电源 + MCLK/BCK/WS(42/40/41)；Mic data=39
- M5PM1 PMIC @ 0x6E：GPIO0=EPD 电源 / GPIO3=SD 电源
- LED：Adafruit_NeoPixel(2, GPIO21, NEO_GRB+NEO_KHZ800)

---

## 2. 功能全景（7 页）

| # | 页面 | 核心 | A | B | 长按A | 长按B | 长按C |
|---|------|------|---|---|-------|-------|-------|
| 0 | 日历黄历 | 横幅+月历+天气卡+黄历 | 前一天 | 后一天 | 跳末页 | 报天气 | 刷天气 |
| 1 | 资讯早报 | IT之家 RSS 10 条（过滤非科技） | 上条 | 下条 | — | 朗读 | 联网刷 |
| 2 | 考点闪卡 | SD 卡片 问题/答案/速记 | 上条 | 下条 | 标记掌握 | 朗读 | 强刷 |
| 3 | CodingPlan | 智谱额度 3 卡+Token+工具用量 | 上页 | — | — | — | 强刷 |
| 4 | 语音待办 | 录音/转写/详情/回放 | 上条 | 下条 | 详情回放 | 删除 | 切页 |
| 5 | 仪表盘 | 环境/设备/番茄/备考 | 上页 | 番茄开停 | — | — | 强刷 |
| 6 | 二维码 | SD 二维码图 | 上页 | 下页 | — | — | 关机菜单 |

**全局能力**：RGB 状态灯 / 5 分钟待机省电(light sleep) / 定时语音提醒 / 离线中文 TTS(esp-sr) / 语音助手「小彩」/ 串口诊断(#命令) / 多 WiFi(3 组) / 墨水屏刷新管控(30min 全彩冷却)

> ⚠️ 页面顺序改动必须**全量同步**：`PAGE_TITLES` / `HINTS` / `pageBuf` switch / `renderScreen` switch / 按键 `%N` / `refreshPageData` —— 漏一处就"按键没反应"或底栏错乱。

---

## 3. 开发环境与工具链

- **框架**：PlatformIO + Arduino，board `esp32-s3-devkitc-1`，partition `tts_16MB.csv`（含 3MB voice_data TTS 模型 @0xC90000）
- **关键 build_flags**：`ARDUINO_USB_CDC_ON_BOOT=1`（串口走 USB CDC 才能通 COM4！）、`M5UNIFIED_RMT_VERSION=2`（否则 LED 空实现）、esp-sr TTS 链接
- **依赖**：M5Unified ^0.2.5 / M5GFX ^0.2.7 / ArduinoJson ^7 / Adafruit NeoPixel
- **编译上传**：`pio run -j 1 -t upload --upload-port COM4`（`-j 1` 防 OOM；约 45-250s）
- **编译 MemoryError**：清理 `.pio/build/m5stack-papercolor` 重编

---

## 4. 设备识别与串口（⚠️ 易踩坑）

| 设备 | 端口 | SER 序列号 |
|------|------|-----------|
| **PaperColor** | **COM4** | `44:1B:F6:C1:7E:C8` |
| StackChan | COM6 | `44:1B:F6:E5:62:74` |

- 两台同为 VID:PID=`303A:1001` USB-JTAG，**必须靠 SER 区分**；上传/诊断前先 `py -m esptool --port COM4 read_mac` 核对 MAC
- `upload_speed=460800`（921600 写 2.6MB 固件断连）
- **pySerial 打开 COM4（dtr=True）会触发设备复位**——打开后须轮询 `#STATUS` 直到 `cfg_ok` 就绪再发命令；`dtr=False` 会抑制 CDC 输出（只用于 listen_wake.py 等特殊场景）
- **light sleep 后 USB-JTAG 数据通路可能卡死**（COM4 可开但 esptool 握手失败）→ **物理重插 USB 线**恢复；唤醒仅按键（此 IDF 无 USB 串口唤醒 API）
- 串口协议：USB CDC、`\n` 结束符；大 config 逐条 `#CFGLINE`/`#CFGTOK` 分块+ACK（USB CDC 493B 溢出坑）

---

## 5. 核心文件结构

```
src/main.cpp            ← 固件主体（~4400 行，全部功能在此）
include/lunar_cal.h     ← 农历/干支/黄历 1900-2100 数据表
lib/esptts/             ← 离线中文 TTS 引擎（esp_tts_chinese + voice_set_xiaole .a）
cards/ch01.json...      ← 考点卡片（parse_cards.py 从 Anki md 生成）
docs/                   ← hardware.md / voice-commands.md / troubleshooting.md
platformio.ini          ← 平台/分区/烧录配置（含全部关键坑注释）
PROGRESS.md             ← 进展快照 + 每次会话摘要（本项目"日志"）
HANDOFF.md              ← 本文件（交接清单）
```

**配套脚本（根目录）**：
- `write_config.py` — 自动检测当前 WiFi → 拼 config.ini → 串口写设备 SD（逐条 ACK）
- `verify_audit.py` — 回归测试（`py -3 verify_audit.py`）
- `send_cmd.py` / `read_serial.py` / `upload_file.py` — 串口工具
- `parse_cards.py` / `crop_qr.py` — 卡片解析 / 二维码裁边
- `interact_sleep.py` / `listen_wake.py` — 待机/唤醒回归工具

---

## 6. 当前状态（2026-08-26）

| 项 | 状态 |
|----|------|
| 固件 | ✅ 编译通过，RAM 37.8% / Flash 42.7%（fw=Aug 26 2026 15:03:27，RTC 修复版已烧录 COM4） |
| RTC 时间同步 | ✅ 修复：SNTP 在 iPhone 热点下不可靠 → 新增 HTTP Date 兜底校准（`fetchHttpTimeSync`），实测 RTC=8/26 |
| 待机省电 | ✅ light sleep（WiFi off + CPU 停转 + 跳过 SHT40），~2mA |
| 三键唤醒 | ✅ RTC 上拉 + wakeFromStandby 重配按键，A/B/C 全部实测通过 |
| 待机页 v9 | ✅ 纯白底 + 年红/日期深蓝 + 星期红块 + 黄历(完整) + 天气，不刷新 |
| 语音命令 | ✅ 全局长按C + 「小彩」前缀防误判 |
| 联网链路 | ✅ WiFi→智谱额度→RSS→天气 全通（iPhone 热点实测，cfg_ok=1） |
| 诊断 | ✅ 新增 `#TIME` 命令（对比 sys/rtc）；`verify_audit.py` 增强（write_timeout 防卡死） |
| GitHub | ✅ DrDavidDa/eInk-Desk-Terminal（公开）+ Release + CI 构建 success |

---

## 7. 待办 / 未决（接手下一步）

- [ ] **实机照片**：README 顶部暂用官方图，待拍实机照替换 ASCII 演示区
- [ ] 死代码未删：`markTodoDone()`、相册、COMPARISON_DB、宜忌/天气伪随机（**用户选择不动**）
- [ ] `#WX` 唤醒后立即执行可能失败（WiFi 重连延迟）→ 稍等重发即成功，可后续优化
- [ ] CHANGELOG Unreleased：本地唤醒词（已砍，违背省电）/ 天气多城市 / Web 端卡片管理（均为可选方向，未定）
- [ ] 待机喊话唤醒 = **已否决**（用户 2026-08-13 决定，esp-sr 违背省电）

---

## 8. 关键技术备忘（踩过的坑，必读）

### 硬件/驱动
- **白屏根因**：PMIC 0x09(idle sleep) 被改写 → 用 Arduino Wire 写 PMIC `0x09=0x00` + `0x11=0x09` 恢复
- **绝不在 `M5.begin()` 前 `Wire.begin()`**；`initSDCard` 绝不调 `SPI.end()/setPins()`（破坏 EPD 共享 SPI2）
- EPD 全屏刷新慢是正常（fillScreen ~16.8s）；Spectra6 红色料偏暗（像棕色），大面积红用 `color565(255,140,0)` 偏亮橙黄
- 墨水屏 4 种刷新：`epd_fastest`/`epd_fast`(黑白 Bayer)/`epd_text`(RGB 彩色快)/`epd_quality`；**彩色页面必须 `epd_text`**
- 刷新管控：`setAutoDisplay(false)` + `renderScreen(force)` 唯一刷屏入口；全彩 30min 冷却自动降级 fast；**开始录音绝不 renderScreen**（阻塞丢采样）

### 按键
- 用 `M5.BtnX.isPressed()`（自研状态机），**勿裸 digitalRead**（M5Unified 未配置该 GPIO 会失效；light sleep 唤醒后需 `wakeFromStandby()` 重配 INPUT_PULLUP + 同步 prevLevel）
- `BTN_HOLD_MS=1300`；每键独立防抖（共用一个防抖会吞按键）

### 语音/音频
- 录音前 `M5.Speaker.end()`（Speaker/Mic 共用电源时钟），录完 `M5.Mic.end()`
- 乒乓缓冲（`M5.Mic.record()` 是异步双缓冲）；**人声在 L 声道(MIC1)**，取 `recRead[i*2]`
- 分块 record 依赖 loop 频率 → 用 `recordingTask`/主循环 processRecording + 录音中禁 WiFi 轮询
- ASR：SiliconFlow 走 **HTTP/1.1 + Connection: close**（1.0 会挂起）；**HTTP 明文(80)**（HTTPS TLS 慢/卡）；录音保存时读入 PSRAM，转写任务不碰 SD/EPD

### 联网
- 天气 open-meteo 用 **`HTTP/1.0`**（1.1 返回 chunked 会被当 JSON 解析失败）
- IT之家 RSS 大：BUF 80KB(PSRAM)；stripHtml 用 inTag 状态机处理转义标签；`toShortLead` 截到句号
- iPhone 热点：隐藏 SSID + 需开「最大兼容性」(WPA2)；万达企业 WiFi(WPA2-Enterprise) 连不上
- config.ini 多行文本含 `\n` → 必须逐条 `#CFGLINE`/`#CFGTOK` 分块 + ACK 流控（USB CDC 493B 溢出坑）；`CP_SERIAL_BUF_LEN=1024`

### 显示
- **efont 字体不能非整数 setTextSize 缩放**（渲染错乱）——整数缩放(如3)OK；无 18/20 号粗体，用 16/24 代替
- 中文 UTF-8 3 字节：**绝不用指针+1 截中文**（乱码「口」），用独立数组/`index()`
- 待机页时间用 **Font8**(75px 实心 RLEfont)——Font7 7 段数码管细笔画彩色抖动缺笔画
- 避免大面积浅灰/灰色料背景（颗粒感）；分隔用 1px 细线
- 时间同步：`initRtcFromBuildTime()` 无条件用 `__DATE__/__TIME__` 覆盖 RTC，再试 NTP

### 其他
- 诊断代码勿残留（"开机按键变化就刷新"会拦截刷新但不改索引 → 用户看到刷新了但页面不变）
- `py` 启动器（`python` 是 WindowsApps stub 返回 9009）
- 误刷恢复：`pio run -t upload` 全量重刷 bootloader+partition+ota_data+app；voice_data 分区 @0xC10000 单独 `esptool read-flash` 检查

---

## 9. 验证 / 回归

- **回归**：`py -3 verify_audit.py`（#STATUS/#NEWS/#WX/#CFGREAD/#PLAY/#PSTATE；已内置 DTR/RTS 复位 + #STATUS 心跳防待机打断）
- **诊断命令**：#STATUS / #NEWS / #WX / #POLL / #CFGREAD / #CFGTOK / #CFGDONE / #PLAY / #PSTATE / #TODODUMP / #WAVDIAG / #CLEARREC / #REMLIST / #REMCLEAR / #ASRSIM / #STANDBY / #LED / #KEY / #DNS / #RMFILE
- **开机自检链路**：WiFi→智谱额度→RSS10条→天气5天，全链路实测通过
- **待机回归**：`interact_sleep.py`（复位→#STANDBY→[SLEEP]）+ `listen_wake.py`（dtr=False 等按键唤醒）
- 启动完成检测用 `[NEWS]` RSS 解析日志（[WX] 天气日志非必出现）

---

## 10. GitHub 发布

- 仓库：`https://github.com/DrDavidDa/eInk-Desk-Terminal`（PUBLIC，main 分支）
- 账号 DrDavidDa（gh CLI 已登录）；CI 自动构建（pip+pio run → 上传 firmware.bin artifact）
- ⚠️ **git 全局代理 127.0.0.1:7897 是坏的**（TLS 卡死）→ push 必须临时直连：
  ```bash
  git -c http.proxy= -c https.proxy= add -A
  git -c http.proxy= -c https.proxy= commit -m "..."
  git -c http.proxy= -c https.proxy= push origin main
  ```
- .gitignore 排除密钥/诊断脚本/PROGRESS.md；README 中文爆款风 + 官方图 + 购买/文档链接

---

## 11. 常用命令速查

```bash
# 编译 + 上传（先 read_mac 核对设备）
py -m esptool --port COM4 read_mac
pio run -j 1 -t upload --upload-port COM4

# 串口诊断（pySerial 打开会复位设备，先等就绪）
py -3 send_cmd.py COM4 "#STATUS"

# 写 WiFi/config（自动检测当前网）
py -3 write_config.py

# 回归测试
py -3 verify_audit.py
```

> 🔁 **恢复本项目**：说「继续小彩」→ 读 repo 记忆 `papercolor-epd.md` + 本清单 + `PROGRESS.md` → 从第 7 节待办继续。
