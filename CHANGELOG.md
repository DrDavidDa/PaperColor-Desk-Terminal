# Changelog

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/) 规范。

## [Unreleased]

### 修复
- **RTC 日期不同步**：SNTP(UDP123) 在 iPhone 热点下不可靠 → 日期停在旧值。新增 HTTP Date 兜底校准（`fetchHttpTimeSync` 拉 www.baidu.com 响应头 → settimeofday + 写回 RTC 芯片），开机/待机唤醒 SNTP 后兜底执行，实测 RTC=真实日期
- 新增 `#TIME` 诊断命令：对比系统 `time()` 与 RTC 芯片当前值
- `verify_audit.py` 增强：`ser.write` 加 `write_timeout` 防 USB 卡死无限阻塞 + 异常捕获与明确提示（light sleep 后需物理重插 USB）

### 待评估
- 天气多城市配置
- Web 端卡片管理

## [v3.0] - 2026-08-11

### 新增
- 全局语音助手「小彩」：任意页长按 C 唤起，说指令一次刷屏直达
- 语音命令「小彩」前缀：正常录音含关键词不再误删
- 待机唤醒 WiFi 自动重连（`wakeFromStandby` + `reconnectWifiFromConfig`）
- 考试天数精确计算（julianDay）
- 转写阶段橙灯反馈

### 优化
- 唤醒后静默刷新天气
- `#WX` 命令先重连 WiFi 再执行
- 恢复生产待机 3 分钟

### 修复
- 待机唤醒 WiFi 未重连
- 月历 2 月/30 天月错误显示 29/30/31
- 待机时钟静止
- `#PLAY` 串口命令 break 未清缓冲
- `#CFG` 诊断打印越界读

## [v2.0] - 2026-08-05

- 7 页面完整功能（日历黄历/资讯早报/考点闪卡/CodingPlan/语音待办/仪表盘/二维码）
- 离线中文 TTS（esp-sr voice_data 分区）
- Anki 间隔记忆算法 + NVS 掉电存储
- 精确农历/干支/黄历（1900–2100）
- 墨水屏刷新管控（30 分钟全彩冷却）
- 待机页纯白极简 v9（不刷新省电）

## [v1.0] - 2026-07-28

- 首个可用固件：硬件驱动 + 基础 3 页
- SD 卡强力挂载引擎（自动穷举 SPI/CS 引脚）
- 图像双引擎解码（PNG/JPG/BMP + 中文文件名）
