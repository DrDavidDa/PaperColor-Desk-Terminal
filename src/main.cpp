/**
 * ====================================================================
 * M5Stack PaperColor 桌面学习终端 (PaperColor_Study v3.0)
 * 主程序: src/main.cpp
 * 
 * 硬件支持: ESP32-S3R8 + 4" 600x400 Spectra 6 彩色墨水屏 + SHT40 + SD 卡
 * 驱动架构: M5Unified + M5GFX + SD/FS + Preferences(NVS)
 * 
 * 核心提升:
 *   1. 硬件级 SD 卡强力挂载引擎 (自动穷举 SPI SCK=14,MISO=13,MOSI=12 与 CS=4,13,41,5,10 引脚)
 *   2. 图像双引擎解码 (同时原生支持 .png / .jpg / .jpeg / .bmp 格式与中文文件名)
 *   3. 全界面大字号排版引擎 (全局提升至 16px/20px/24px 大字体，阅读超清晰)
 *   4. Spectra 6 炫彩视觉系统 (调彩红、湛蓝、翠绿、金黄、藏青五色绚丽熏章与彩色标题)
 *   5. 墨水屏低频番茄专注钟 (以“分钟”为单位递减刷新，绝无低频闪烁打扰)
 *   6. Anki 间隔记忆算法 + NVS 掉电永久存储
 * ====================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <SD.h>
#include <FS.h>
#include <SPI.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <M5GFX.h>
#include <Adafruit_NeoPixel.h>
// HWCDC：检测 USB-Serial/JTAG 是否插入（light sleep 会卡死 USB 数据通路，据此跳过 light sleep）
#include "HWCDC.h"

// 中文语音合成（esp-sr v1.9.5 预编译库，模型从 flash voice_data 分区读入 PSRAM）
#include <esp_tts.h>
#include <esp_tts_voice_template.h>
#include <esp_partition.h>

// 精确农历/干支/黄历宜忌（离线数据表 1900-2100，见 lunar_cal.h）
#include "lunar_cal.h"

// RGB LED（PaperColor: GPIO21, 2颗 WS2812 GRB）。M5Unified 的 RMT LED 驱动在此平台失效(begin=0)，改用 Adafruit
#define RGBLED_PIN 21
#define RGBLED_COUNT 2
Adafruit_NeoPixel rgbStrip(RGBLED_COUNT, RGBLED_PIN, NEO_GRB + NEO_KHZ800);
void ledSetAll(int r, int g, int b) {
    for (int i = 0; i < RGBLED_COUNT; i++) rgbStrip.setPixelColor(i, rgbStrip.Color(r, g, b));
    rgbStrip.show();
}
void ledSetBrightness(uint8_t br) { rgbStrip.setBrightness(br); }

// ====================================================================
// 一、集中参数配置区域
// ====================================================================

#define WIFI_SSID     "Your_WiFi_SSID"
#define WIFI_PASS     "Your_WiFi_Password"

// ===== 自研按键检测（M5PaperColor 物理按键 GPIO） =====
// 重要：M5PaperColor 的按钮实际接在 GPIO1/9/10（按下=低电平）！
//   BtnA = GPIO10, BtnB = GPIO9, BtnC = GPIO1
// （GPIO2/3/5 是 I2C 引脚，不是按钮！）
// 墨水屏全屏刷新(2~3秒)会阻塞 loop，故直接读 GPIO 电平自研防抖+长短按，彻底解决。
#define BTN_A_GPIO    GPIO_NUM_10    // BtnA
#define BTN_B_GPIO    GPIO_NUM_9     // BtnB
#define BTN_C_GPIO    GPIO_NUM_1     // BtnC
#define BTN_HOLD_MS   1300           // 长按判定阈值 (ms)（原1800偏长，用户长按~1.5s会误判为短按）

#define SCREEN_WIDTH         600
#define SCREEN_HEIGHT        400

#define STATUS_BAR_HEIGHT    42   // Y: 0 ~ 42
#define MAIN_AREA_HEIGHT     296  // Y: 42 ~ 338
#define ACTION_BAR_HEIGHT    62   // Y: 338 ~ 400
#define PAGE_COUNT           7    // 页面总数（0考点1早报2待办3日历4仪表盘5CodingPlan6二维码）

#define MARGIN_X             20   // 主体左右边距

// 刷新与定时器参数
#define FULL_REFRESH_COOLDOWN_MS  (30UL * 60UL * 1000UL)  // 强刷冷却: 30分钟（不足则自动降级黑白快刷）
#define BUTTON_DEBOUNCE_MS        (200UL)                // 按键防抖: 200ms（避免短按被吞）
#define SENSOR_READ_INTERVAL_MS   (30000UL)              // 温湿度读取间隔: 30秒

// 番茄钟参数 (以分钟为单位)
#define POMODORO_FOCUS_MIN        25
#define POMODORO_REST_MIN         5

// 2026年中级经济师考试预估日期: 2026-11-07
#define EXAM_YEAR  2026
#define EXAM_MONTH 11
#define EXAM_DAY   7

// ====================================================================
// 二、数据结构定义
// ====================================================================

struct KnowledgePoint {
    const char* const category;   // 科目分类
    const char* const question;   // 问题（左侧）
    const char* const answer;     // 答案要点（右侧）
    const char* const mnemonic;   // 速记口诀（右侧，彩色标注）
};

// 运行时考点（从 SD /cards/*.json 加载，替换内置 KNOWLEDGE_DB）
struct RuntimeCard {
    char category[24];
    char question[280];
    char answer[280];
    char mnemonic[200];
};
#define KP_MAX_CARDS 200
RuntimeCard* kpCards = nullptr;      // 运行时卡片存储（PSRAM 动态分配，减小内部 RAM）
size_t kpCount = 0;                  // 实际加载卡片数（0=使用内置）
void loadCardsFromSD();              // 前向声明
void stopVoicePlayback();            // 前向声明（playCurrentTodoVoice 调用）

struct NewsItem {
    const char* const tag;        // 标签
    const char* const title;      // 标题
    const char* const summary;    // 摘要
};

struct CardMemoryState {
    uint32_t lastReviewedEpoch;   
    uint32_t cooldownUntilEpoch;   
    uint8_t status;                // 0: 正常, 1: 已掌握(3天冷却), 2: 需重温(加急)
};

// ====================================================================
// 三、内置考点与资讯数据库 (Flash 存储)
// ====================================================================

const KnowledgePoint KNOWLEDGE_DB[] = {
    {
        "人力资源",
        "Q1: 内源性动机 vs 外源性动机，区别是什么？",
        "内源=行为本身有价值(挑战/成就/潜力)；外源=行为结果有报偿(工资/奖金/地位)。",
        "速记:「源」字泉眼 → 内源=心泉自涌，外源=外管接水。工资落外源不落内源！"
    },
    {
        "人力资源",
        "Q2: 赫茨伯格双因素理论中，工资属于哪类？",
        "保健因素(管不满轴)。消除不满≠产生满意，满意另靠激励因素(认可/晋升/责任)。",
        "速记: 工资落【保健轴】不落【激励轴】！双因素=2根独立轴，非一根轴两端。"
    },
    {
        "人力资源",
        "Q3: 马斯洛需要层次理论五层是什么？",
        "生理→安全→归属爱→尊重→自我实现。底3层=基本需要(外在)，顶2层=高级需要(内在)。",
        "速记: 金字塔5层「生安归尊自」。爬塔三律: 低层先满足/已满足无激励/尊重踏两界。"
    },
    {
        "人力资源",
        "Q4: 亚当斯公平理论中，员工比的是什么？",
        "比投入/产出的【比值】而非绝对量，且这个秤是员工自己主观校准的。",
        "速记: 天平模型 → 左盘投入右盘产出，比的是比值！报酬过高也会不适。"
    },
    {
        "人力资源",
        "Q5: 斯金纳强化理论中，负强化与惩罚有何区别？",
        "负强化=撤销厌恶刺激以增加好行为；惩罚=给予厌恶刺激以减少坏行为。",
        "速记: 负强化【撤恶增好】，惩罚【给恶抑坏】！二者方向相反，勿混淆。"
    }
};
const size_t KNOWLEDGE_COUNT = sizeof(KNOWLEDGE_DB) / sizeof(KNOWLEDGE_DB[0]);

// 统一访问当前有效考点集：SD 卡加载优先，否则用内置
inline size_t cardTotal() { return kpCount > 0 ? kpCount : KNOWLEDGE_COUNT; }
inline const char* cardCategory(size_t i) {
    return kpCount > 0 ? kpCards[i].category : KNOWLEDGE_DB[i].category;
}
inline const char* cardQuestion(size_t i) {
    return kpCount > 0 ? kpCards[i].question : KNOWLEDGE_DB[i].question;
}
inline const char* cardAnswer(size_t i) {
    return kpCount > 0 ? kpCards[i].answer : KNOWLEDGE_DB[i].answer;
}
inline const char* cardMnemonic(size_t i) {
    return kpCount > 0 ? kpCards[i].mnemonic : KNOWLEDGE_DB[i].mnemonic;
}

const NewsItem NEWS_DB[] = {
    {
        "AI突破",
        "DeepSeek 与 Claude 3.7 推动大模型推理能力飞跃",
        "前沿 AI 模型在复杂逻辑推理、代码生成及多模态决策上取得重大突破，深度赋能智能终端与自动化 Agent。"
    },
    {
        "端侧AI",
        "嵌入式端侧 AI 芯片爆发 墨水屏终端迎来智能化升级",
        "低功耗 ESP32-S3 等端侧芯片结合本地化记忆算法与极简图形引擎，为常驻桌面终端带来高效护眼学习新体验。"
    },
    {
        "宏观",
        "央行开展公开市场逆回购操作 保持流动性合理充裕",
        "人民银行今日开展公开市场逆回购操作，中标利率平稳，维护银行体系流动性合理充裕，支持实体经济。"
    },
    {
        "政策",
        "人社部优化职业技能补贴政策 鼓励产教深度融合",
        "人社部发布通知，优化技能提升补贴流程，鼓励企业开展弹性工作制与高技能人才产教融合培训。"
    }
};
const size_t NEWS_COUNT = sizeof(NEWS_DB) / sizeof(NEWS_DB[0]);

// ===== 联网早报（IT之家 RSS 拉取，优先于内置 NEWS_DB） =====
struct RuntimeNews { char tag[16]; char title[120]; char summary[512]; };   // 512: 正文分页阅读需更长摘要
RuntimeNews newsItems[10];          // 联网早报存储（最多10条）
int newsCount = 0;                  // 0=使用内置 NEWS_DB
bool newsLoading = false;           // 拉取进行中标志
uint32_t newsUpdatedAt = 0;         // 最近拉取时刻（开机限频）
int newsLastDay = -1, newsLastMonth = -1;   // 最近定时拉取日期（避免重复）

int newsTotal() { return newsCount > 0 ? newsCount : NEWS_COUNT; }
const char* newsTag(size_t i) { return newsCount > 0 ? newsItems[i].tag : NEWS_DB[i].tag; }
const char* newsTitle(size_t i) { return newsCount > 0 ? newsItems[i].title : NEWS_DB[i].title; }
const char* newsSummary(size_t i) { return newsCount > 0 ? newsItems[i].summary : NEWS_DB[i].summary; }

// 剥 HTML 标签 + 解码常见实体（RSS description 含 HTML，且标签可能被转义成 &lt;p&gt; 形式）
// 关键: &lt; → 进入“标签内”状态（不写入），&gt; → 退出；原始 < > 同理
void stripHtml(const char* src, int srcLen, char* dst, int maxLen) {
    int d = 0;
    bool inTag = false;   // 标签内（含 &lt; 解码出的标签，内容整体跳过）
    for (int i = 0; i < srcLen && d < maxLen - 1; i++) {
        char c = src[i];
        if (c == '&') {
            if (strncmp(src + i, "&lt;", 4) == 0)  { inTag = true;  i += 3; continue; }   // 转义 < → 进入标签
            if (strncmp(src + i, "&gt;", 4) == 0)  { inTag = false; i += 3; continue; }   // 转义 > → 离开标签
            if (strncmp(src + i, "&amp;", 5) == 0) { if (!inTag) dst[d++] = '&';  i += 4; continue; }
            if (strncmp(src + i, "&quot;", 6) == 0){ if (!inTag) dst[d++] = '"';  i += 5; continue; }
            if (strncmp(src + i, "&nbsp;", 6) == 0){ if (!inTag) dst[d++] = ' ';  i += 5; continue; }
            if (strncmp(src + i, "&#39;", 5) == 0) { if (!inTag) dst[d++] = '\''; i += 4; continue; }
            if (!inTag) dst[d++] = '&';              // 未识别实体
            continue;
        }
        if (c == '<') { inTag = true;  continue; }  // 原始标签开始
        if (c == '>') { inTag = false; continue; }  // 原始标签结束
        if (!inTag) dst[d++] = c;                   // 仅标签外文本写入
    }
    dst[d] = '\0';
}

// 截取首句快讯：优先在句末标点（。！？或 ASCII .!?）处断，保证句子完整带句号
// 若 maxChars 内无句末标点，继续扫描到句末标点（绝对上限 150 字符，防超长句无限扫）
// s 为 UTF-8，原地截断
void toShortLead(char* s, int maxChars) {
    const int MAX_SCAN_CHARS = 150;   // 无句号时最多延长扫描到 150 字符
    int len = strlen(s);
    int chars = 0, idx = 0;
    int lastEnd = -1;                 // 最近句末标点后的字节偏移
    while (idx < len && chars < MAX_SCAN_CHARS) {
        uint8_t c = (uint8_t)s[idx];
        int cl = 1;
        if (c >= 0xF0) cl = 4; else if (c >= 0xE0) cl = 3; else if (c >= 0xC0) cl = 2;
        if (idx + cl > len) break;
        bool isEnd = false;
        if (cl == 3 && (uint8_t)s[idx] == 0xE3 && (uint8_t)s[idx+1] == 0x80 && (uint8_t)s[idx+2] == 0x82) isEnd = true;                  // 。
        else if (cl == 3 && (uint8_t)s[idx] == 0xEF && (uint8_t)s[idx+1] == 0xBC && ((uint8_t)s[idx+2] == 0x81 || (uint8_t)s[idx+2] == 0x9F)) isEnd = true;  // ！？
        else if (cl == 1 && (s[idx] == '.' || s[idx] == '!' || s[idx] == '?')) isEnd = true;
        if (isEnd) lastEnd = idx + cl;
        chars++; idx += cl;
        if (chars >= maxChars && lastEnd >= 0) {
            s[lastEnd] = '\0';        // 达到上限且已有句号 → 截到句号（句子完整）
            return;
        }
    }
    if (lastEnd >= 0) s[lastEnd] = '\0';   // 有句号 → 截到句号
    else s[idx] = '\0';                    // 极端：150字内无句号 → 截断
}

// 早报分类过滤：非科技类（汽车/交通/娱乐等）标题命中任一排除词即跳过
const char* const NEWS_EXCLUDE[] = {
    "汽车","轿车","SUV","MPV","新能源车","电动车","插混","纯电","车型","新车","预售","试驾","车展",
    "比亚迪","特斯拉","蔚来","小鹏","理想","极氪","问界","智界","阿维塔","坦克","哈弗","捷途","奇瑞","吉利","长城","长安",
    "别克","奔驰","宝马","奥迪","丰田","本田","日产","大众","福特","领克","荣威","五菱","红旗","广汽","上汽","小米汽车",
    "高铁","动车","列车","铁路","地铁","航班","客机",
    "电影","剧集","综艺","游戏","体育","娱乐","明星","音乐","动画","漫画","电竞","演唱会","票房","NBA"
};
bool isExcludedNews(const char* title) {
    for (size_t i = 0; i < sizeof(NEWS_EXCLUDE) / sizeof(NEWS_EXCLUDE[0]); i++) {
        if (title && strstr(title, NEWS_EXCLUDE[i])) return true;
    }
    return false;
}

// 前向声明（parseRss/fetchDailyNews 在全局变量声明之前使用）
extern size_t newsIndex;
extern char cpWifiSsid[3][40];
extern char cpWifiPass[3][40];
extern bool wifiConnected;

// 解析 RSS body 中前 10 条 <item> 的 title/description
void parseRss(const char* body, int bodyLen) {
    newsCount = 0;
    newsIndex = 0;   // 修复M3: 重拉后回到第一条
    const char* bodyEnd = body + bodyLen;
    const char* pos = body;
    while (newsCount < 10 && pos && pos < bodyEnd) {
        const char* item = strstr(pos, "<item>");
        if (!item || item >= bodyEnd) break;
        const char* itemEnd = strstr(item, "</item>");
        if (!itemEnd || itemEnd > bodyEnd) break;
        char title[130] = {0}, desc[512] = {0};
        const char* to = strstr(item, "<title>");
        if (to && to < itemEnd) {
            const char* tc = strstr(to + 7, "</title>");
            if (tc && tc <= itemEnd) stripHtml(to + 7, tc - to - 7, title, sizeof(title));
        }
        const char* do_ = strstr(item, "<description>");
        if (do_ && do_ < itemEnd) {
            const char* dc = strstr(do_ + 13, "</description>");
            if (dc && dc <= itemEnd) {
                stripHtml(do_ + 13, dc - do_ - 13, desc, sizeof(desc));
                toShortLead(desc, 100);  // 只取首句作快讯（16号正文4行~136字容量，100字完整显示）
            }
        }
        // 过滤空白标题项 + 排除非科技类（汽车/交通/娱乐）
        char* tp = title; while (*tp == ' ' || *tp == '\t' || *tp == '\n' || *tp == '\r') tp++;
        if (tp[0] && !isExcludedNews(tp)) {
            strncpy(newsItems[newsCount].tag, "IT之家", sizeof(newsItems[newsCount].tag) - 1);
            newsItems[newsCount].tag[sizeof(newsItems[newsCount].tag) - 1] = '\0';
            strncpy(newsItems[newsCount].title, tp, sizeof(newsItems[newsCount].title) - 1);
            newsItems[newsCount].title[sizeof(newsItems[newsCount].title) - 1] = '\0';   // 修复M2: 截断后补NUL
            char* sp = desc; while (*sp == ' ' || *sp == '\t' || *sp == '\n' || *sp == '\r') sp++;
            strncpy(newsItems[newsCount].summary, sp, sizeof(newsItems[newsCount].summary) - 1);
            newsItems[newsCount].summary[sizeof(newsItems[newsCount].summary) - 1] = '\0';
            newsCount++;
        }
        pos = itemEnd + 8;
    }
    Serial.printf("[NEWS] RSS 解析 %d 条\n", newsCount);
}

// 拉取 IT之家 RSS（免费稳定中文科技源），解析填充 newsItems[]
void fetchDailyNews() {
    if (newsLoading) return;
    newsLoading = true;
    // 修复M9: 若 WiFi 未连接，尝试用 config 的 WiFi 连接（兼容 use_wifi=false 时 RSS 也能联网）
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_STA);
        for (int i = 0; i < 3; i++) {
            if (strlen(cpWifiSsid[i]) == 0) continue;
            WiFi.begin(cpWifiSsid[i], cpWifiPass[i]);
            unsigned long st = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - st < 8000UL) delay(200);
            if (WiFi.status() == WL_CONNECTED) { wifiConnected = true; break; }
        }
    }
    Serial.println("[NEWS] 拉取 IT之家 RSS...");
    // 注意：WiFi.hostByName 在此 Arduino core 不可靠（实测全部域名 FAIL），
    // 但 client.connect(域名) 内部 lwip 解析正常（智谱轮询正是如此工作）
    WiFiClientSecure client;
    client.setInsecure();   // 跳过证书校验（新闻源可接受）
    client.setTimeout(8);
    if (!client.connect("www.ithome.com", 443)) {
        char errBuf[64];
        client.lastError(errBuf, sizeof(errBuf));
        Serial.printf("[NEWS] 连接失败 err=%s\n", errBuf);
        newsLoading = false;
        return;
    }
    Serial.println("[NEWS] 连接成功");
    client.println("GET /rss/ HTTP/1.1");
    client.println("Host: www.ithome.com");
    client.println("User-Agent: Mozilla/5.0");
    client.println("Accept-Encoding: identity");   // 避免 gzip
    client.println("Connection: close");
    client.println();

    const int BUF = 80 * 1024;      // IT之家 RSS 单条 description 可达 37KB，28KB 只够1条；80KB 覆盖前10条
    static char* buf = nullptr;
    if (!buf) buf = (char*)heap_caps_malloc(BUF, MALLOC_CAP_SPIRAM);
    if (!buf) { newsLoading = false; return; }

    int pos = 0;
    unsigned long t0 = millis();
    while (client.connected() && millis() - t0 < 20000 && pos < BUF - 1) {
        while (client.available() && pos < BUF - 1) {
            char c = client.read();
            if (c >= 0) buf[pos++] = c;
        }
        delay(1);
    }
    buf[pos] = '\0';
    client.stop();
    Serial.printf("[NEWS] 收到 %d 字节\n", pos);

    char* body = strstr(buf, "\r\n\r\n");   // 定位 HTTP body
    if (body) {
        body += 4;
        int bodyLen = (buf + pos) - body;
        if (bodyLen > 0) parseRss(body, bodyLen);
    }
    newsLoading = false;
}

// ====================================================================
// 四、全局状态变量与 NVS 持久化对象
// ====================================================================

Preferences prefs;

uint8_t currentPage = 0;        // 0: 日历黄历, 1: 早报, 2: 考点, 3: CodingPlan, 4: 待办, 5: 仪表盘, 6: 二维码
size_t ebbinghausIndex = 0;     
size_t newsIndex = 0;           

// ===== 语音速记待办 (页面 2) =====
#define TODO_MAX 30
struct TodoItem {
    char text[96];        // 待办文本
    char audioFile[64];   // 关联语音文件路径（空=纯文本）
    char recTime[24];     // 录音日期时间 (MM-DD HH:MM)
    uint16_t recSec;      // 录音时长（秒）
    char asr[512];        // 语音转写文字（SiliconFlow SenseVoice，空=未转写）
    bool done;            // 已完成
    bool highPriority;    // 高优先级
};

// ===== 定时提醒（F）：RTC 到点 TTS 播报 + 屏幕提示（SD /reminders.txt 持久化） =====
#define REMINDER_MAX 8
struct Reminder {
    char time[6];         // "HH:MM"
    char text[64];        // 提醒内容
    bool fired;           // 今天是否已触发
};
Reminder reminders[REMINDER_MAX];
int reminderCount = 0;
int lastRemindDay = 0;    // 跨天重置 fired
TodoItem todoItems[TODO_MAX];
size_t todoCount = 0;
size_t todoIndex = 0;

// ===== 语音录音（MEMS 麦克风 → WAV 流式写 SD 卡） =====
// 交互：待办页短按C 开始录音，再短按C 停止并保存（可录任意时长，时长仅受 SD 容量限制）
// 流式方案：主循环每块采 250ms，立即只取 R 声道写入 SD 文件，不占大内存缓冲，
// 彻底避免"内存缓冲上限/主循环阻塞导致录音中断"问题。
#define REC_SAMPLE_RATE 16000       // 采样率
#define REC_CHUNK_SAMPLES 4000      // 每次采样的块大小（250ms）
// 双缓冲乒乓（M5Unified 官方模式）：M5.Mic.record() 异步——提交 A 时读已填好的 B，
// 交替使用避免"读到自己刚提交未填好的缓冲"导致杂音/错乱。
int16_t recBufA[REC_CHUNK_SAMPLES * 2];
int16_t recBufB[REC_CHUNK_SAMPLES * 2];
int16_t* recSubmit = recBufA;       // 当前要提交的缓冲
int16_t* recRead   = recBufB;       // 上一块（已填好）缓冲
bool recFirstBlock = true;          // 第一块无上一块可读
bool isRecordingNow = false;        // 录音会话进行中（开始→未停止）
uint32_t recWritePos = 0;           // 已录字节数（写入 SD 的字节量，用于时长统计）
bool recMicReady = false;           // 麦克风是否已初始化
File recWavFile;                    // 录音 WAV 文件句柄（流式写入）

// ===== 全局语音命令模式（任意页长按C唤起，最小路径：不刷屏+蓝灯+内存录音3.5s）=====
bool voiceCmdMode = false;          // 语音命令模式进行中
uint8_t* vcAudioBuf = nullptr;      // 语音命令录音缓冲（PSRAM）
size_t vcAudioLen = 0;              // 已录字节
size_t vcAudioMax = 0;              // 缓冲容量
unsigned long vcCmdStart = 0;       // 开始时刻（自动停止）
#define VC_CMD_MS 3500UL            // 自动录音 3.5 秒（短句指令足够）
#define VC_CMD_MAX_BYTES (16000 * 4 * 2)   // 4秒单声道字节上限

// 非阻塞回放状态（loop 轮询 isPlaying，任意键停止）
bool voicePlaying = false;          // 语音回放进行中
uint8_t* voicePlayBuf = nullptr;    // 回放 WAV 缓冲（PSRAM）
unsigned long voicePlayStart = 0;   // 回放开始时刻（超时保护）

// TTS 中文语音合成（esp-sr v1.9.5）—— 模型来自 flash voice_data 分区
// 所有 TTS 触发均为“附加”功能：ttsReady==false 时静默跳过，绝不影响现有功能
bool ttsReady = false;                // 引擎就绪标志
esp_tts_handle_t g_tts = nullptr;     // 引擎句柄
void* g_ttsModel = nullptr;           // 模型 PSRAM 缓冲
int16_t* g_ttsPcm = nullptr;          // 合成 PCM 输出缓冲（PSRAM）
#define TTS_PCM_MAX (1024 * 1024)     // 1MB ≈ 32 秒音频

// ===== 日历黄历天气 (页面 4) =====
int calOffset = 0;        // -1=昨天, 0=今天, 1=明天, 2=后天
char calLunar[48] = "农历";  // 农历文本（离线简化）
char calGanzhi[48] = "";     // 干支
char calYi[64] = "宜：祈福 出行";   // 宜
char calJi[64] = "忌：动土 安葬";   // 忌
char weatherText[64] = "晴";       // 今日天气（fetchWeather 联网拉取真实数据）
char weatherCity[16] = "北京";     // 天气城市（config weather_city 可覆盖）
float weatherLat = 39.9042f;       // 天气纬度（config weather_lat 可覆盖，默认北京）
float weatherLon = 116.4074f;      // 天气经度（config weather_lon 可覆盖，默认北京）
int weatherHigh = 25, weatherLow = 16;
char weatherTextTmr[64] = "多云";  // 明日天气
int weatherHighTmr = 26, weatherLowTmr = 18;   // 明日高低温
// 5天天气数组（联网填充）：0=昨天 1=今天 2=明天 3=后天 4=大后天
char wxTxt[5][16] = {"晴","晴","多云","多云","多云"};
int wxHi[5] = {25,25,26,26,26};
int wxLo[5] = {16,16,18,18,18};
unsigned long weatherUpdatedAt = 0; // 天气最近拉取时刻（开机限频）
bool weatherSuccess = false;        // 上次天气拉取是否成功（成功3h/失败5min重试）

// ===== 微信二维码 (页面 4) =====
char qrPaths[8][64];      // SD 卡二维码图片路径
size_t qrCount = 0;
size_t qrIndex = 0;

// ===== 智谱 Coding Plan 额度监控 (页面 6) =====
// 数据完全来自主机串口推送；串口接收只更新内存，绝不刷屏
struct CodingPlanData {
    int   quota_5h_percent = 0;       // 5小时额度使用率 %
    int   quota_7d_percent = 0;       // 每周额度使用率 %
    int   quota_mcp_percent = 0;      // MCP月度额度使用率 %
    char  quota_5h_reset_time[12] = "--:--";
    char  quota_7d_reset_time[12] = "--:--";
    char  quota_mcp_reset_time[12] = "--:--";
    long  daily_token_total = 0;      // 今日 Token 消耗总量
    long  daily_token_main = 0;       // 今日主模型 Token
    long  tool_search_count = 0;      // 联网搜索 MCP 调用次数（更直观的 plan 工作量）
    long  tool_webread_count = 0;     // 网页读取 MCP 调用次数
    char  plan_status[16] = "idle";   // idle/planning/coding/waiting_confirm
    char  update_time[32] = "--";     // 上次更新时间
    bool  connected = false;          // 串口连接状态
};
CodingPlanData cpData;

// Coding Plan 配置（SD config.ini 覆盖）
int cpPollIntervalSec = 300;      // 轮询间隔（默认300s，仅WiFi方案用）
int cpWarnThreshold = 70;         // 告警阈值 %
int cpAlertThreshold = 90;        // 超限阈值 %
bool cpAlertAcknowledged = false; // 用户已进 Coding Plan 页看过超限内容（确认后不再报警）
bool cpSdLogEnabled = true;       // 是否写入 SD 用量日志
char cpWifiSsid[3][40] = {{0},{0},{0}};   // 最多 3 组已知 WiFi（自动扫描连接）
char cpWifiPass[3][40] = {{0},{0},{0}};
char cpZhipuCookie[512] = "";     // 智谱 Bearer Token（config.ini，WiFi直连方案用）
char sfApiKey[128] = "";          // SiliconFlow API Key（语音待办转文字，config.ini sf_api_key）
bool cpUseWifi = false;           // 是否启用 WiFi 直连智谱方案

// ===== 语音转文字后台任务（录音保存读入内存，独立任务联网转写，不阻塞主循环/不碰SD/EPD总线） =====
bool asrPending = false;        // 有待转写的录音
bool asrRunning = false;        // 转写任务运行中（防重入）
int asrPendingIdx = -1;         // 待转写待办索引
uint8_t* asrAudioBuf = nullptr; // PSRAM 音频缓冲（录音停止时读入，避免与EPD共享SPI冲突）
size_t asrAudioLen = 0;         // 音频缓冲长度
char asrResult[512];            // 转写结果
bool asrResultReady = false;    // 结果就绪（主循环消费）
int asrResultIdx = -1;          // 结果对应待办索引
int asrPageIdx = 0;             // 语音待办详情当前文字页（0起）
int asrPages = 1;               // 语音待办详情总页数（drawTodoPage 渲染时更新）
bool todoDetailMode = false;    // 语音待办详情模式（长按A进入，短按C返回）
unsigned long cpLastPollAt = 0;   // 上次 WiFi 轮询时刻
bool cpPollingNow = false;        // 正在轮询（防止阻塞）

// 串口接收缓冲（1024：config.ini 含 ~428 字符智谱 JWT token，总量 ~574 字节，512 会截断！）
#define CP_SERIAL_BUF_LEN 1024
char cpSerialBuf[CP_SERIAL_BUF_LEN];
size_t cpSerialLen = 0;
char cpTokenBuf[512];   // 智谱 token 分块累积缓冲（USB CDC 长行会溢出，需分块）
size_t cpTokenLen = 0;
// 文件上传（USB 串口二进制，用于二维码图片等；分块200字节+ACK流控）
bool fileReceiving = false;
File uploadFile;
uint32_t fileExpected = 0;
uint32_t fileReceived = 0;
int fileChunkCount = 0;
uint8_t fileBuf[512];   // 文件接收缓冲（批量写SD，避免每字节write极慢）
int fileBufLen = 0;
unsigned long cpLastSerialAt = 0;   // 最近一次收到有效数据时刻
unsigned long cpLastLedAt = 0;      // LED 告警刷新时刻
unsigned long keyLedUntil = 0;      // 按键 LED 反馈截止时刻
int ledTestR = 0, ledTestG = 0, ledTestB = 0;  // #LED 测试灯颜色
unsigned long ledTestUntil = 0;     // #LED 测试灯截止
unsigned long keyALast = 0, keyBLast = 0, keyCLast = 0;   // 每键独立防抖（切页按C不会吞掉后续B）

unsigned long lastFullRefreshTime = 0;
unsigned long lastSensorReadTime = 0;

// ===== 自研按键状态（直接读 GPIO，不受刷新阻塞影响） =====
struct KeyState {
    bool level;          // 当前按下电平（true=按下）
    bool prevLevel;      // 上次电平
    unsigned long pressMs;  // 按下起始时刻
    bool clickPending;   // 本次循环产生一次点击
    bool holdPending;    // 本次循环产生一次长按
    bool holdFired;      // 长按是否已触发
    bool suppressUntilRelease;  // 待机唤醒键抑制：松手前不产生点击/长按（防误触）
};
KeyState keyA, keyB, keyC;

bool isFirstBoot = true;               
bool cooldownBlocked = false;          

float currentTemp = 24.5f;             
float currentHum = 52.0f;              
bool hasSHT40 = false;                 
bool hasSDCard = false;                
bool wifiConnected = false;
bool timeSynced = false;
bool powerMenuShown = false;     // END 页长按C 弹出关机/重启菜单
bool suppressWakeClick = false;  // light sleep 唤醒后抑制唤醒键的首次点击/长按（防止唤醒动作被误判为切页等）

// ===== RTC 芯片时间同步（数据正确性关键）=====
// 待机页/日历页/黄历/定时提醒全部读 M5.Rtc（RTC 芯片），而 WiFi 连接只 configTime 启动 SNTP 同步系统 time，
// 不写回 RTC 芯片 → 日期永远停在编译时间（曾导致 8/21 显示成 8/12）。此函数把同步后的系统时间回写 RTC 芯片。
bool syncRtcFromNTP() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 3000)) return false;   // 等待 SNTP 响应（最多 3s）
    if (timeinfo.tm_year + 1900 < 2024) return false;   // SNTP 未生效（系统时间仍为 1970/旧值），不覆盖 RTC
    m5::rtc_datetime_t rtcTime;
    rtcTime.date.year = timeinfo.tm_year + 1900;
    rtcTime.date.month = timeinfo.tm_mon + 1;
    rtcTime.date.date = timeinfo.tm_mday;
    rtcTime.date.weekDay = timeinfo.tm_wday;
    rtcTime.time.hours = timeinfo.tm_hour;
    rtcTime.time.minutes = timeinfo.tm_min;
    rtcTime.time.seconds = timeinfo.tm_sec;
    M5.Rtc.setDateTime(rtcTime);
    timeSynced = true;
    Serial.printf("[RTC] NTP 同步 RTC 芯片: %04d-%02d-%02d %02d:%02d:%02d\n",
                  rtcTime.date.year, rtcTime.date.month, rtcTime.date.date,
                  rtcTime.time.hours, rtcTime.time.minutes, rtcTime.time.seconds);
    return true;
}

// ===== HTTP 时间权威校准：SNTP(UDP123) 在部分热点/防火墙被拦截时不可用，改走 HTTP 响应头 Date =====
// 无论 SNTP 结果如何都调用一次（HTTP 成功则 settimeofday 覆盖系统 time() 并写回 RTC 芯片）
bool fetchHttpTimeSync() {
    WiFiClient client;
    if (!client.connect("www.baidu.com", 80)) return false;
    client.setTimeout(6);
    client.println("GET / HTTP/1.1");
    client.println("Host: www.baidu.com");
    client.println("User-Agent: Mozilla/5.0");
    client.println("Connection: close");
    client.println();
    unsigned long deadline = millis() + 8000;
    char buf[1024];
    int n = 0;
    while (client.connected() && millis() < deadline && n < (int)sizeof(buf) - 1) {
        if (client.available()) buf[n++] = client.read();
        else delay(5);
    }
    buf[n] = 0;
    client.stop();
    // Date: Wed, 26 Aug 2026 06:58:10 GMT（RFC1123，GMT 即 UTC）
    char* p = strstr(buf, "Date: ");
    if (!p) return false;
    p += 6;
    int d = 0, y = 0, h = 0, mi = 0, s = 0;
    char mon[4] = {0};
    if (sscanf(p, "%*s %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &s) != 6) return false;
    static const char* const M[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int m = 0;
    for (int i = 0; i < 12; i++) if (strncmp(mon, M[i], 3) == 0) { m = i + 1; break; }
    if (m == 0 || y < 2024 || d < 1 || d > 31) return false;
    // Date 头是 GMT(UTC)，纯算术转 epoch（不依赖 TZ 环境变量，避免时区/DST 歧义）
    static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    long days = 0;
    for (int yy = 1970; yy < y; yy++) {
        bool leap = (yy % 4 == 0 && yy % 100 != 0) || (yy % 400 == 0);
        days += leap ? 366 : 365;
    }
    for (int mm = 1; mm < m; mm++) {
        int dm = mdays[mm - 1];
        if (mm == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))) dm = 29;
        days += dm;
    }
    days += d - 1;
    time_t utc = days * 86400L + h * 3600L + mi * 60L + s;
    if (utc < 1700000000LL) return false;   // 2023-11 之后，防异常值
    struct timeval tv = { utc, 0 };
    settimeofday(&tv, NULL);        // 覆盖系统 time()（UTC epoch，getLocalTime 按 +8h 显示北京时间）
    syncRtcFromNTP();               // 用正确的 time() 写回 RTC 芯片（待机页/日历页日期正确）
    Serial.printf("[RTC] HTTP 权威校准: %04d-%02d-%02d %02d:%02d:%02d UTC\n", y, m, d, h, mi, s);
    return true;
}
// ===== 省电待机（E2）：闲置自动待机 + 任意键唤醒 =====
bool standbyMode = false;        // 待机中（降频/断WiFi/待机页）
unsigned long lastActivityMs = 0; // 最近一次按键活动时刻
#define IDLE_SLEEP_MS 300000UL    // 闲置5分钟进入待机（生产值；#STANDBY 命令可强制进待机测试）
unsigned long wifiRetryAt = 0;     // WiFi 重连失败的重试时刻（0=无待重试；热点晚开等场景 30s 后自动重试）
int wakeJumpPage = -1;          // 待机唤醒后跳转目标页：A/B→0(首页), C→4(语音待办)；-1=不跳转
bool wifiReconnectPending = false; // 待机唤醒后待执行 WiFi 重连
bool cpWifiOk = false;           // 智谱轮询的 WiFi 连接缓存（待机断网时重置）
bool cpWifiBusy = false;         // WiFi 连接/智谱拉取中（RGB 青色反馈）
unsigned long lastScreenUpdate = 0;
// 待机页：纯白底+日期+天气（缓存），进入画一次不定时刷新（无时钟，省电+墨水屏寿命）

// ===== 待机唤醒辅助（E2 修复）：恢复频率 + 延迟重连 WiFi =====
// 唤醒只标记重连，实际连接推迟到按键处理完执行（重连阻塞 8s×N，不能卡按键响应）
void wakeFromStandby() {
    standbyMode = false;
    setCpuFrequencyMhz(240);
    wifiRetryAt = 0;
    wifiReconnectPending = true;
    suppressWakeClick = true;   // ★ 抑制唤醒键的首次按键事件：长按某键唤醒后，松开会被误判成该键点击（如 C 键切页跳转到语音待办）
    // ★ 唤醒后重配三键输入上拉（重要）：light sleep 唤醒后配置了 EXT1 唤醒的 RTC 引脚
    // 可能仍处于 RTC 模式，digitalRead 恒高 → 按键检测全部失效无法切页。
    // 重新 pinMode(INPUT_PULLUP) 恢复数字输入 + 同步 prevLevel 防脏边沿。
    pinMode(BTN_A_GPIO, INPUT_PULLUP);
    pinMode(BTN_B_GPIO, INPUT_PULLUP);
    pinMode(BTN_C_GPIO, INPUT_PULLUP);
    keyA.prevLevel = !digitalRead(BTN_A_GPIO);
    keyB.prevLevel = !digitalRead(BTN_B_GPIO);
    keyC.prevLevel = !digitalRead(BTN_C_GPIO);

    // ★ 待机唤醒直达页：A/B → 首页(0)，C → 语音待办(4)
    // 唤醒键按住期间抑制其点击/长按（松手不误触），跳转由 loop 按键处理前执行
    wakeJumpPage = -1;
    KeyState* wk = nullptr;
    if (!digitalRead(BTN_A_GPIO))      { wakeJumpPage = 0; wk = &keyA; }
    else if (!digitalRead(BTN_B_GPIO)) { wakeJumpPage = 0; wk = &keyB; }
    else if (!digitalRead(BTN_C_GPIO)) { wakeJumpPage = 4; wk = &keyC; }
    if (wk) {
        wk->suppressUntilRelease = true;
        wk->clickPending = false;
        wk->holdPending = false;
    }
    // 唤醒后若天气数据偏旧(>1h)则标记刷新：loop 天气定时在 WiFi 重连成功后自动拉取，
    // 下次待机页即显示新天气（节流避免频繁唤醒反复拉取）
    if (weatherUpdatedAt == 0 || millis() - weatherUpdatedAt > 3600000UL) {
        weatherUpdatedAt = 0;
    }
}

// 用 config 的 3 组 WiFi 重连（待机唤醒后调用；成功设 wifiConnected 并同步 NTP 时间）
bool reconnectWifiFromConfig() {
    if (WiFi.status() == WL_CONNECTED) { wifiConnected = true; return true; }
    WiFi.mode(WIFI_STA);
    for (int i = 0; i < 3; i++) {
        if (strlen(cpWifiSsid[i]) == 0) continue;
        WiFi.begin(cpWifiSsid[i], cpWifiPass[i]);
        unsigned long st = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - st < 8000UL) delay(200);
        if (WiFi.status() == WL_CONNECTED) {
            wifiConnected = true;
            configTime(8 * 3600, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
            syncRtcFromNTP();            // 待机唤醒后把真实时间写回 RTC 芯片（否则日期停在编译时间）
            fetchHttpTimeSync();         // HTTP 权威校准：SNTP 被拦截时强制覆盖真实时间
            Serial.printf("[WAKE] WiFi 已重连: %s (IP %s)\n", cpWifiSsid[i], WiFi.localIP().toString().c_str());
            return true;
        }
    }
    wifiConnected = false;
    Serial.println("[WAKE] WiFi 重连失败（无可用已知网络）");
    return false;
}

// 番茄钟状态 (以分钟为单位)
bool pomodoroRunning = false;
uint32_t pomodoroStartMillis = 0;
uint32_t pomodoroRemainMin = POMODORO_FOCUS_MIN;
bool pomodoroInRest = false;
uint32_t pomodoroDoneCount = 0;       // 今日完成番茄数
uint32_t todayReviewedCount = 0;      // 今日已复习考点数

// 记忆标记回显状态
char memoryNoticeBuf[64] = "";
unsigned long memoryNoticeTime = 0;

// 记忆状态数组（按运行时最大卡片数分配，避免 SD 加载更多卡时越界）
CardMemoryState kpState[KP_MAX_CARDS];

// ====================================================================
// 五、硬件级 SD 卡挂载与动态相册扫描引擎
// ====================================================================

bool initSDCard() {
    if (hasSDCard) return true;

    // M5PaperColor: SD 卡与 EPD 共享 SPI2_HOST 总线 (SCK=15, MISO=14, MOSI=13)，CS=GPIO47。
    // M5GFX 已把全局 SPI 对象配置到该总线，绝不能调用 SPI.end()/SPI.begin() 重新配置，
    // 否则会销毁 EPD 总线导致白屏。这里直接复用已配置好的总线挂载 SD 卡即可。
    if (SD.begin(GPIO_NUM_47, SPI, 25000000)) {
        hasSDCard = true;
        return true;
    }

    return false;
}

// 从 SD /cards/*.json 加载考点卡片（Anki 导出格式），替换内置 KNOWLEDGE_DB
// 文件名 ch01.json / ch02.json ...；JSON 为数组，每项 {category,question,answer,mnemonic}
// 失败/无文件则保持内置考点（kpCount=0）
void loadCardsFromSD() {
    if (!kpCards) { kpCount = 0; return; }   // PSRAM 分配失败则保持内置考点
    if (!initSDCard()) { kpCount = 0; return; }

    if (!SD.exists("/cards")) { kpCount = 0; Serial.println("[CARD] /cards 目录不存在"); return; }

    kpCount = 0;
    // 依次加载 ch01..ch99
    for (int ch = 1; ch <= 99 && kpCount < KP_MAX_CARDS; ch++) {
        char path[24];
        snprintf(path, sizeof(path), "/cards/ch%02d.json", ch);
        if (!SD.exists(path)) continue;   // 该章文件不存在则跳过（不一定是断号）

        File cf = SD.open(path, FILE_READ);
        if (!cf) continue;
        size_t sz = cf.size();
        if (sz == 0 || sz > 32 * 1024) { cf.close(); continue; }
        char* buf = (char*)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM);
        if (!buf) { cf.close(); continue; }
        cf.read((uint8_t*)buf, sz);
        buf[sz] = '\0';
        cf.close();

        JsonDocument doc;
        if (deserializeJson(doc, buf) != DeserializationError::Ok) {
            free(buf);
            Serial.printf("[CARD] %s JSON解析失败\n", path);
            continue;
        }
        free(buf);

        JsonArray arr = doc.as<JsonArray>();
        for (JsonObject obj : arr) {
            if (kpCount >= KP_MAX_CARDS) break;
            strncpy(kpCards[kpCount].category,
                    obj["category"] | "经济基础", sizeof(kpCards[kpCount].category) - 1);
            kpCards[kpCount].category[sizeof(kpCards[kpCount].category) - 1] = '\0';   // 修复M1: 截断后补NUL
            strncpy(kpCards[kpCount].question,
                    obj["question"] | "", sizeof(kpCards[kpCount].question) - 1);
            kpCards[kpCount].question[sizeof(kpCards[kpCount].question) - 1] = '\0';
            strncpy(kpCards[kpCount].answer,
                    obj["answer"] | "", sizeof(kpCards[kpCount].answer) - 1);
            kpCards[kpCount].answer[sizeof(kpCards[kpCount].answer) - 1] = '\0';
            strncpy(kpCards[kpCount].mnemonic,
                    obj["mnemonic"] | "", sizeof(kpCards[kpCount].mnemonic) - 1);
            kpCards[kpCount].mnemonic[sizeof(kpCards[kpCount].mnemonic) - 1] = '\0';
            kpCount++;
        }
        Serial.printf("[CARD] %s 加载 %u 张\n", path, (unsigned)arr.size());
    }

    if (kpCount > 0) {
        Serial.printf("[CARD] SD卡片加载成功: %d 张（替代内置 %d 张）\n", (int)kpCount, (int)KNOWLEDGE_COUNT);
    } else {
        Serial.println("[CARD] 无SD卡片，使用内置考点");
    }
}

// 编译时间兜底：无 WiFi 时用编译时刻写入 RTC（北京时区），保证日期时间可用
bool initRtcFromBuildTime() {
    // __DATE__ = "Aug  9 2026", __TIME__ = "HH:MM:SS"
    static const char* const MONTHS[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                         "Jul","Aug","Sep","Oct","Nov","Dec"};
    char dateStr[] = __DATE__;
    char timeStr[] = __TIME__;

    int month = 1, day = 1, year = 2026;
    char mon[4] = {0};
    sscanf(dateStr, "%3s %d %d", mon, &day, &year);
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon, MONTHS[i]) == 0) { month = i + 1; break; }
    }
    int hour = 0, minute = 0, second = 0;
    sscanf(timeStr, "%d:%d:%d", &hour, &minute, &second);

    // 无条件用编译时间覆盖 RTC（编译时刻即当前时刻，比旧 RTC 更可信）
    // 注意：NTP 同步在 setup 后续步骤执行，会再次覆盖为精确时间
    m5::rtc_datetime_t rtcTime;
    rtcTime.date.year = year;
    rtcTime.date.month = month;
    rtcTime.date.date = day;
    rtcTime.date.weekDay = 0;   // 由库计算
    rtcTime.time.hours = hour;
    rtcTime.time.minutes = minute;
    rtcTime.time.seconds = second;
    M5.Rtc.setDateTime(rtcTime);
    timeSynced = true;
    return true;
}

bool initWiFiNTP() {
    if (strlen(WIFI_SSID) == 0 || strcmp(WIFI_SSID, "Your_WiFi_SSID") == 0) {
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
        delay(200);
    }
    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (!wifiConnected) {
        return false;
    }

    configTime(8 * 3600, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 2000)) {
        m5::rtc_datetime_t rtcTime;
        rtcTime.date.year = timeinfo.tm_year + 1900;
        rtcTime.date.month = timeinfo.tm_mon + 1;
        rtcTime.date.date = timeinfo.tm_mday;
        rtcTime.date.weekDay = timeinfo.tm_wday;
        rtcTime.time.hours = timeinfo.tm_hour;
        rtcTime.time.minutes = timeinfo.tm_min;
        rtcTime.time.seconds = timeinfo.tm_sec;
        M5.Rtc.setDateTime(rtcTime);
        timeSynced = true;
        return true;
    }
    return false;
}

// 定时提醒持久化（/reminders.txt，每行 "HH:MM|内容"）
void saveReminders() {
    if (!initSDCard()) return;
    File f = SD.open("/reminders.txt", FILE_WRITE);
    if (!f) return;
    for (int i = 0; i < reminderCount; i++) {
        f.print(reminders[i].time);
        f.print("|");
        f.println(reminders[i].text);
    }
    f.close();
}
void loadReminders() {
    reminderCount = 0;
    if (!initSDCard() || !SD.exists("/reminders.txt")) return;
    File f = SD.open("/reminders.txt", FILE_READ);
    if (!f) return;
    while (f.available() && reminderCount < REMINDER_MAX) {
        String line = f.readStringUntil('\n');
        line.trim();
        int p = line.indexOf('|');
        if (p == 5 && p + 1 < (int)line.length()) {
            line.substring(0, 5).toCharArray(reminders[reminderCount].time, sizeof(reminders[reminderCount].time));
            String txt = line.substring(p + 1); txt.trim();
            strncpy(reminders[reminderCount].text, txt.c_str(), sizeof(reminders[reminderCount].text) - 1);
            reminders[reminderCount].text[sizeof(reminders[reminderCount].text) - 1] = '\0';
            reminders[reminderCount].fired = false;
            reminderCount++;
        }
    }
    f.close();
}

void loadTodoFromSD() {
    todoCount = 0;
    if (!initSDCard()) return;

    const char* paths[] = {"/todo.txt", "/todos.txt", "/todos/todo.txt"};
    File f;
    for (const char* p : paths) {
        if (SD.exists(p)) { f = SD.open(p, FILE_READ); if (f) break; }
    }
    if (!f) return;

    while (f.available() && todoCount < TODO_MAX) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        // 支持前缀标记：!高优先级  #已完成
        bool hi = line.startsWith("!");
        bool done = line.startsWith("#");
        if (hi || done) line = line.substring(1);
        line.trim();
        if (line.length() == 0) continue;
        // 解析语音标记：@/record/todo_N.wav  文本
        todoItems[todoCount].audioFile[0] = '\0';
        if (line.startsWith("@")) {
            int sp = line.indexOf(' ');
            if (sp > 1) {
                String af = line.substring(1, sp);
                strncpy(todoItems[todoCount].audioFile, af.c_str(), sizeof(todoItems[todoCount].audioFile) - 1);
                todoItems[todoCount].audioFile[sizeof(todoItems[todoCount].audioFile) - 1] = '\0';
                line = line.substring(sp + 1);
                line.trim();
            }
        }
        // 解析录音时间+秒数（MM-DD HH:MM|N，语音待办；无则清空）
        todoItems[todoCount].recTime[0] = '\0';
        todoItems[todoCount].recSec = 0;
        if (line.length() >= 14 && line[2] == '-' && line[5] == ' ' && line[8] == ':' && line[11] == '|') {
            String rt = line.substring(0, 11);   // "MM-DD HH:MM"
            strncpy(todoItems[todoCount].recTime, rt.c_str(), sizeof(todoItems[todoCount].recTime) - 1);
            todoItems[todoCount].recTime[sizeof(todoItems[todoCount].recTime) - 1] = '\0';
            int secStart = 12;
            int sp2 = line.indexOf(' ', secStart);
            String secStr = (sp2 > 0) ? line.substring(secStart, sp2) : line.substring(secStart);
            todoItems[todoCount].recSec = (uint16_t)secStr.toInt();
            line = (sp2 > 0) ? line.substring(sp2 + 1) : String("");
            line.trim();
        }
        // 解析语音转写文字（|分隔，兼容旧格式：@文件 语音备忘 |转写文字）
        todoItems[todoCount].asr[0] = '\0';
        int pbar = line.indexOf('|');
        if (pbar > 0) {
            String as = line.substring(pbar + 1); as.trim();
            strncpy(todoItems[todoCount].asr, as.c_str(), sizeof(todoItems[todoCount].asr) - 1);
            todoItems[todoCount].asr[sizeof(todoItems[todoCount].asr) - 1] = '\0';
            line = line.substring(0, pbar); line.trim();
        }
        strncpy(todoItems[todoCount].text, line.c_str(), sizeof(todoItems[todoCount].text) - 1);
        todoItems[todoCount].text[sizeof(todoItems[todoCount].text) - 1] = '\0';
        todoItems[todoCount].done = done;
        todoItems[todoCount].highPriority = hi;
        todoCount++;
    }
    f.close();
}

// 扫描 SD 卡 /qr/ 目录下的二维码图片（png/jpg/bmp），页面 5 使用
void scanQRCodeDirectory() {
    qrCount = 0;
    if (!initSDCard()) return;

    File dir = SD.open("/qr");
    if (!dir || !dir.isDirectory()) return;

    File file = dir.openNextFile();
    while (file && qrCount < 8) {
        if (!file.isDirectory()) {
            const char* name = file.name();
            if (name[0] == '.') { file = dir.openNextFile(); continue; }
            if (strstr(name, ".png") || strstr(name, ".PNG") ||
                strstr(name, ".jpg") || strstr(name, ".JPG") ||
                strstr(name, ".bmp") || strstr(name, ".BMP")) {
                char full[64];
                snprintf(full, sizeof(full), "/qr/%s", name);
                strncpy(qrPaths[qrCount], full, sizeof(qrPaths[0]) - 1);
                qrPaths[qrCount][sizeof(qrPaths[0]) - 1] = '\0';
                qrCount++;
            }
        }
        file = dir.openNextFile();
    }
}

// 将待办列表写回 SD /todo.txt（统一持久化入口）
// 语音待办用 "@文件名 " 前缀标记，重启后 loadTodoFromSD 能恢复语音关联
void saveTodoListToSD() {
    if (!initSDCard()) return;
    File f = SD.open("/todo.txt", FILE_WRITE);
    if (!f) return;
    for (size_t i = 0; i < todoCount; i++) {
        if (todoItems[i].highPriority) f.print("!");
        else if (todoItems[i].done) f.print("#");
        if (todoItems[i].audioFile[0] != '\0') {
            f.print("@");                 // 语音标记：@/record/todo_N.wav
            f.print(todoItems[i].audioFile);
            f.print(" ");
            // 录音时间+秒数（MM-DD HH:MM|N，重启后恢复横条秒数显示）
            if (todoItems[i].recTime[0]) {
                f.print(todoItems[i].recTime);
                f.print("|");
                f.print(todoItems[i].recSec);
                f.print(" ");
            }
        }
        f.print(todoItems[i].text);
        // 语音转写文字（|分隔，重启后 loadTodoFromSD 恢复）
        if (todoItems[i].asr[0] != '\0') { f.print(" |"); f.print(todoItems[i].asr); }
        f.println();
    }
    f.close();
}

// 前向声明（供 refreshPageData / recordTodoVoice 调用）
void updateCalendarData();
void renderScreen(bool forceQuality = false);
void drawTodoPage();

// 页面数据刷新：切换页面/手动刷新时重拉对应数据
void refreshPageData() {
    if (currentPage == 0) {
        updateCalendarData();   // 日历页：更新黄历/天气数据
    } else if (currentPage == 1) {
        // 早报页：尝试联网刷新（无网则保持内置数据）
        if (wifiConnected) {
            // 简化：联网逻辑留接口，此处保持内置数据
        }
    } else if (currentPage == 4) {
        if (todoCount == 0) loadTodoFromSD();   // 待办页：加载待办
    } else if (currentPage == 6) {
        if (qrCount == 0) scanQRCodeDirectory();   // 二维码页（最后一页）
    }
}

// ===== TTS 中文语音合成（esp-sr v1.9.5）=====
// 模型从 flash voice_data 分区读入 PSRAM（已由 esptool 烧录 2.78MB xiaoxin_small）。
// 任何一步失败都静默降级（ttsReady=false），现有功能完全不受影响。
void initTTS() {
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    if (!part) { Serial.println("[TTS] 无 voice_data 分区，语音朗读禁用"); return; }
    size_t fsize = part->size;
    void* p = heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
    if (!p) { Serial.println("[TTS] PSRAM 分配失败"); return; }
    if (esp_partition_read(part, 0, p, fsize) != ESP_OK) { heap_caps_free(p); Serial.println("[TTS] 模型读取失败"); return; }
    g_ttsModel = p;
    esp_tts_voice_t* voice = esp_tts_voice_set_init(&esp_tts_voice_template, p);
    if (!voice) { heap_caps_free(p); g_ttsModel = nullptr; Serial.println("[TTS] 音色初始化失败"); return; }
    g_tts = esp_tts_create(voice);
    if (!g_tts) { Serial.println("[TTS] 引擎创建失败"); return; }
    g_ttsPcm = (int16_t*)heap_caps_malloc(TTS_PCM_MAX, MALLOC_CAP_SPIRAM);
    ttsReady = (g_ttsPcm != nullptr);
    Serial.printf("[TTS] 引擎就绪（模型 %u 字节 @ flash voice_data）\n", fsize);
}

// TTS 合成并播放一段中文（阻塞式，与现有 SD 语音朗读一致）
// 未就绪/录音/回放中时静默跳过，绝不影响现有功能
void ttsSpeak(const char* text) {
    if (!ttsReady || !g_tts || !g_ttsPcm) return;
    if (!text || !text[0]) return;
    if (isRecordingNow || voicePlaying) return;   // 与录音/回放互斥
    if (M5.Mic.isEnabled()) M5.Mic.end();          // 释放 Mic（与播放 WAV 一致，避免 I2S 冲突）
    M5.Speaker.begin();
    M5.Speaker.setVolume(255);
    M5.Speaker.setAllChannelVolume(255);
    if (!esp_tts_parse_chinese(g_tts, text)) return;
    int total = 0;
    while (total < TTS_PCM_MAX / 2) {
        int len = 0;
        short* data = esp_tts_stream_play(g_tts, &len, 2);   // speed=2（自然语速）
        if (len <= 0) break;
        memcpy(g_ttsPcm + total, data, len * 2);
        total += len;
    }
    if (total <= 0) return;
    // 音量增益：esp-sr 合成 PCM 振幅偏低，放大 2 倍提升响度（带削波保护）
    for (int i = 0; i < total; i++) {
        int32_t v = g_ttsPcm[i] * 2;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        g_ttsPcm[i] = (int16_t)v;
    }
    M5.Speaker.playRaw(g_ttsPcm, total, 16000);
    unsigned long waitStart = millis();
    while (M5.Speaker.isPlaying() && millis() - waitStart < 60000UL) delay(20);
    M5.Speaker.stop();
}

// 语音相关（页面 2 朗读考点 / 页面 4 回放待办）
// PaperColor 无内置中文 TTS：优先播放 SD 卡预录音频（答案 /voice/kp_NN_ans.wav、
// 口诀 /voice/kp_NN_mne.wav，可自行用 TTS 生成），无音频则提示音 + 屏幕显示答案/口诀
void speakCurrentKnowledge() {
    if (currentPage != 2) return;
    const char* qTxt = cardQuestion(ebbinghausIndex);
    const char* aTxt = cardAnswer(ebbinghausIndex);
    const char* mTxt = cardMnemonic(ebbinghausIndex);

    // 辅助：播放一个 WAV 文件并等待完成
    auto playWavFile = [](const char* p) -> bool {
        if (!SD.exists(p)) return false;
        File f = SD.open(p, FILE_READ);
        if (!f) return false;
        size_t fsize = f.size();
        if (fsize <= 44 || fsize > 6 * 1024 * 1024) { f.close(); return false; }
        uint8_t* wavData = (uint8_t*)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
        if (!wavData) wavData = (uint8_t*)malloc(fsize);
        if (!wavData) { f.close(); return false; }
        f.read(wavData, fsize);
        f.close();
        if (M5.Mic.isEnabled()) M5.Mic.end();
        M5.Speaker.begin();
        M5.Speaker.playWav(wavData, fsize);
        unsigned long waitStart = millis();
        while (M5.Speaker.isPlaying() && millis() - waitStart < 15000UL) delay(20);
        M5.Speaker.stop();
        free(wavData);
        return true;
    };

    bool played = false;
    if (initSDCard()) {
        char pathAns[64], pathMne[64];
        snprintf(pathAns, sizeof(pathAns), "/voice/kp_%02d_ans.wav", (int)ebbinghausIndex + 1);
        snprintf(pathMne, sizeof(pathMne), "/voice/kp_%02d_mne.wav", (int)ebbinghausIndex + 1);
        bool p1 = playWavFile(pathAns);
        bool p2 = playWavFile(pathMne);
        played = p1 || p2;
    }

    if (!played) {
        if (ttsReady) {
            // 有 TTS 引擎：合成朗读 问题+答案+口诀
            char txt[512];
            snprintf(txt, sizeof(txt), "%s。%s。%s。", qTxt, aTxt, mTxt);
            ttsSpeak(txt);
        } else {
            // 无 TTS：保留原提示音示意（答案+口诀的语音需自行放入 SD 卡 /voice/）
            M5.Speaker.begin();
            M5.Speaker.tone(880, 100);
            delay(120);
            M5.Speaker.tone(1046, 160);
            delay(200);
            M5.Speaker.tone(784, 120);
        }
    }
    // 屏幕显示答案与口诀（朗读内容同步可视化）
    snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "答:%s 口:%s", aTxt, mTxt);
    memoryNoticeTime = millis();
}

// 生成 WAV 文件头（16bit 单声道 PCM）
void writeWavHeader(File& f, uint32_t dataBytes, uint32_t sampleRate) {
    uint8_t hdr[44] = {0};
    memcpy(hdr, "RIFF", 4);
    uint32_t chunkSize = 36 + dataBytes;
    memcpy(hdr + 4, &chunkSize, 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmtSize = 16;
    memcpy(hdr + 16, &fmtSize, 4);
    uint16_t audioFmt = 1;         // PCM
    memcpy(hdr + 20, &audioFmt, 2);
    uint16_t channels = 1;
    memcpy(hdr + 22, &channels, 2);
    memcpy(hdr + 24, &sampleRate, 4);
    uint32_t byteRate = sampleRate * 2;
    memcpy(hdr + 28, &byteRate, 4);
    uint16_t blockAlign = 2;
    memcpy(hdr + 32, &blockAlign, 2);
    uint16_t bitsPerSample = 16;
    memcpy(hdr + 34, &bitsPerSample, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &dataBytes, 4);
    f.write(hdr, 44);
}

// 前向声明（recordTodoVoice / processRecording 调用）
void saveRecording();

// loop 中调用：录音进行时分块采样并立即流式写入 SD（每块约 250ms）
// 注意：M5.Mic 非线程安全，必须在主循环调用（任务方案会导致完全采不到）
// 关键：M5.Mic.record() 是异步双缓冲——第 N 次 record(recSubmit) 返回时，
// 第 N-1 次提交的 recRead 缓冲才填好。乒乓交替：提交 A、读已填好的 B。
// (完全参照官方 Microphone.ino 模式)
void processRecording() {
    if (!isRecordingNow || !recMicReady || !recWavFile) return;
    // 提交当前块（异步，数据由后台 mic_task 填充到 recSubmit）
    if (M5.Mic.record(recSubmit, REC_CHUNK_SAMPLES * 2, REC_SAMPLE_RATE, true)) {
        // 读取上一块已填好的 recRead 并写入 SD
        if (!recFirstBlock) {
            // 只取 L 声道（重要修正：ES7210 MIC1 增益=0x1B 是真实麦克风通道，MIC2 增益=0 无声）
            // 之前误判 R 声道有人声并丢弃 L 是错的（是单缓冲 bug 读到旧数据的假象）
            for (int i = 0; i < REC_CHUNK_SAMPLES; i++) recRead[i] = recRead[i * 2];
            recWavFile.write((const uint8_t*)recRead, REC_CHUNK_SAMPLES * 2);
            recWritePos += REC_CHUNK_SAMPLES * 2;
        }
        recFirstBlock = false;
        // 交换乒乓：本次提交的变"上一块"，下次读它；另一块用于下次提交
        int16_t* tmp = recSubmit;
        recSubmit = recRead;
        recRead = tmp;
    }
}

// ===== 全局语音命令模式（任意页长按C唤起，最小路径）=====
// 原则：唤起后绝不刷屏（墨水屏刷新会阻塞采样丢数据），用 RGB 蓝灯实时反馈；
// 识别命令后由 handleVoiceCommand 内部一次 renderScreen 切到目标页（全程仅1次刷屏）
void stopVoiceCommand();   // 前向声明（processVoiceCommand 调用）
void startVoiceCommand() {
    if (voiceCmdMode || isRecordingNow || voicePlaying || asrPending || fileReceiving) return;
    if (M5.Speaker.isEnabled()) { M5.Speaker.end(); delay(50); }   // 释放喇叭(共用GPIO45/时钟)
    m5::mic_config_t mc;
    mc.pin_data_in = 39; mc.pin_ws = 41; mc.pin_bck = 40; mc.pin_mck = 42;
    mc.input_channel = m5::input_stereo;
    mc.over_sampling = 2;
    mc.sample_rate = REC_SAMPLE_RATE;
    mc.magnification = 64;
    M5.Mic.config(mc);
    M5.Mic.begin();
    if (!vcAudioBuf) vcAudioBuf = (uint8_t*)heap_caps_malloc(VC_CMD_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!vcAudioBuf) { M5.Mic.end(); Serial.println("[VC] PSRAM分配失败"); return; }
    vcAudioMax = VC_CMD_MAX_BYTES;
    vcAudioLen = 0;
    vcCmdStart = millis();
    recSubmit = recBufA; recRead = recBufB; recFirstBlock = true;
    voiceCmdMode = true;
    Serial.println("[VC] 语音命令开始（蓝灯，不刷屏）");
}

// 主循环：乒乓采样累积到内存（与待办录音共用乒乓缓冲但互斥）
void processVoiceCommand() {
    if (!voiceCmdMode) return;
    if (M5.Mic.record(recSubmit, REC_CHUNK_SAMPLES * 2, REC_SAMPLE_RATE, true)) {
        if (!recFirstBlock) {
            for (int i = 0; i < REC_CHUNK_SAMPLES; i++) recRead[i] = recRead[i * 2];   // 取L声道
            if (vcAudioLen + REC_CHUNK_SAMPLES * 2 <= vcAudioMax) {
                memcpy(vcAudioBuf + vcAudioLen, recRead, REC_CHUNK_SAMPLES * 2);
                vcAudioLen += REC_CHUNK_SAMPLES * 2;
            }
        }
        recFirstBlock = false;
        int16_t* tmp = recSubmit; recSubmit = recRead; recRead = tmp;
    }
    if (millis() - vcCmdStart >= VC_CMD_MS) stopVoiceCommand();   // 3.5s 自动停止
}

// 停止：释放 Mic，把缓冲交给后台 ASR 转写（asrPendingIdx=-1 标记命令模式，非待办）
void stopVoiceCommand() {
    if (!voiceCmdMode) return;
    voiceCmdMode = false;
    M5.Mic.end();
    if (vcAudioLen < REC_SAMPLE_RATE) {   // <1秒视为无效
        Serial.printf("[VC] 语音太短 %d\n", (int)vcAudioLen);
        return;
    }
    asrAudioBuf = vcAudioBuf;      // 所有权转给 asrTaskEntry（转写完自动 free）
    asrAudioLen = vcAudioLen;
    vcAudioBuf = nullptr;
    asrPending = true;
    asrPendingIdx = -1;            // -1 = 全局语音命令
    Serial.printf("[VC] 停止，转写 %d 字节\n", (int)asrAudioLen);
}

// 录音交互（状态机）：
//   待办页短按C → 开始录音（RGB蓝灯快闪），再短按C → 停止并保存 WAV
// 支持任意时长（缓冲上限 60 秒）；分块采样避免阻塞按键检测。
void recordTodoVoice() {
    if (isRecordingNow) {
        // 已在录音 → 停止并保存
        saveRecording();
        return;
    }

    Serial.printf("[REC] 开始录音 todoCount=%d page=%d\n", (int)todoCount, (int)currentPage);

    // 开始录音
    if (todoCount >= TODO_MAX) {
        Serial.println("[REC] 待办已满");
        snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[待办已满]");
        memoryNoticeTime = millis();
        return;
    }
    if (!initSDCard()) {
        Serial.println("[REC] 无SD卡");
        snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[无SD卡]");
        memoryNoticeTime = millis();
        return;
    }

    // 关键：PaperColor 的喇叭(ES8311)与麦克风(ES7210)共用 GPIO45 电源与
    // MCLK/BCK/WS(42/40/41)。录音前必须先停用喇叭，避免两个 codec 互相干扰产生噪音。
    if (M5.Speaker.isEnabled()) {
        M5.Speaker.end();
        delay(50);
    }

    // 流式方案：开始录音即创建 WAV 文件并写头（不分配大内存缓冲）
    SD.mkdir("/record");
    char path[64];
    snprintf(path, sizeof(path), "/record/todo_%d.wav", (int)todoCount);
    recWavFile = SD.open(path, FILE_WRITE);
    if (!recWavFile) {
        snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[写入失败]");
        memoryNoticeTime = millis();
        return;
    }
    writeWavHeader(recWavFile, 0, REC_SAMPLE_RATE);   // 头先写 dataBytes=0，停止时回填

    // 显式配置 ES7210 麦克风引脚（M5Unified 默认 pin_data_in=-1 未启用 PaperColor 的 Mic！）
    {
        m5::mic_config_t micCfg;
        micCfg.pin_data_in = 39;
        micCfg.pin_ws = 41;
        micCfg.pin_bck = 40;
        micCfg.pin_mck = 42;
        // 双声道录制（保留 L/R 供后续取 L 声道）；增益 64 平衡音质（96 削波、48 偏弱）
        micCfg.input_channel = m5::input_stereo;
        micCfg.over_sampling = 2;      // 默认2次过采样平均，滤波更干净
        micCfg.sample_rate = REC_SAMPLE_RATE;
        micCfg.magnification = 64;
        M5.Mic.config(micCfg);
    }
    M5.Mic.begin();
    // 重置乒乓状态：recSubmit 提交第一块，recRead 无数据（recFirstBlock=true 跳过）
    recSubmit = recBufA;
    recRead = recBufB;
    recFirstBlock = true;
    recMicReady = true;
    recWritePos = 0;
    isRecordingNow = true;

    snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[录音中…再按C停止]");
    memoryNoticeTime = millis();
    // 关键：开始录音时绝不刷屏！墨水屏刷新会阻塞主循环约 15 秒，
    // 期间 processRecording 无法执行 → 前 15 秒数据全部丢失（用户实测只保留 5 秒）。
    // 改用 LED 蓝灯快闪（loop 状态灯系统检测 isRecordingNow 自动点亮，不阻塞）。
    // 屏幕提示延迟到停止录音时 renderScreen 一并刷新。
    // 注意：此处绝不能操作 M5.Speaker！Mic 已 begin 占用共享 I2S/GPIO45，
    // 再 begin Speaker 会导致 I2S 端口冲突 + spk_task 栈溢出崩溃。
}

// ===== 语音待办转文字：SiliconFlow SenseVoice ASR（multipart/form-data POST） =====
// 读 /record/todo_N.wav → 上传 → 解析返回 text 填入 outText；失败返回 false（不阻塞录音保存）
bool asrTranscribe(const uint8_t* audio, size_t fsize, char* outText, size_t outLen) {
    outText[0] = '\0';
    if (strlen(sfApiKey) == 0) { Serial.println("[ASR] 未配置 sf_api_key"); return false; }
    if (!audio || fsize < 44) { Serial.println("[ASR] 音频缓冲无效"); return false; }
    // WiFi 未连则尝试用 config 的 WiFi 连接
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_STA);
        for (int i = 0; i < 3; i++) {
            if (strlen(cpWifiSsid[i]) == 0) continue;
            WiFi.begin(cpWifiSsid[i], cpWifiPass[i]);
            unsigned long st = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - st < 8000UL) delay(200);
            if (WiFi.status() == WL_CONNECTED) { wifiConnected = true; break; }
        }
        if (WiFi.status() != WL_CONNECTED) { Serial.println("[ASR] WiFi未连接，跳过转写"); return false; }
    }

    // 构造 multipart/form-data（HTTP/1.0 避免 chunked，与天气同款）
    const char* boundary = "----ESP32AsrBoundary7d2c";
    char pre[256], mid[256], post[64];
    snprintf(pre, sizeof(pre),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"todo.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", boundary);
    snprintf(mid, sizeof(mid),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "FunAudioLLM/SenseVoiceSmall\r\n", boundary);
    snprintf(post, sizeof(post), "--%s--\r\n", boundary);
    size_t preLen = strlen(pre), midLen = strlen(mid), postLen = strlen(post);

    // 用 HTTP(80) 而非 HTTPS：设备 WiFiClientSecure TLS 握手到 SiliconFlow 实测 5.6s+ 且可能挂起卡死，
    // 明文 HTTP 避开 TLS 握手，connect 快且不卡（个人设备+小请求体，安全性可接受）
    WiFiClient client;
    client.setTimeout(8);
    unsigned long tStart = millis();
    if (!client.connect("api.siliconflow.cn", 80)) {
        Serial.println("[ASR] connect失败");
        return false;
    }
    Serial.printf("[ASR] connect %lums\n", (unsigned long)(millis() - tStart));
    char req[512];
    // 必须 HTTP/1.1 + Connection: close！SiliconFlow 服务器对 HTTP/1.0 请求不响应(挂起卡死)，
    // 且响应用 Content-Length 非 chunked，直接解析 body 即可
    snprintf(req, sizeof(req),
        "POST /v1/audio/transcriptions HTTP/1.1\r\n"
        "Host: api.siliconflow.cn\r\n"
        "Connection: close\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %u\r\n\r\n",
        sfApiKey, boundary, (unsigned)(preLen + fsize + midLen + postLen));
    client.print(req);
    // 分批上传（弱网下避免整块 write 长时间阻塞；每块检查写失败/总超时 15s）
    struct { const uint8_t* p; size_t n; } parts[4] = {
        {(const uint8_t*)pre, preLen}, {audio, fsize}, {(const uint8_t*)mid, midLen}, {(const uint8_t*)post, postLen}
    };
    bool upFail = false;
    for (int pi = 0; pi < 4 && !upFail; pi++) {
        size_t off = 0;
        while (off < parts[pi].n) {
            if (millis() - tStart > 30000) { upFail = true; break; }   // 大录音(数百KB)在弱网上传慢，放宽到 30s
            size_t chunk = parts[pi].n - off; if (chunk > 2048) chunk = 2048;   // 大块减少写次数
            size_t w = client.write(parts[pi].p + off, chunk);
            if (w == 0) { upFail = true; break; }
            off += w;
        }
    }
    if (upFail) { Serial.println("[ASR] 上传失败/超时"); client.stop(); return false; }
    Serial.printf("[ASR] 上传 %u 字节 %lums\n", (unsigned)fsize, (unsigned long)(millis() - tStart));

    // 读响应（最长 20s，长录音转写需更久）
    const int RBUF = 4096;
    static char* rbuf = nullptr;
    if (!rbuf) rbuf = (char*)heap_caps_malloc(RBUF, MALLOC_CAP_SPIRAM);
    if (!rbuf) return false;
    int pos = 0;
    unsigned long t0 = millis();
    while (client.connected() && millis() - t0 < 20000 && pos < RBUF - 1) {
        while (client.available() && pos < RBUF - 1) {
            char c = client.read();
            if (c >= 0) rbuf[pos++] = c;
        }
        delay(1);
    }
    client.stop();
    rbuf[pos] = '\0';
    char* body = strstr(rbuf, "\r\n\r\n");
    if (!body) { Serial.printf("[ASR] 无body rbuf=%.120s\n", rbuf); return false; }
    body += 4;

    JsonDocument doc;
    if (deserializeJson(doc, body)) { Serial.printf("[ASR] 解析失败 body=%.160s\n", body); return false; }
    const char* txt = doc["text"] | "";
    if (txt[0] == '\0') { Serial.printf("[ASR] 无text body=%.200s\n", body); return false; }
    strncpy(outText, txt, outLen - 1);
    outText[outLen - 1] = '\0';
    Serial.printf("[ASR] 转写成功: %s\n", outText);
    return true;
}

// 转写任务入口：联网转写（不碰 SD/EPD 避免 SPI 冲突），结果由主循环消费；失败自动重试 2 次（弱网超时恢复）
void asrTaskEntry(void*) {
    Serial.println("[ASR] 任务启动");
    char buf[512];
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        if (attempt > 0) {
            Serial.printf("[ASR] 重试 %d/%d\n", attempt + 1, 2);
            vTaskDelay(3000 / portTICK_PERIOD_MS);
        }
        ok = asrTranscribe(asrAudioBuf, asrAudioLen, buf, sizeof(buf));
    }
    if (ok) {
        strncpy(asrResult, buf, sizeof(asrResult) - 1);
        asrResult[sizeof(asrResult) - 1] = '\0';
        asrResultIdx = asrPendingIdx;
        asrResultReady = true;
    } else if (asrPendingIdx == -1) {
        // 命令模式转写失败：置空结果供主循环提示「识别失败」（避免无反馈）
        asrResult[0] = '\0';
        asrResultIdx = -1;
        asrResultReady = true;
    }
    if (asrAudioBuf) { free(asrAudioBuf); asrAudioBuf = nullptr; asrAudioLen = 0; }
    asrPending = false;
    asrRunning = false;
    vTaskDelete(NULL);
}

// ===== 语音命令助手（B）：从语音提取定时提醒（"下午3点开会"→15:00 开会） =====
void fetchWeather();   // 前向声明（handleVoiceCommand 调用）
bool extractReminder(const char* text, char* hm, char* remText, size_t remLen) {
    const char* dian = strstr(text, "点");
    if (!dian) return false;
    const char* p = dian - 1;
    int mul = 1, acc = 0;
    while (p >= text && *p >= '0' && *p <= '9') { acc += (*p - '0') * mul; mul *= 10; p--; }
    if (mul <= 1 || acc > 23) return false;   // 无数字或小时非法
    int hh = acc, mm = 0;
    if ((strstr(text, "下午") || strstr(text, "晚上") || strstr(text, "傍晚")) && hh < 12) hh += 12;
    if (strstr(dian, "点半")) mm = 30;
    snprintf(hm, 6, "%02d:%02d", hh, mm);
    const char* tail = dian + (strstr(dian, "点半") ? 3 : 1);
    while (*tail == ' ') tail++;   // 跳过前导空格（中文语气词不逐个比较，UTF-8 多字节）
    if (*tail == '\0') tail = "到点提醒";
    strncpy(remText, tail, remLen - 1);
    remText[remLen - 1] = '\0';
    return true;
}

// 语音命令识别：转写文字 → 执行设备命令；返回 true=已作为命令处理（不存待办）
bool handleVoiceCommand(const char* text) {
    if (!text || text[0] == '\0') return false;
    size_t tl = strlen(text);
    // 定时提醒（优先）：含数字+点，如 "提醒我3点开会" / "3点半打卡"
    char hm[6], remText[64];
    if ((strstr(text, "提醒") || strstr(text, "记得")) && extractReminder(text, hm, remText, sizeof(remText))) {
        if (reminderCount < REMINDER_MAX) {
            memcpy(reminders[reminderCount].time, hm, 6);
            strncpy(reminders[reminderCount].text, remText, sizeof(reminders[reminderCount].text) - 1);
            reminders[reminderCount].text[sizeof(reminders[reminderCount].text) - 1] = '\0';
            reminders[reminderCount].fired = false;
            reminderCount++;
            saveReminders();
            char txt[96];
            snprintf(txt, sizeof(txt), "已设置提醒，%s，%s。", remText, hm);
            ttsSpeak(txt);
            snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[语音] 已设提醒 %s %s", hm, remText);
            memoryNoticeTime = millis();
            Serial.printf("[CMD] 提醒 %s %s\n", hm, remText);
        }
        return true;
    }
    // 天气（短句）："今天天气" / "天气怎么样"
    if (strstr(text, "天气") && tl <= 12) {
        currentPage = 0; refreshPageData(); fetchWeather(); renderScreen(true);
        char wtxt[96];
        snprintf(wtxt, sizeof(wtxt), "今日天气，%s，%d到%d摄氏度。", weatherText, weatherLow, weatherHigh);
        ttsSpeak(wtxt);
        snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[语音] 已播报天气");
        memoryNoticeTime = millis();
        Serial.println("[CMD] 天气命令");
        return true;
    }
    // 早报（短句）："读新闻" / "早报"
    if ((strstr(text, "新闻") || strstr(text, "早报")) && tl <= 12) {
        currentPage = 1; refreshPageData(); renderScreen(false);
        if (newsCount > 0) {
            char txt[256];
            snprintf(txt, sizeof(txt), "%s。%s", newsTitle(0), newsSummary(0));
            ttsSpeak(txt);
        }
        snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[语音] 已朗读早报");
        memoryNoticeTime = millis();
        Serial.println("[CMD] 早报命令");
        return true;
    }
    // 番茄（短句）："开始番茄" / "专注"
    if ((strstr(text, "番茄") || strstr(text, "专注")) && tl <= 12) {
        currentPage = 5;
        pomodoroRunning = true; pomodoroInRest = false;
        pomodoroStartMillis = millis(); pomodoroRemainMin = POMODORO_FOCUS_MIN;
        refreshPageData(); renderScreen(false);
        ttsSpeak("番茄钟开始，专注二十五分钟。");
        snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[语音] 番茄钟已启动");
        memoryNoticeTime = millis();
        Serial.println("[CMD] 番茄命令");
        return true;
    }
    // 切页（含打开/切到/去 + 页面名）："打开日历" / "去看待办" / "切到考点"
    if (strstr(text, "打开") || strstr(text, "切到") || strstr(text, "去") || strstr(text, "显示")) {
        if (strstr(text, "日历")) { currentPage = 0; refreshPageData(); renderScreen(false); Serial.println("[CMD] 切页:日历"); return true; }
        if (strstr(text, "待办")) { currentPage = 4; refreshPageData(); renderScreen(false); Serial.println("[CMD] 切页:待办"); return true; }
        if (strstr(text, "考点") || strstr(text, "卡片")) { currentPage = 2; refreshPageData(); renderScreen(false); Serial.println("[CMD] 切页:考点"); return true; }
        if (strstr(text, "仪表盘")) { currentPage = 5; refreshPageData(); renderScreen(false); Serial.println("[CMD] 切页:仪表盘"); return true; }
        if (strstr(text, "二维码")) { currentPage = 6; refreshPageData(); renderScreen(false); Serial.println("[CMD] 切页:二维码"); return true; }
        if (strstr(text, "早报") || strstr(text, "新闻")) { currentPage = 1; refreshPageData(); renderScreen(false); Serial.println("[CMD] 切页:早报"); return true; }
    }
    // 直接说页面名（命令模式常用，整句精确匹配防误判）："日历""待办""考点""仪表盘""二维码"
    {
        static const struct { const char* name; int page; } pg[5] = {
            {"日历",0},{"待办",4},{"考点",2},{"仪表盘",5},{"二维码",6}
        };
        for (int i = 0; i < 5; i++) {
            if (strcmp(text, pg[i].name) == 0) {
                currentPage = pg[i].page; refreshPageData(); renderScreen(false);
                Serial.printf("[CMD] 切页(直呼):%s\n", pg[i].name);
                return true;
            }
        }
    }
    return false;
}

// 停止录音并保存为 WAV（新增语音待办条目）
void saveRecording() {
    isRecordingNow = false;
    if (!recMicReady) return;
    recMicReady = false;

    // 释放麦克风 codec，避免与喇叭(共用 GPIO45/时钟)冲突
    M5.Mic.end();

    // 停止录音：回填 WAV 头 data 大小并关闭文件（流式写入已完成）
    // recWritePos 是已写入 SD 的单声道字节数（processRecording 每块写 REC_CHUNK_SAMPLES*2 字节）
    // dataBytes = recWritePos
    if (recWavFile) {
        uint32_t dataBytes = recWritePos;
        uint32_t chunkSize = 36 + dataBytes;
        uint8_t buf[4];
        // WAV 头布局：RIFF(0-3) / chunkSize(4-7) / WAVE(8-11) / ... / data(36-39) / dataSize(40-43)
        // 必须 seek 到正确偏移再写，否则覆盖 "RIFF"/"data" 标签导致 BAD_HEADER！
        memcpy(buf, &chunkSize, 4);
        recWavFile.seek(4);        // RIFF chunkSize @ offset 4
        recWavFile.write(buf, 4);
        memcpy(buf, &dataBytes, 4);
        recWavFile.seek(40);       // data chunk size @ offset 40
        recWavFile.write(buf, 4);
        recWavFile.close();
        recWavFile = File();
    }

    Serial.printf("[REC] recWritePos=%u samples=%u\n", (unsigned)recWritePos, (unsigned)(recWritePos / 2));

    int monoLen = recWritePos / 2;   // 单声道样本数（实际写入量）
    if (monoLen < REC_SAMPLE_RATE / 4) {   // 少于 0.25 秒视为无效
        snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[录音太短]");
        memoryNoticeTime = millis();
        Serial.printf("[REC] 录音太短 %d samples\n", monoLen);
        renderScreen(false);
        return;
    }

    // 新增语音待办条目
    char path[64];
    snprintf(path, sizeof(path), "/record/todo_%d.wav", (int)todoCount);
    strncpy(todoItems[todoCount].text, "语音备忘", sizeof(todoItems[todoCount].text) - 1);
    todoItems[todoCount].text[sizeof(todoItems[todoCount].text) - 1] = '\0';
    strncpy(todoItems[todoCount].audioFile, path, sizeof(todoItems[todoCount].audioFile) - 1);
    todoItems[todoCount].audioFile[sizeof(todoItems[todoCount].audioFile) - 1] = '\0';
    // 记录录音日期时间和时长
    auto rdt = M5.Rtc.getDateTime();
    snprintf(todoItems[todoCount].recTime, sizeof(todoItems[todoCount].recTime),
             "%02d-%02d %02d:%02d", rdt.date.month, rdt.date.date, rdt.time.hours, rdt.time.minutes);
    todoItems[todoCount].recSec = (uint16_t)((recWritePos / 2) / REC_SAMPLE_RATE);  // 单声道秒数
    todoItems[todoCount].done = false;
    todoItems[todoCount].highPriority = false;
    todoCount++;
    todoIndex = todoCount - 1;
    snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[已保存语音]");

    // 写回 /todo.txt 持久化（否则重启后录音待办丢失；语音关联用 @ 前缀保留）
    saveTodoListToSD();

    // 语音转文字：把 WAV 读入 PSRAM 内存后排队后台任务转写（此刻 SPI 空闲，避免与 EPD 刷新共享总线冲突）
    if (strlen(sfApiKey) > 0) {
        File rf = SD.open(path, FILE_READ);
        if (rf) {
            size_t sz = rf.size();
            if (sz >= 44 && sz <= 2 * 1024 * 1024UL) {
                uint8_t* ab = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
                if (ab) {
                    asrAudioLen = rf.read(ab, sz);
                    asrAudioBuf = ab;
                    asrPending = true;
                    asrPendingIdx = (int)todoIndex;
                    Serial.printf("[ASR] 已读入 %d 字节，排队转写\n", (int)asrAudioLen);
                }
            }
            rf.close();
        } else {
            Serial.println("[ASR] 读取录音失败，仅保留录音");
        }
    }

    // 录音结束提示音（滴一声，确认已保存）
    delay(50);
    M5.Speaker.begin();
    M5.Speaker.tone(1046, 120);
    delay(200);
    M5.Speaker.stop();

    memoryNoticeTime = millis();
    renderScreen(false);
}

// 回放当前待办的语音 WAV（若该条目有关联录音文件）
void playCurrentTodoVoice() {
    if (currentPage != 4 || todoCount == 0) return;
    if (voicePlaying) { stopVoicePlayback(); return; }   // 修复S1：播放中再次长按A先停止，避免释放正在播放的缓冲(UAF)
    if (todoItems[todoIndex].audioFile[0] == '\0') {
        snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[无语音]");
        memoryNoticeTime = millis();
        return;
    }
    // 长按A = 进入详情模式（看全文 + 回放原音），短按C 返回列表
    todoDetailMode = true;
    Serial.printf("[PLAY] 长按A触发回放 todoIndex=%d file=%s\n", (int)todoIndex, todoItems[todoIndex].audioFile);
    if (!initSDCard() || !SD.exists(todoItems[todoIndex].audioFile)) {
        snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[语音文件缺失]");
        memoryNoticeTime = millis();
        return;
    }
    File f = SD.open(todoItems[todoIndex].audioFile, FILE_READ);
    if (!f) return;
    size_t fsize = f.size();
    // 上限放宽：1分钟录音≈1.9MB，用 PSRAM 分配（内部 RAM 仅~300KB 会 malloc 失败）
    if (fsize == 0 || fsize > 6 * 1024 * 1024) { f.close(); return; }
    uint8_t* wavData = (uint8_t*)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
    if (!wavData) wavData = (uint8_t*)malloc(fsize);
    if (!wavData) { f.close(); return; }
    f.read(wavData, fsize);
    f.close();

    // 回放前若麦克风占用则先释放（共用 GPIO45/时钟，避免冲突）
    if (M5.Mic.isEnabled()) M5.Mic.end();
    M5.Speaker.begin();
    M5.Speaker.stop();                    // 先停掉可能正在播放的上一段

    // 非阻塞回放：启动播放后立即返回，loop 轮询 isPlaying + 任意键可停止
    // （之前阻塞式 while 等 60 秒，录音中/播放中按键全被吞，用户无法打断）
    if (voicePlayBuf) free(voicePlayBuf);
    voicePlayBuf = wavData;
    voicePlayStart = millis();
    M5.Speaker.playWav(voicePlayBuf, fsize);
    voicePlaying = true;   // playWav 之后再置位，避免 isPlaying 时序误判
    Serial.printf("[PLAY] 非阻塞启动 fsize=%u isPlaying=%d\n", (unsigned)fsize, (int)M5.Speaker.isPlaying());

    snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[回放 %d/%d 按A/C停止]",
             (int)todoIndex + 1, (int)todoCount);
    memoryNoticeTime = millis();
    renderScreen(false);   // 长按A = 进入详情模式并刷新显示转写全文（回放原音同步进行）
}

// 停止回放并释放缓冲（loop 或按键调用）
void stopVoicePlayback() {
    if (!voicePlaying) return;
    voicePlaying = false;
    M5.Speaker.stop();
    if (voicePlayBuf) {
        free(voicePlayBuf);
        voicePlayBuf = nullptr;
    }
}

// 删除当前待办（语音待办同时删除 SD 上的 WAV 文件），并写回 /todo.txt
void deleteCurrentTodo() {
    if (todoCount == 0) return;
    todoDetailMode = false;   // 删除后退出详情模式
    // 删除关联语音文件（仅当该条目是语音待办且有文件）
    if (todoItems[todoIndex].audioFile[0] != '\0') {
        if (initSDCard() && SD.exists(todoItems[todoIndex].audioFile)) {
            SD.remove(todoItems[todoIndex].audioFile);
        }
    }
    // 前移数组删除该条目
    for (size_t i = todoIndex; i + 1 < todoCount; i++) {
        todoItems[i] = todoItems[i + 1];
    }
    todoCount--;
    if (todoIndex >= todoCount && todoCount > 0) todoIndex = todoCount - 1;
    if (todoCount == 0) todoIndex = 0;
    saveTodoListToSD();   // 写回 /todo.txt 持久化（保留其余语音关联）
    snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[已删除待办]");
    memoryNoticeTime = millis();
    renderScreen(false);
}

void loadMemoryStates() {
    prefs.begin("kp_mem", true);
    for (size_t i = 0; i < cardTotal(); i++) {
        char key[16];
        snprintf(key, sizeof(key), "s_%d", (int)i);
        kpState[i].status = prefs.getUChar(key, 0);
    }
    prefs.end();
}

void saveMemoryState(size_t index, uint8_t status) {
    if (index >= cardTotal()) return;
    kpState[index].status = status;
    prefs.begin("kp_mem", false);
    char key[16];
    snprintf(key, sizeof(key), "s_%d", (int)index);
    prefs.putUChar(key, status);
    prefs.end();
}

// 自研按键检测：直接读 GPIO（已确认 BtnA=GPIO10, BtnB=GPIO9, BtnC=GPIO1，按下=低电平）
// 并补充 M5Unified 的 isPressed()（OR 合并），双保险。
// 相比 wasClicked（瞬时状态会被刷新阻塞吞掉），电平边沿检测更可靠。
void initCustomButtons() {
    // 显式配置为输入上拉：M5Unified 把 GPIO1/9/10 配为 input（无拉），
    // 浮空输入电平不稳定会导致误读。上拉后空闲=高，按下=低（!digitalRead=true）。
    pinMode(BTN_A_GPIO, INPUT_PULLUP);
    pinMode(BTN_B_GPIO, INPUT_PULLUP);
    pinMode(BTN_C_GPIO, INPUT_PULLUP);
    keyA.prevLevel = !digitalRead(BTN_A_GPIO);
    keyB.prevLevel = !digitalRead(BTN_B_GPIO);
    keyC.prevLevel = !digitalRead(BTN_C_GPIO);
}

static void scanKey(KeyState& k, bool pressed) {
    k.clickPending = false;
    k.holdPending = false;

    k.level = pressed;

    if (pressed && !k.prevLevel) {
        // 刚按下
        k.pressMs = millis();
        k.holdFired = false;
    } else if (pressed && k.prevLevel) {
        // 保持中
        if (!k.holdFired && (millis() - k.pressMs >= BTN_HOLD_MS)) {
            k.holdFired = true;
            if (!k.suppressUntilRelease) k.holdPending = true;   // 唤醒键长按不触发
        }
    } else if (!pressed && k.prevLevel) {
        // 刚释放
        if (!k.holdFired && !k.suppressUntilRelease) {
            k.clickPending = true;  // 短按=点击（唤醒键松手不触发）
        }
        k.suppressUntilRelease = false;   // 松手后解除抑制
    }
    k.prevLevel = pressed;
}

// 每次 loop 顶部调用（M5.update() 之后），读取三键电平并更新事件
// 按键检测：仅用 GPIO 直读（按下=低，已配置 INPUT_PULLUP 上拉）。
// 重要：不能用 M5.BtnX.isPressed() 做 OR 合并——M5Unified 内部按键状态机
// 会因长按保持而持续返回 true，导致 rawX 恒为"按下"，短按/长按判定全部错乱！
void updateCustomButtons() {
    bool a = (!digitalRead(BTN_A_GPIO));
    bool b = (!digitalRead(BTN_B_GPIO));
    bool c = (!digitalRead(BTN_C_GPIO));
    scanKey(keyA, a);
    scanKey(keyB, b);
    scanKey(keyC, c);
}

// ====================================================================
// 六、UTF-8 字符自动换行渲染引擎 (大字号适配)
// ====================================================================

inline uint8_t getUtf8CharLen(uint8_t c) {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

inline bool isAsciiWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool isLineStartForbiddenPunct(const char* charBuf) {
    if (!charBuf) return false;
    if (charBuf[1] == '\0') {
        char c = charBuf[0];
        return (c == ',' || c == '.' || c == '!' || c == '?' || c == ';' || c == ':' || c == ')' || c == ']' || c == '}');
    }
    uint8_t c0 = (uint8_t)charBuf[0];
    uint8_t c1 = (uint8_t)charBuf[1];
    uint8_t c2 = (uint8_t)charBuf[2];
    if (c0 == 0xEF) {
        if (c1 == 0xBC) {
            if (c2 == 0x8C || c2 == 0x81 || c2 == 0x9B || c2 == 0x9A || c2 == 0x9F || c2 == 0x89) return true;
        }
        if (c1 == 0xBD && c2 == 0x9D) return true;
    }
    if (c0 == 0xE3 && c1 == 0x80 && (c2 == 0x81 || c2 == 0x82)) return true;
    if (c0 == 0xE2 && c1 == 0x80 && (c2 == 0x9D || c2 == 0x99)) return true;
    return false;
}

int drawWrappedText(const char* text, int x, int y, int maxWidth, int maxLines, int lineHeight, uint32_t textColor = TFT_BLACK, size_t startOffset = 0, size_t* pConsumed = nullptr) {
    if (!text || strlen(text) == 0 || maxWidth <= 0 || maxLines <= 0) return 0;
    size_t len = strlen(text);
    if (startOffset >= len) { if (pConsumed) *pConsumed = 0; return 0; }

    int currentX = x;
    int currentY = y;
    int lineCount = 1;
    size_t i = startOffset;

    M5.Display.setTextWrap(false);
    M5.Display.setTextColor(textColor);

    while (i < len && lineCount <= maxLines) {
        if (text[i] == '\r') {
            i++;
            continue;
        }

        if (text[i] == '\n') {
            currentX = x;
            currentY += lineHeight;
            lineCount++;
            i++;
            continue;
        }

        if (isAsciiWordChar(text[i])) {
            size_t wordStart = i;
            size_t wordLen = 0;
            while (wordStart + wordLen < len && isAsciiWordChar(text[wordStart + wordLen])) {
                wordLen++;
            }

            char wordBuf[64];
            size_t copyLen = wordLen < sizeof(wordBuf) - 1 ? wordLen : sizeof(wordBuf) - 1;
            memcpy(wordBuf, text + wordStart, copyLen);
            wordBuf[copyLen] = '\0';

            int wordWidth = M5.Display.textWidth(wordBuf);

            if (currentX > x && currentX + wordWidth > x + maxWidth) {
                currentX = x;
                currentY += lineHeight;
                lineCount++;
                if (lineCount > maxLines) {
                    M5.Display.setCursor(x + maxWidth - M5.Display.textWidth("..."), currentY - lineHeight);
                    M5.Display.print("...");
                    break;
                }
            }
        }

        uint8_t charLen = getUtf8CharLen((uint8_t)text[i]);
        if (i + charLen > len) break;

        char charBuf[5] = {0};
        memcpy(charBuf, text + i, charLen);

        int charWidth = M5.Display.textWidth(charBuf);

        bool needWrap = (currentX > x && currentX + charWidth > x + maxWidth);
        if (!needWrap && currentX > x && (currentX + charWidth + 14 > x + maxWidth) && (i + charLen < len)) {
            uint8_t nextLen = getUtf8CharLen((uint8_t)text[i + charLen]);
            if (i + charLen + nextLen <= len) {
                char nextBuf[5] = {0};
                memcpy(nextBuf, text + i + charLen, nextLen);
                if (isLineStartForbiddenPunct(nextBuf)) {
                    needWrap = true;
                }
            }
        }

        if (needWrap) {
            currentX = x;
            currentY += lineHeight;
            lineCount++;
            if (lineCount > maxLines) {
                M5.Display.setCursor(x + maxWidth - M5.Display.textWidth("..."), currentY - lineHeight);
                M5.Display.print("...");
                break;
            }
        }

        M5.Display.setCursor(currentX, currentY);
        M5.Display.print(charBuf);

        currentX += charWidth;
        i += charLen;
    }

    if (pConsumed) *pConsumed = i - startOffset;   // 本页实际消耗字符数（供正文分页）
    return (lineCount * lineHeight);
}

// ====================================================================
// 七、外设与时间计算
// ====================================================================

void readSHT40() {
    Wire.beginTransmission(0x44);
    Wire.write(0xFD);
    if (Wire.endTransmission() == 0) {
        delay(10);
        if (Wire.requestFrom(0x44, 6) == 6) {
            uint16_t t_raw = (Wire.read() << 8) | Wire.read();
            Wire.read();
            uint16_t rh_raw = (Wire.read() << 8) | Wire.read();
            Wire.read();
            currentTemp = -45.0f + 175.0f * ((float)t_raw / 65535.0f);
            currentHum = -6.0f + 125.0f * ((float)rh_raw / 65535.0f);
            if (currentHum < 0.0f) currentHum = 0.0f;
            if (currentHum > 100.0f) currentHum = 100.0f;
            hasSHT40 = true;
        }
    }
}

long julianDay(int y, int m, int d);   // 前向声明（getDaysToExam 调用，定义在其后）
int getDaysToExam() {
    auto dt = M5.Rtc.getDateTime();
    int currentYear = dt.date.year < 2024 ? 2026 : dt.date.year;
    // 用儒略日差值精确计算（原近似算法偏差可达数天）
    int remain = (int)(julianDay(EXAM_YEAR, EXAM_MONTH, EXAM_DAY)
                     - julianDay(currentYear, dt.date.month, dt.date.date));
    if (remain < 0) remain = 0;
    return remain;
}

// ====================================================================
// 黄历/天气数据（离线简化版，支持从 SD 卡读取 /calendar.txt 覆盖）
// 注：完整联网黄历/天气需外部 API，本版以确定性离线数据 + SD 覆盖为准
// ====================================================================
// 公历转儒略日
long julianDay(int y, int m, int d) {
    if (m <= 2) { y--; m += 12; }
    return 365L * y + y / 4 - y / 100 + y / 400 + (153L * m - 457) / 5 + d - 306;
}

// 精确农历/干支/黄历宜忌（数据表 1900-2100，离线准确，lunar_cal.h）
void lunarCalc(int year, int month, int day, char* lunarOut, size_t outLen, char* ganzhiOut, size_t gzLen) {
    int ly, lm, ld; bool leap;
    solarToLunar(year, month, day, ly, lm, ld, leap);
    // 农历文本（如"六月初八" / "闰四月十五"）
    if (leap) snprintf(lunarOut, outLen, "闰%s月%s", LUNAR_MONTH_CN[lm], LUNAR_DAY_CN[ld - 1]);
    else snprintf(lunarOut, outLen, "%s月%s", LUNAR_MONTH_CN[lm], LUNAR_DAY_CN[ld - 1]);
    // 年干支 + 生肖
    int gy = (ly - 4) % 10; if (gy < 0) gy += 10;
    int gz = (ly - 4) % 12; if (gz < 0) gz += 12;
    snprintf(ganzhiOut, gzLen, "%s%s年 属%s", LUNAR_GAN[gy], LUNAR_ZHI[gz], LUNAR_SHENG[gz]);
    // 黄历宜忌（建除十二神，真实对应日期，每天不同）
    int dseq = dayGanzhiSeq(year, month, day);
    int jian = calcJianChu(lm, dseq % 12);
    snprintf(calYi, sizeof(calYi), "宜：%s", JIANCHU_YI[jian]);
    snprintf(calJi, sizeof(calJi), "忌：%s", JIANCHU_JI[jian]);
}

void updateCalendarData() {
    auto dt = M5.Rtc.getDateTime();
    int y = dt.date.year;
    int m = dt.date.month;
    int d = dt.date.date + calOffset;
    if (y < 2024) { y = 2026; m = 8; d = 9 + calOffset; }
    // 日期退位（跨月/跨年，支持看前一天）
    while (d < 1) {
        m--;
        if (m < 1) { m = 12; y--; }
        if (m == 2) d = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 29 : 28;
        else if (m == 4 || m == 6 || m == 9 || m == 11) d = 30;
        else d = 31;
    }
    // 日期进位简化（跨月/跨年简化处理）
    if (m == 2) {
        bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        if (d > (leap ? 29 : 28)) { d = 1; m++; }
    } else if (d > 31 || (m == 4 || m == 6 || m == 9 || m == 11) && d > 30) {
        d = 1; m++;
        if (m > 12) { m = 1; y++; }
    }
    lunarCalc(y, m, d, calLunar, sizeof(calLunar), calGanzhi, sizeof(calGanzhi));

    // 天气数据由 fetchWeather() 联网拉取（Open-Meteo 真实数据），此处不伪造
    // 从 SD 卡读取 /calendar.txt 覆盖（格式: 农历|干支|宜|忌|天气|高|低）
    if (initSDCard() && SD.exists("/calendar.txt")) {
        File f = SD.open("/calendar.txt", FILE_READ);
        if (f) {
            String s = f.readString();
            f.close();
            int p = s.indexOf('|');
            if (p > 0) { s.substring(0, p).toCharArray(calLunar, sizeof(calLunar)); s = s.substring(p + 1); }
            p = s.indexOf('|');
            if (p > 0) { s.substring(0, p).toCharArray(calGanzhi, sizeof(calGanzhi)); s = s.substring(p + 1); }
            p = s.indexOf('|');
            if (p > 0) { snprintf(calYi, sizeof(calYi), "宜：%s", s.substring(0, p).c_str()); s = s.substring(p + 1); }
            p = s.indexOf('|');
            if (p > 0) { snprintf(calJi, sizeof(calJi), "忌：%s", s.substring(0, p).c_str()); s = s.substring(p + 1); }
            p = s.indexOf('|');
            if (p > 0) { s.substring(0, p).toCharArray(weatherText, sizeof(weatherText)); s = s.substring(p + 1); }
            weatherHigh = s.toInt();
            p = s.indexOf('|');
            if (p > 0) weatherLow = s.substring(p + 1).toInt();
        }
    }
}

// ===== 真实天气：Open-Meteo 免费 API（无需 key） =====
// WMO 天气代码 → 中文
static const char* wmoCodeToText(int code) {
    if (code == 0) return "晴";
    if (code <= 2) return "多云";
    if (code == 3) return "阴";
    if (code <= 48) return "雾";
    if (code <= 57) return "小雨";
    if (code <= 67) return "中雨";
    if (code <= 77) return "雪";
    if (code <= 82) return "阵雨";
    if (code <= 86) return "雨夹雪";
    if (code <= 99) return "雷阵雨";
    return "多云";
}

// 拉取北京（默认坐标）今日/明日真实天气，填 weatherText/weatherHigh/Low + 明日
void fetchWeather() {
    bool ok = false;
    do {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[WX] WiFi未连接 status=%d\n", (int)WiFi.status());
            break;
        }
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(8);
        if (!client.connect("api.open-meteo.com", 443)) {
            char errBuf[64];
            client.lastError(errBuf, sizeof(errBuf));
            IPAddress ip;
            bool dnsOk = WiFi.hostByName("api.open-meteo.com", ip);
            Serial.printf("[WX] connect失败 err=%s dns=%d%s\n", errBuf, (int)dnsOk, dnsOk ? (" ip=" + ip.toString()).c_str() : "");
            break;
        }
        // HTTP/1.0：避免服务器返回 Transfer-Encoding: chunked（1.1 特性），直接收完整 JSON body
        // 坐标走 config（weather_lat/weather_lon），默认北京；snprintf 拼 URL（Print::printf 的 %f 实测畸形，勿用）
        char wxUrl[200];
        snprintf(wxUrl, sizeof(wxUrl),
                 "GET /v1/forecast?latitude=%.4f&longitude=%.4f&daily=weathercode,temperature_2m_max,temperature_2m_min&timezone=Asia%%2FShanghai&past_days=1&forecast_days=4 HTTP/1.0",
                 weatherLat, weatherLon);
        client.println(wxUrl);
        client.println("Host: api.open-meteo.com");
        client.println("User-Agent: Mozilla/5.0");
        client.println("Accept-Encoding: identity");
        client.println();

        const int BUF = 4096;
        static char* wbuf = nullptr;
        if (!wbuf) wbuf = (char*)heap_caps_malloc(BUF, MALLOC_CAP_SPIRAM);
        if (!wbuf) { Serial.printf("[WX] PSRAM分配失败\n"); break; }
        int pos = 0;
        unsigned long t0 = millis();
        while (client.connected() && millis() - t0 < 10000 && pos < BUF - 1) {
            while (client.available() && pos < BUF - 1) {
                char c = client.read();
                if (c >= 0) wbuf[pos++] = c;
            }
            delay(1);
        }
        wbuf[pos] = '\0';
        client.stop();
        char* body = strstr(wbuf, "\r\n\r\n");
        if (!body) { Serial.printf("[WX] 无body 收到%d字节 buf=%.120s\n", pos, wbuf); break; }
        body += 4;

        JsonDocument doc;
        if (deserializeJson(doc, body)) { Serial.printf("[WX] 解析失败 body=%.160s\n", body); break; }
        JsonArray wc = doc["daily"]["weathercode"];
        JsonArray mx = doc["daily"]["temperature_2m_max"];
        JsonArray mn = doc["daily"]["temperature_2m_min"];
        if (wc.size() >= 5 && mx.size() >= 5 && mn.size() >= 5) {
            // 数组顺序：0=昨天 1=今天 2=明天 3=后天 4=大后天
            for (int i = 0; i < 5; i++) {
                snprintf(wxTxt[i], sizeof(wxTxt[i]), "%s", wmoCodeToText((int)wc[i]));
                wxHi[i] = (int)mx[i];
                wxLo[i] = (int)mn[i];
            }
            // 兼容今日/明日（供 TTS 播报、#STATUS）
            snprintf(weatherText, sizeof(weatherText), "%s", wxTxt[1]);
            snprintf(weatherTextTmr, sizeof(weatherTextTmr), "%s", wxTxt[2]);
            weatherHigh = wxHi[1]; weatherLow = wxLo[1];
            weatherHighTmr = wxHi[2]; weatherLowTmr = wxLo[2];
            Serial.printf("[WX] 昨%s %d°/%d° 今%s %d°/%d° 明%s %d°/%d° 后%s %d°/%d° 大后%s %d°/%d°\n",
                          wxTxt[0], wxHi[0], wxLo[0], wxTxt[1], wxHi[1], wxLo[1],
                          wxTxt[2], wxHi[2], wxLo[2], wxTxt[3], wxHi[3], wxLo[3],
                          wxTxt[4], wxHi[4], wxLo[4]);
            ok = true;
        }
    } while (false);
    weatherUpdatedAt = millis();
    weatherSuccess = ok;
}

// ====================================================================
// 八、Spectra 6 炫彩 UI 视图渲染逻辑 (大字号 + 调彩徽章)
// ====================================================================

// ====================================================================
// 静态顶栏：仅显示页面标题 + 页码/状态小标，不显示时间温度
// （时间温度仅在仪表盘页展示，避免顶栏因时间变化而要求刷新）
// ====================================================================
void drawTopBar() {
    M5.Display.fillRect(0, 0, SCREEN_WIDTH, STATUS_BAR_HEIGHT, TFT_RED);   // 深红顶栏（用户指定，恢复）
    M5.Display.setTextColor(TFT_WHITE, TFT_RED);

    const char* title = "";
    static const char* const PAGE_TITLES[] = {
        "日历黄历", "资讯早报", "考点闪卡", "Coding Plan", "语音待办", "状态仪表盘", "微信二维码"
    };
    if (currentPage < 7) title = PAGE_TITLES[currentPage];

    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.drawString(title, SCREEN_WIDTH / 2, STATUS_BAR_HEIGHT / 2);

    // 左侧小标：页码指示
    M5.Display.setFont(&fonts::efontCN_14_b);
    M5.Display.setTextDatum(textdatum_t::middle_left);
    char pageNo[16];
    snprintf(pageNo, sizeof(pageNo), "%d/%d", (int)currentPage + 1, (int)PAGE_COUNT);
    M5.Display.drawString(pageNo, MARGIN_X, STATUS_BAR_HEIGHT / 2);

    // 右侧小标：网络状态 + 电量（深红底白字；电量 0-100 或 -1 未知）
    M5.Display.setFont(&fonts::efontCN_14_b);
    M5.Display.setTextDatum(textdatum_t::middle_right);
    int bLevel = M5.Power.getBatteryLevel();
    char right[24];
    if (wifiConnected) {
        snprintf(right, sizeof(right), "WiFi %d%%", bLevel >= 0 ? bLevel : 0);
    } else {
        snprintf(right, sizeof(right), "%d%%", bLevel >= 0 ? bLevel : 0);
    }
    M5.Display.drawString(right, SCREEN_WIDTH - MARGIN_X, STATUS_BAR_HEIGHT / 2);
}

void drawActionBar() {
    int startY = STATUS_BAR_HEIGHT + MAIN_AREA_HEIGHT;

    M5.Display.fillRect(0, startY, SCREEN_WIDTH, ACTION_BAR_HEIGHT, TFT_NAVY);   // 深蓝实底，避免深绿发虚
    M5.Display.drawFastHLine(0, startY, SCREEN_WIDTH, TFT_WHITE);                 // 黄线在墨水屏几乎不可见，改白色

    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
    M5.Display.setTextDatum(textdatum_t::middle_left);

    // 每页操作提示（顺序与页面映射一致：0日历1早报2考点3CodingPlan4待办5仪表盘6二维码）
    static const char* const HINTS[] = {
        "A上一天 B明天 长按A末页 长按B报天气 长按C刷新数据",  // 日历(0)
        "A上条 B下条 长按B朗读 长按C联网刷新",          // 早报(1)
        "A上条 B下条 长按A掌握 长按B朗读 长按C强刷",    // 考点(2)
        "A上页 WiFi自动更新 长按C强刷",               // CodingPlan(3)
        "A上条 B下条 长按A看全文 长按B删除 C录音",          // 待办(4)
        "A上页 B开停番茄 长按C强刷",                  // 仪表盘(5)
        "A上页 B下页 长按C关机菜单",                  // 二维码(6)
    };
    M5.Display.drawString(HINTS[currentPage < 7 ? currentPage : 0],
                          MARGIN_X, startY + ACTION_BAR_HEIGHT / 2);

    M5.Display.setTextDatum(textdatum_t::middle_right);
    char pageBuf[64];

    if (memoryNoticeTime > 0 && millis() - memoryNoticeTime < 3000) {
        snprintf(pageBuf, sizeof(pageBuf), "%s", memoryNoticeBuf);
        M5.Display.setTextColor(TFT_RED, TFT_NAVY);   // 黄色不可见，改红
    } else if (cooldownBlocked) {
        unsigned long elapsed = millis() - lastFullRefreshTime;
        unsigned long remainSec = (elapsed < FULL_REFRESH_COOLDOWN_MS) ? (FULL_REFRESH_COOLDOWN_MS - elapsed + 999UL) / 1000UL : 0;
        snprintf(pageBuf, sizeof(pageBuf), "强刷冷却 %lus", remainSec);
        M5.Display.setTextColor(TFT_RED, TFT_NAVY);   // 黄色不可见，改红
    } else {
        switch (currentPage) {
            case 0: {
                // 显示当前查看的日期语义（配合 A/B 切换昨日/今日/明日/后天）
                const char* when = "今日";
                if (calOffset == -1) when = "昨日";
                else if (calOffset == 1) when = "明日";
                else if (calOffset == 2) when = "后天";
                snprintf(pageBuf, sizeof(pageBuf), "%s", when);
                break;
            }
            case 1:
                snprintf(pageBuf, sizeof(pageBuf), "早报 %d/%d", (int)newsIndex + 1, (int)newsTotal());   // 修复M4: 用newsTotal非NEWS_COUNT
                break;
            case 2: {
                uint8_t st = kpState[ebbinghausIndex].status;
                const char* stStr = (st == 1) ? " 已掌握" : (st == 2 ? " 重温" : "");
                snprintf(pageBuf, sizeof(pageBuf), "考点 %d/%d%s", (int)ebbinghausIndex + 1, (int)cardTotal(), stStr);
                break;
            }
            case 3:
                snprintf(pageBuf, sizeof(pageBuf), "额度 5h:%d%% 7d:%d%%",
                         cpData.quota_5h_percent, cpData.quota_7d_percent);
                break;
            case 4:
                snprintf(pageBuf, sizeof(pageBuf), "待办 %d/%d", (int)todoIndex + 1, (int)todoCount);
                break;
            case 5:
                snprintf(pageBuf, sizeof(pageBuf), "番茄 %s", pomodoroRunning ? (pomodoroInRest ? "休息中" : "专注中") : "未启动");
                break;
            case 6:
                snprintf(pageBuf, sizeof(pageBuf), "二维码 %d/%d", (int)qrIndex + 1, (int)qrCount);
                break;
        }
        M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
    }
    M5.Display.drawString(pageBuf, SCREEN_WIDTH - MARGIN_X, startY + ACTION_BAR_HEIGHT / 2);
}

void drawEbbinghausPage() {
    int startY = STATUS_BAR_HEIGHT;
    int areaW = SCREEN_WIDTH - MARGIN_X * 2;
    int leftWidth = (int)(areaW * 0.58f) - 8;    // 左: 问题区 58%（问题要突出）
    int gap = 16;
    int rightX = MARGIN_X + leftWidth + gap;
    int rightWidth = areaW - leftWidth - gap;    // 右: 答案区 42%

    // 状态圆点: 已掌握绿 / 需重温红 / 正常深蓝
    uint8_t st = kpState[ebbinghausIndex].status;
    uint32_t stColor = st == 1 ? TFT_DARKGREEN : (st == 2 ? TFT_RED : TFT_NAVY);
    const char* qCate = cardCategory(ebbinghausIndex);
    const char* qText = cardQuestion(ebbinghausIndex);
    const char* aText = cardAnswer(ebbinghausIndex);
    const char* mText = cardMnemonic(ebbinghausIndex);

    // ---- 左栏: 问题区（白底，问题文字 24 号黑色粗体，顶部状态徽章紧凑） ----
    M5.Display.fillRoundRect(MARGIN_X, startY + 8, leftWidth, MAIN_AREA_HEIGHT - 16, 14, TFT_WHITE);
    M5.Display.drawRoundRect(MARGIN_X, startY + 8, leftWidth, MAIN_AREA_HEIGHT - 16, 14, TFT_NAVY);

    // 顶部状态条：题号（大字，红）+ 状态圆点 + 科目/状态
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextColor(stColor, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::middle_left);
    char qNo[16];
    snprintf(qNo, sizeof(qNo), "Q%d", (int)ebbinghausIndex + 1);
    M5.Display.drawString(qNo, MARGIN_X + 12, startY + 26);
    M5.Display.fillCircle(MARGIN_X + 62, startY + 26, 6, stColor);
    M5.Display.setFont(&fonts::efontCN_14_b);
    M5.Display.setTextColor(st == 1 ? TFT_DARKGREEN : (st == 2 ? TFT_RED : TFT_NAVY), TFT_WHITE);
    char stText[32];
    snprintf(stText, sizeof(stText), "%s · %s", qCate,
             st == 1 ? "已掌握" : (st == 2 ? "需重温" : "学习中"));
    M5.Display.drawString(stText, MARGIN_X + 76, startY + 26);

    // 问题正文：24号粗体（库内最大稳定字号），行距48紧凑排列，突出且不缩放
    // 避免 setTextSize 缩放导致 efont 渲染错乱/重叠
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::top_left);
    drawWrappedText(qText, MARGIN_X + 14, startY + 44, leftWidth - 28, 7, 42, TFT_BLACK);

    // ---- 右栏: 答案区（白底，答案黑字 + 速记蓝色粗体标注） ----
    M5.Display.fillRoundRect(rightX, startY + 8, rightWidth, MAIN_AREA_HEIGHT - 16, 14, TFT_WHITE);
    M5.Display.drawRoundRect(rightX, startY + 8, rightWidth, MAIN_AREA_HEIGHT - 16, 14, TFT_RED);

    // 顶部标题条（深红背景，紧凑）
    M5.Display.fillRoundRect(rightX + 10, startY + 14, rightWidth - 20, 30, 8, TFT_RED);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_WHITE, TFT_RED);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.drawString("答 案", rightX + rightWidth / 2, startY + 29);

    // 答案正文: 16 号黑色粗体（右栏空间有限，16号保证不超出栏目）
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::top_left);
    drawWrappedText(aText, rightX + 12, startY + 54, rightWidth - 24, 3, 30, TFT_BLACK);

    // 分隔线（红）
    M5.Display.drawFastHLine(rightX + 12, startY + 150, rightWidth - 24, TFT_RED);

    // 速记区: 深蓝背景条 + 白色速记文字
    M5.Display.fillRoundRect(rightX + 10, startY + 158, rightWidth - 20, 28, 8, TFT_NAVY);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.drawString("速记口诀", rightX + rightWidth / 2, startY + 172);
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setFont(&fonts::efontCN_16_b);
    drawWrappedText(mText, rightX + 12, startY + 194, rightWidth - 24, 4, 26, TFT_BLUE);

    M5.Display.setTextDatum(textdatum_t::top_left);
}

void drawNewsPage() {
    const char* tag = newsTag(newsIndex);
    const char* title = newsTitle(newsIndex);
    const char* summary = newsSummary(newsIndex);

    int startY = STATUS_BAR_HEIGHT;
    int areaW = SCREEN_WIDTH - MARGIN_X * 2;

    // 分类色块（仅红/黄/蓝三色深色块）：政策红 / 宏观数据黄 / 行业AI蓝
    uint32_t tagColor = TFT_BLUE;
    if (strstr(tag, "政策")) tagColor = TFT_RED;
    else if (strstr(tag, "宏观") || strstr(tag, "数据")) tagColor = TFT_ORANGE;   // 黄块墨水屏太淡，改橙
    else if (strstr(tag, "行业") || strstr(tag, "AI")) tagColor = TFT_BLUE;

    // ---- 顶部: 分类标签条（深色背景 + 反白粗体） Y:50~80 ----
    M5.Display.fillRoundRect(MARGIN_X, startY + 8, areaW, 30, 8, tagColor);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_WHITE, tagColor);
    M5.Display.setTextDatum(textdatum_t::middle_left);
    char tagBuf[40];
    snprintf(tagBuf, sizeof(tagBuf), "%s  早报 %d/%d", tag, (int)newsIndex + 1, newsTotal());
    M5.Display.drawString(tagBuf, MARGIN_X + 26, startY + 23);

    // ---- 标题区: 白底黑字 24号粗体（最多2行） Y:86~156 ----
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    drawWrappedText(title, MARGIN_X + 8, startY + 44, areaW - 16, 2, 35, TFT_BLACK);

    // 分隔线（红） Y:160
    M5.Display.drawFastHLine(MARGIN_X + 8, startY + 118, areaW - 16, TFT_RED);

    // ---- 快讯正文区: 16号黑色粗体 短讯（首句，最多4行，Y:170~278） ----
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    drawWrappedText(summary, MARGIN_X + 8, startY + 128, areaW - 16, 4, 27, TFT_BLACK);

    // ---- 底部: 下一条早报预览条 Y:300~336 ----
    int previewY = startY + MAIN_AREA_HEIGHT - 38;
    M5.Display.fillRoundRect(MARGIN_X, previewY, areaW, 36, 8, TFT_NAVY);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
    M5.Display.setTextDatum(textdatum_t::middle_left);
    M5.Display.drawString("下条", MARGIN_X + 12, previewY + 18);
    size_t nextIdx = (newsIndex + 1) % newsTotal();
    char pBuf[128];
    snprintf(pBuf, sizeof(pBuf), "[%s] %s", newsTag(nextIdx), newsTitle(nextIdx));
    M5.Display.drawString(pBuf, MARGIN_X + 104, previewY + 18);

    M5.Display.setTextDatum(textdatum_t::top_left);
}

// ====================================================================
// 页面 2: 状态仪表盘 (2×2 四象限: 环境/设备/番茄/备考)
// ====================================================================
void drawDashboardPage() {
    int startY = STATUS_BAR_HEIGHT;
    int pad = 14;
    int qw = (SCREEN_WIDTH - MARGIN_X * 2 - pad) / 2;   // 每象限宽
    int qh = (MAIN_AREA_HEIGHT - pad * 2 - pad) / 2;    // 每象限高

    int x0 = MARGIN_X, y0 = startY + pad;

    // ---- 左上: 环境监测 (SHT40) ----
    int x = x0, y = y0;
    M5.Display.fillRoundRect(x, y, qw, qh, 10, TFT_WHITE);
    M5.Display.drawRoundRect(x, y, qw, qh, 10, TFT_NAVY);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.drawString("环境监测", x + 12, y + 10);
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextDatum(textdatum_t::middle_left);
    bool comf = (currentTemp >= 20 && currentTemp <= 26) && (currentHum >= 40 && currentHum <= 70);
    uint32_t tc = comf ? TFT_NAVY : TFT_RED;
    M5.Display.setTextColor(tc, TFT_WHITE);
    char tBuf[32];
    snprintf(tBuf, sizeof(tBuf), "%.1f℃", currentTemp);
    M5.Display.drawString(tBuf, x + 12, y + 62);
    M5.Display.setTextColor(tc, TFT_WHITE);
    snprintf(tBuf, sizeof(tBuf), "%.0f%%RH", currentHum);
    M5.Display.drawString(tBuf, x + 12, y + 100);
    M5.Display.setFont(&fonts::efontCN_14_b);
    M5.Display.setTextColor(comf ? TFT_NAVY : TFT_RED, TFT_WHITE);
    M5.Display.drawString(comf ? "舒适" : "注意", x + qw - 50, y + 62);

    // ---- 右上: 设备状态 ----
    x = x0 + qw + pad; y = y0;
    M5.Display.fillRoundRect(x, y, qw, qh, 10, TFT_WHITE);
    M5.Display.drawRoundRect(x, y, qw, qh, 10, TFT_NAVY);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.drawString("设备状态", x + 12, y + 10);
    M5.Display.setFont(&fonts::efontCN_16_b);
    int yPos = y + 48;
    // 电量（PaperColor 电池经 PMIC 读取真实百分比）
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    int bLevel = M5.Power.getBatteryLevel();
    if (bLevel >= 0) {
        char bbuf[24];
        snprintf(bbuf, sizeof(bbuf), "电池：%d%%", bLevel);
        M5.Display.drawString(bbuf, x + 12, yPos);
    } else {
        M5.Display.drawString("电池：在线", x + 12, yPos);
    }
    yPos += 32;
    // WiFi 信号
    M5.Display.setTextColor(wifiConnected ? TFT_NAVY : TFT_RED, TFT_WHITE);
    M5.Display.drawString(wifiConnected ? "WiFi：已连接" : "WiFi：离线", x + 12, yPos); yPos += 32;
    // SD 卡
    M5.Display.setTextColor(hasSDCard ? TFT_NAVY : TFT_RED, TFT_WHITE);
    M5.Display.drawString(hasSDCard ? "SD卡：就绪" : "SD卡：无", x + 12, yPos);

    // ---- 左下: 专注番茄 ----
    x = x0; y = y0 + qh + pad;
    M5.Display.fillRoundRect(x, y, qw, qh, 10, TFT_WHITE);
    M5.Display.drawRoundRect(x, y, qw, qh, 10, TFT_RED);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.drawString("专注番茄", x + 12, y + 10);
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextDatum(textdatum_t::middle_left);
    M5.Display.setTextColor(pomodoroRunning ? TFT_RED : TFT_BLACK, TFT_WHITE);
    char pBuf[32];
    snprintf(pBuf, sizeof(pBuf), "%d 分钟", (int)pomodoroRemainMin);
    M5.Display.drawString(pBuf, x + 12, y + 62);
    M5.Display.setFont(&fonts::efontCN_14_b);
    M5.Display.setTextColor(pomodoroRunning ? (pomodoroInRest ? TFT_RED : TFT_NAVY) : TFT_NAVY, TFT_WHITE);
    M5.Display.drawString(pomodoroRunning ? (pomodoroInRest ? "休息中·按B暂停" : "专注中·按B暂停") : "未启动·按B开始",
                          x + 12, y + 100);
    // 今日完成次数（简化：从状态统计）
    char doneBuf[48];
    snprintf(doneBuf, sizeof(doneBuf), "今日完成 %d 个", pomodoroDoneCount);
    M5.Display.setFont(&fonts::efontCN_14_b);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.drawString(doneBuf, x + 12, y + 130);

    // ---- 右下: 备考进度 ----
    x = x0 + qw + pad; y = y0 + qh + pad;
    M5.Display.fillRoundRect(x, y, qw, qh, 10, TFT_WHITE);
    M5.Display.drawRoundRect(x, y, qw, qh, 10, TFT_NAVY);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.drawString("备考进度", x + 12, y + 10);
    M5.Display.setFont(&fonts::efontCN_16_b);
    int mastered = 0;
    for (size_t i = 0; i < cardTotal(); i++) if (kpState[i].status == 1) mastered++;
    char progBuf[48];
    snprintf(progBuf, sizeof(progBuf), "已掌握 %d/%d", mastered, (int)cardTotal());
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.drawString(progBuf, x + 12, y + 48);
    int days = getDaysToExam();
    snprintf(progBuf, sizeof(progBuf), "距考试 %d 天", days);
    M5.Display.setTextColor(days < 30 ? TFT_RED : TFT_NAVY, TFT_WHITE);
    M5.Display.drawString(progBuf, x + 12, y + 82);
    snprintf(progBuf, sizeof(progBuf), "今日复习 %d", todayReviewedCount);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.drawString(progBuf, x + 12, y + 116);

    M5.Display.setTextDatum(textdatum_t::top_left);
}

// ====================================================================
// 页面 3: 语音速记待办 (黑白粗体列表 + 优先级圆点)
// ====================================================================
// 纯计算：把文字按 maxWidth(px) 切成行，返回行起始字符偏移到 lineStarts（最多 maxRows 行），返回行数
// 与 drawWrappedText 换行规则一致（UTF-8 / ASCII 词 / 行首禁标点），用于详情分页
static int wrapTextLines(const char* text, int maxWidth, size_t* lineStarts, int maxRows) {
    if (!text || maxWidth <= 0 || maxRows <= 0) return 0;
    size_t len = strlen(text);
    int rows = 0;
    size_t lineStart = 0;
    size_t i = 0;
    int currentX = 0;
    M5.Display.setFont(&fonts::efontCN_24_b);
    while (i < len && rows < maxRows) {
        if (text[i] == '\r') { i++; continue; }
        if (text[i] == '\n') {
            if (rows < maxRows) lineStarts[rows++] = lineStart;
            lineStart = i + 1; currentX = 0; i++; continue;
        }
        if (isAsciiWordChar(text[i])) {
            size_t ws = i, wl = 0;
            while (ws + wl < len && isAsciiWordChar(text[ws + wl])) wl++;
            char wb[64]; size_t cl = wl < 63 ? wl : 63;
            memcpy(wb, text + ws, cl); wb[cl] = '\0';
            if (currentX > 0 && currentX + (int)M5.Display.textWidth(wb) > maxWidth) {
                if (rows < maxRows) lineStarts[rows++] = lineStart;
                lineStart = i; currentX = 0;
            }
        }
        uint8_t clen = getUtf8CharLen((uint8_t)text[i]);
        if (i + clen > len) break;
        char cb[5] = {0}; memcpy(cb, text + i, clen);
        int cw = M5.Display.textWidth(cb);
        bool needWrap = (currentX > 0 && currentX + cw > maxWidth);
        if (!needWrap && currentX > 0 && (currentX + cw + 14 > maxWidth) && (i + clen < len)) {
            uint8_t nlen = getUtf8CharLen((uint8_t)text[i + clen]);
            if (i + clen + nlen <= len) {
                char nb[5] = {0}; memcpy(nb, text + i + clen, nlen);
                if (isLineStartForbiddenPunct(nb)) needWrap = true;
            }
        }
        if (needWrap) {
            if (rows < maxRows) lineStarts[rows++] = lineStart;
            lineStart = i; currentX = 0;
        }
        currentX += cw;
        i += clen;
    }
    if (rows < maxRows && lineStart < len) lineStarts[rows++] = lineStart;
    return rows;
}

void drawTodoPage() {
    int startY = STATUS_BAR_HEIGHT;
    int areaW = SCREEN_WIDTH - MARGIN_X * 2;
    M5.Display.fillRect(MARGIN_X, startY, areaW, MAIN_AREA_HEIGHT, TFT_WHITE);

    // 列表区（上方留 40px 给底部录音按钮）
    int listH = MAIN_AREA_HEIGHT - 52;

    if (todoCount == 0 && !isRecordingNow) {
        M5.Display.setFont(&fonts::efontCN_24_b);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        M5.Display.setTextDatum(textdatum_t::middle_center);
        M5.Display.drawString("暂无待办", SCREEN_WIDTH / 2, startY + 60);
        M5.Display.setFont(&fonts::efontCN_16_b);
        M5.Display.drawString("SD 卡 /todo.txt 每行一条", SCREEN_WIDTH / 2, startY + 110);
        M5.Display.drawString("前缀 ! = 高优先级  # = 已完成", SCREEN_WIDTH / 2, startY + 140);
        M5.Display.setTextDatum(textdatum_t::top_left);
    } else if (todoDetailMode && todoItems[todoIndex].audioFile[0] != '\0' && todoItems[todoIndex].asr[0] != '\0') {
        // ===== 语音待办详情模式（长按A进入）：转写全文分页 / 长按A回放 / 短按C返回 =====
        const TodoItem& it = todoItems[todoIndex];
        int x = MARGIN_X + 8;
        int dW = areaW - 16;
        // 标题 + 时间（右侧）
        M5.Display.setFont(&fonts::efontCN_24_b);
        M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
        M5.Display.setTextDatum(textdatum_t::top_left);
        M5.Display.drawString("语音待办", x, startY + 8);
        M5.Display.setFont(&fonts::efontCN_14_b);
        M5.Display.setTextDatum(textdatum_t::middle_right);
        char tm[40];
        if (it.recTime[0]) snprintf(tm, sizeof(tm), "%s · %ds", it.recTime, (int)it.recSec);
        else snprintf(tm, sizeof(tm), "%ds", (int)it.recSec);
        M5.Display.drawString(tm, x + dW, startY + 26);
        M5.Display.setTextDatum(textdatum_t::top_left);
        M5.Display.drawFastHLine(x, startY + 42, dW, TFT_RED);
        // 转写全文（24号分页，每页6行，A/B翻页）
        {
            size_t rowStarts[64];
            int rows = wrapTextLines(it.asr, dW - 16, rowStarts, 64);
            asrPages = (rows + 5) / 6;
            if (asrPages < 1) asrPages = 1;
            if (asrPageIdx >= asrPages) asrPageIdx = asrPages - 1;
            if (asrPageIdx < 0) asrPageIdx = 0;
            int sr = asrPageIdx * 6;
            M5.Display.setFont(&fonts::efontCN_24_b);
            M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
            drawWrappedText(it.asr, x, startY + 58, dW - 16, 6, 34, TFT_BLACK, rowStarts[sr]);
            // 页指示（右下角）
            M5.Display.setFont(&fonts::efontCN_14_b);
            M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
            M5.Display.setTextDatum(textdatum_t::middle_right);
            char pg[24];
            snprintf(pg, sizeof(pg), "转写 %d/%d 页", asrPageIdx + 1, asrPages);
            M5.Display.drawString(pg, x + dW, startY + MAIN_AREA_HEIGHT - 40);
            M5.Display.setTextDatum(textdatum_t::top_left);
            // 底部操作提示条
            M5.Display.fillRoundRect(x, startY + MAIN_AREA_HEIGHT - 44, dW, 36, 8, TFT_NAVY);
            M5.Display.setFont(&fonts::efontCN_14_b);
            M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
            M5.Display.setTextDatum(textdatum_t::middle_center);
            M5.Display.drawString("A/B翻页 · 长按A回放 · 短按C返回", x + dW / 2, startY + MAIN_AREA_HEIGHT - 26);
            M5.Display.setTextDatum(textdatum_t::top_left);
        }
    } else {
        // 列表最多显示 5 条（与以前一致），每条为"横条卡片"两行：主行摘要(24号) + 副行辅助(12号)
        int lineH = (listH - 16) / 5;
        int visTop = (todoIndex / 5) * 5;
        int x = MARGIN_X + 8;
        int y = startY + 8;
        int shown = 0;
        for (int i = visTop; i < (int)todoCount && shown < 5; i++, shown++) {
            const TodoItem& it = todoItems[i];
            int itemY = y + shown * lineH;
            int barW = areaW - 16;
            // 横条背景：当前选中深蓝底（反白），未选中白底
            bool sel = (i == (int)todoIndex);
            M5.Display.fillRoundRect(x, itemY, barW, lineH - 6, 8, sel ? TFT_NAVY : TFT_WHITE);
            M5.Display.drawRoundRect(x, itemY, barW, lineH - 6, 8, it.done ? TFT_DARKGREY : (sel ? TFT_WHITE : TFT_NAVY));

            bool asrBusy = (asrPending && asrPendingIdx == (int)i);   // 该条正在后台转写

            if (it.audioFile[0] != '\0') {
                // ===== 语音待办：单行「文件名(20260810录音N秒) + 空格 + 转写文字」= 能显示多少显示多少，截断不超bar =====
                M5.Display.setFont(&fonts::efontCN_16_b);
                M5.Display.setTextDatum(textdatum_t::middle_left);
                // 文件名：YYYYMMDD录音N秒（年=当前，月日=recTime "MM-DD HH:MM"）
                auto rdt0 = M5.Rtc.getDateTime();
                int curYr = rdt0.date.year < 2024 ? 2026 : rdt0.date.year;
                char nameBuf[48];
                if (it.recTime[0] && strlen(it.recTime) >= 5) {
                    char mm[3] = {it.recTime[0], it.recTime[1], 0};
                    char dd[3] = {it.recTime[3], it.recTime[4], 0};
                    snprintf(nameBuf, sizeof(nameBuf), "%04d%s%s录音%ds", curYr, mm, dd, (int)it.recSec);
                } else {
                    snprintf(nameBuf, sizeof(nameBuf), "%04d%02d%02d录音%ds", curYr, rdt0.date.month, rdt0.date.date, (int)it.recSec);
                }
                M5.Display.setTextColor(sel ? TFT_WHITE : TFT_NAVY, sel ? TFT_NAVY : TFT_WHITE);
                M5.Display.drawString(nameBuf, x + 14, itemY + (lineH - 6) / 2);
                int nw = M5.Display.textWidth(nameBuf);
                // 转写文字（截断到 bar 剩余宽度；转写中显示状态）
                int subX = x + 14 + nw + 8;
                if (asrBusy) {
                    M5.Display.setTextColor(sel ? TFT_LIGHTGREY : TFT_DARKGREY, sel ? TFT_NAVY : TFT_WHITE);
                    M5.Display.drawString("转写中…", subX, itemY + (lineH - 6) / 2);
                } else if (it.asr[0] != '\0') {
                    char sb[512];
                    snprintf(sb, sizeof(sb), "%s", it.asr);
                    while (strlen(sb) > 0 && M5.Display.textWidth(sb) > (barW - 24 - nw)) {
                        size_t n = strlen(sb);
                        size_t cut = 1;
                        while (cut < n && ((unsigned char)sb[n - cut] & 0xC0) == 0x80) cut++;
                        sb[n - cut] = '\0';
                    }
                    if (sb[0]) {
                        M5.Display.setTextColor(sel ? TFT_LIGHTGREY : TFT_BLACK, sel ? TFT_NAVY : TFT_WHITE);
                        M5.Display.drawString(sb, subX, itemY + (lineH - 6) / 2);
                    }
                }
                M5.Display.setTextDatum(textdatum_t::top_left);
            } else {
                // ===== 文字待办：24号主行 + 12号副行 =====
                M5.Display.setFont(&fonts::efontCN_24_b);
                M5.Display.setTextColor(sel ? TFT_WHITE : TFT_BLACK, sel ? TFT_NAVY : TFT_WHITE);
                M5.Display.setTextDatum(textdatum_t::top_left);
                char main[48];
                if (it.done) snprintf(main, sizeof(main), "✓ %s", it.text);
                else snprintf(main, sizeof(main), "%s", it.text);
                M5.Display.drawString(main, x + 14, itemY + 8);
                // 副行
                M5.Display.setFont(&fonts::efontCN_12_b);
                M5.Display.setTextColor(sel ? TFT_LIGHTGREY : TFT_NAVY, sel ? TFT_NAVY : TFT_WHITE);
                char sub[48];
                if (it.done) snprintf(sub, sizeof(sub), "已完成");
                else if (it.highPriority) snprintf(sub, sizeof(sub), "高优先级");
                else sub[0] = '\0';
                if (sub[0]) M5.Display.drawString(sub, x + 14, itemY + lineH - 20);
                M5.Display.setTextDatum(textdatum_t::top_left);
            }
        }
    }

    // 底部录音按钮（详情模式已画操作提示条，跳过；列表模式显示录音按钮）
    bool inDetail = (todoDetailMode && todoItems[todoIndex].audioFile[0] != '\0' && todoItems[todoIndex].asr[0] != '\0');
    if (!inDetail) {
        int btnY = startY + listH + 6;
        uint32_t btnColor = isRecordingNow ? TFT_BLUE : TFT_RED;
        M5.Display.fillRoundRect(MARGIN_X + 20, btnY, areaW - 40, 38, 10, btnColor);
        M5.Display.setFont(&fonts::efontCN_16_b);
        M5.Display.setTextColor(TFT_WHITE, btnColor);
        M5.Display.setTextDatum(textdatum_t::middle_center);
        M5.Display.drawString(isRecordingNow ? "● 录音中…再按C停止" : "● 按C开始录音", SCREEN_WIDTH / 2, btnY + 20);
        M5.Display.setTextDatum(textdatum_t::top_left);
    }

    char infoBuf[48];
    snprintf(infoBuf, sizeof(infoBuf), "待办 %d/%d", (int)todoIndex + 1, (int)todoCount);
    M5.Display.setFont(&fonts::efontCN_14_b);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.drawString(infoBuf, MARGIN_X + 12, startY + MAIN_AREA_HEIGHT - 22);
}

// ====================================================================
// 页面 3: 日历黄历天气 (3:4:3 三栏)
// ====================================================================
// 天气简化图标（墨迹风：晴=太阳 云/阴=云 雨=雨滴 雪=雪花）
static void drawWeatherIcon(int x, int y, int r, const char* wt) {
    if (!wt) return;
    bool sun  = strstr(wt, "晴") != nullptr;
    bool cloud = strstr(wt, "云") != nullptr || strstr(wt, "阴") != nullptr;
    bool rain = strstr(wt, "雨") != nullptr || strstr(wt, "雷") != nullptr;
    bool snow = strstr(wt, "雪") != nullptr;
    if (sun && !cloud) {
        // 太阳：橙黄圆 + 光芒线
        M5.Display.fillCircle(x, y, r, TFT_ORANGE);
        for (int a = 0; a < 8; a++) {
            float ang = a * 0.785f;   // 45°
            int x1 = x + (int)((r + 3) * cosf(ang)), y1 = y + (int)((r + 3) * sinf(ang));
            int x2 = x + (int)((r + 8) * cosf(ang)), y2 = y + (int)((r + 8) * sinf(ang));
            M5.Display.drawLine(x1, y1, x2, y2, TFT_ORANGE);
        }
    }
    if (cloud) {
        // 云：三圆 + 底
        M5.Display.fillCircle(x - r / 2, y, r / 2, TFT_LIGHTGREY);
        M5.Display.fillCircle(x, y - r / 3, r / 2, TFT_LIGHTGREY);
        M5.Display.fillCircle(x + r / 2, y, r / 2, TFT_LIGHTGREY);
        M5.Display.fillRect(x - r, y, r * 2, r / 2, TFT_LIGHTGREY);
    }
    if (rain) {
        // 雨：三条蓝色斜线
        for (int i = -1; i <= 1; i++) {
            M5.Display.drawLine(x + i * 5, y - r / 2, x + i * 5 - 3, y + r / 2, TFT_BLUE);
        }
    }
    if (snow) {
        M5.Display.drawCircle(x, y, r / 2, TFT_NAVY);
        M5.Display.fillCircle(x, y, r / 3, TFT_LIGHTGREY);
    }
}

// 每日哲理短句（腹黑风，按日期轮换；≤11字保证24号横幅右侧放得下）
static const char* const PHILOSOPHY[] = {
    "崩溃，从下次一定开始。",
    "努力像在还房贷。",
    "成长是把哭声静音。",
    "想太多，做得少。",
    "间歇努力，持续躺平。",
    "搬砖不狠，地位不稳。",
    "坚持到底，输得体面。",
    "缺的不是机会是勇气。",
    "道理都懂，就是懒。",
    "苟且之外还是苟且。",
    "好运总在松懈时溜走。",
    "人生如心电图，一帆即挂。",
    "努力未必成，躺平爽。",
    "躺平不耻，焦虑才耻。",
    "价值，老板说了算。",
};

void drawCalendarPage() {
    int startY = STATUS_BAR_HEIGHT;
    int areaW = SCREEN_WIDTH - MARGIN_X * 2;
    int x0 = MARGIN_X;

    auto dt = M5.Rtc.getDateTime();
    int y = dt.date.year, m = dt.date.month, d = dt.date.date;
    if (y < 2024) { y = 2026; m = 8; d = 9; }
    d += calOffset;
    // 日期退位（跨月/跨年，支持看前一天）
    while (d < 1) {
        m--;
        if (m < 1) { m = 12; y--; }
        if (m == 2) d = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 29 : 28;
        else if (m == 4 || m == 6 || m == 9 || m == 11) d = 30;
        else d = 31;
    }
    if (m == 2) { bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); if (d > (leap ? 29 : 28)) { d = 1; m++; } }
    else if (d > 31 || ((m == 4 || m == 6 || m == 9 || m == 11) && d > 30)) { d = 1; m++; if (m > 12) { m = 1; y++; } }

    // 计算星期
    static const char* const WK[7] = {"周日","周一","周二","周三","周四","周五","周六"};
    struct tm tmv = {0};
    tmv.tm_year = y - 1900; tmv.tm_mon = m - 1; tmv.tm_mday = d;
    mktime(&tmv);
    int wday = (tmv.tm_wday + 7) % 7;

    // ========== 顶部日期横幅（全宽白卡 + 深蓝描边）Y 50~120（日期/农历/哲理短句 3 行）==========
    M5.Display.fillRoundRect(x0, startY + 8, areaW, 70, 12, TFT_WHITE);
    M5.Display.drawRoundRect(x0, startY + 8, areaW, 70, 12, TFT_NAVY);
    // 行1：8月10日 星期一（24号同字号同基线；月日红 + 星期周末红/平日蓝；下移留上边距不压框线）Y 56~84
    M5.Display.setTextDatum(textdatum_t::top_left);
    char mdStr[16];
    snprintf(mdStr, sizeof(mdStr), "%d月%d日", m, d);
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextColor(TFT_RED, TFT_WHITE);
    M5.Display.drawString(mdStr, x0 + 14, startY + 14);
    int mdW = M5.Display.textWidth(mdStr);
    char wkStr[8];
    snprintf(wkStr, sizeof(wkStr), "%s", WK[wday]);
    M5.Display.setTextColor((wday == 0 || wday == 6) ? TFT_RED : TFT_BLUE, TFT_WHITE);
    M5.Display.drawString(wkStr, x0 + 14 + mdW + 10, startY + 14);
    // 行2：农历（黑）+ 干支（蓝）左对齐 Y 88~104
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.drawString(calLunar, x0 + 14, startY + 46);
    int lmW = M5.Display.textWidth(calLunar);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.drawString(calGanzhi[0] ? calGanzhi : "丙午年", x0 + 14 + lmW + 12, startY + 46);
    // 右侧：每日哲理短句（黑色 24 号，垂直居中右对齐）
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::middle_right);
    int phiIdx = ((y * 13 + m * 7 + d) % 15 + 15) % 15;
    M5.Display.drawString(PHILOSOPHY[phiIdx], x0 + areaW - 14, startY + 40);
    M5.Display.setTextDatum(textdatum_t::top_left);

    // ========== 双栏：左月历 55% / 右天气+黄历 ==========
    int leftW = (int)(areaW * 0.55f);       // 308
    int gap = 10;
    int rightX = x0 + leftW + gap;          // 338
    int rightW = areaW - leftW - gap;       // 242
    int topY = startY + 82;                 // 124

    // ----- 左栏：完整月历（周标题 + 6行日期）Y 134~330 -----
    int leftH = MAIN_AREA_HEIGHT - 16 - (topY - startY - 8);   // 196
    M5.Display.fillRoundRect(x0, topY, leftW, leftH, 12, TFT_WHITE);
    M5.Display.drawRoundRect(x0, topY, leftW, leftH, 12, TFT_NAVY);
    int cellW = (leftW - 16) / 7;           // 每格宽 41
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setFont(&fonts::efontCN_14_b);
    // 周标题：独立单字数组（勿用 WK[c]+1 指针偏移！UTF-8 中文3字节会错位成乱码「口」），周末红；下方细线与日期区隔开
    static const char* const WD[7] = {"日","一","二","三","四","五","六"};
    for (int c = 0; c < 7; c++) {
        int cx = x0 + 8 + cellW * c + cellW / 2;
        M5.Display.setTextColor((c == 0 || c == 6) ? TFT_RED : TFT_NAVY, TFT_WHITE);
        M5.Display.drawString(WD[c], cx, topY + 16);
    }
    M5.Display.drawFastHLine(x0 + 3, topY + 30, leftW - 6, TFT_LIGHTGREY);   // 细分隔线（非方框）
    // 日期格 6 行（今天红色圆角底反白）
    // 修复P1-1: 按当月实际天数判断（否则2月/30天月会错误显示 29/30/31 号）
    int firstDow = (wday - (d - 1) % 7 + 7) % 7;   // 当月1号星期
    int daysInMonth = (m == 2) ? (((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 29 : 28)
                       : (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
    int yPos = topY + 44;
    for (int row = 0; row < 6; row++) {
        for (int c = 0; c < 7; c++) {
            int dayNum = row * 7 + c - firstDow + 1;
            int cx = x0 + 8 + cellW * c + cellW / 2;
            if (dayNum >= 1 && dayNum <= daysInMonth) {
                bool isWeekend = (c == 0 || c == 6);
                bool isToday = (dayNum == d);
                char cell[4];
                snprintf(cell, sizeof(cell), "%d", dayNum);
                if (isToday) {
                    M5.Display.fillRoundRect(cx - cellW / 2 + 3, yPos - 10, cellW - 6, 20, 8, TFT_RED);
                    M5.Display.setTextColor(TFT_WHITE, TFT_RED);
                } else {
                    M5.Display.setTextColor(isWeekend ? TFT_RED : TFT_BLACK, TFT_WHITE);
                }
                M5.Display.drawString(cell, cx, yPos);
            }
        }
        yPos += 24;
    }
    M5.Display.setTextDatum(textdatum_t::top_left);

    // ----- 右栏：天气卡 Y 134~222（城市24号 / 当前日期图标+天气+高低温一行 / 次日完整一行）-----
    // 按 calOffset 选数据：主行 0=昨天 1=今天 2=明天 3=后天，副行=次日（大后天有数据）
    int wxMain = calOffset + 1;
    int wxNext = wxMain + 1;
    const char* nextLabel = "明日";
    if (calOffset == -1) nextLabel = "今天";
    else if (calOffset == 1) nextLabel = "后天";
    else if (calOffset == 2) nextLabel = "大后天";
    int wx = rightX, ww = rightW;
    int wcx = wx + ww / 2;
    M5.Display.fillRoundRect(wx, topY, ww, 88, 12, TFT_WHITE);
    M5.Display.drawRoundRect(wx, topY, ww, 88, 12, TFT_BLUE);
    // 城市（24号蓝，顶部居中）
    M5.Display.setTextDatum(textdatum_t::top_center);
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextColor(TFT_BLUE, TFT_WHITE);
    M5.Display.drawString(weatherCity, wcx, topY + 2);
    // 主行（当前日期）：图标 + 天气 + 高温 + 低温（整行居中，彼此间隙8px）
    int wy = topY + 42;
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    int wtW = M5.Display.textWidth(wxTxt[wxMain]);
    M5.Display.setFont(&fonts::efontCN_24_b);
    int hiW = M5.Display.textWidth("26°");
    M5.Display.setFont(&fonts::efontCN_16_b);
    int loW = M5.Display.textWidth("18°");
    int rowW = 28 + 8 + wtW + 8 + hiW + 8 + loW;   // 图标+天气+高+低温 整行宽
    int sx = wcx - rowW / 2;
    drawWeatherIcon(sx, wy, 14, wxTxt[wxMain]);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::middle_left);
    M5.Display.drawString(wxTxt[wxMain], sx + 28 + 8, wy);
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextColor(TFT_RED, TFT_WHITE);
    char wStr[16];
    snprintf(wStr, sizeof(wStr), "%d°", wxHi[wxMain]);
    M5.Display.drawString(wStr, sx + 28 + 8 + wtW + 8, wy);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_BLUE, TFT_WHITE);
    snprintf(wStr, sizeof(wStr), "%d°", wxLo[wxMain]);
    M5.Display.drawString(wStr, sx + 28 + 8 + wtW + 8 + hiW + 8, wy);
    // 副行（次日）：图标 + 标签(今日/明日/后天/大后天) + 天气 + 高/低（12号一行居中完整显示）
    int ny = topY + 72;
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setFont(&fonts::efontCN_12_b);
    int lblW = M5.Display.textWidth(nextLabel);
    int wxtW = M5.Display.textWidth(wxTxt[wxNext]);
    snprintf(wStr, sizeof(wStr), "%d°/%d°", wxHi[wxNext], wxLo[wxNext]);
    int tW = M5.Display.textWidth(wStr);
    int nrowW = 20 + 3 + lblW + 3 + wxtW + 3 + tW;   // 图标+标签+天气+温度 整行宽
    int nsx = wcx - nrowW / 2;
    drawWeatherIcon(nsx + 10, ny, 9, wxTxt[wxNext]);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.drawString(nextLabel, nsx + 20 + 3, ny);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.drawString(wxTxt[wxNext], nsx + 20 + 3 + lblW + 3, ny);
    M5.Display.setTextColor(TFT_RED, TFT_WHITE);
    M5.Display.drawString(wStr, nsx + 20 + 3 + lblW + 3 + wxtW + 3, ny);
    M5.Display.setTextDatum(textdatum_t::top_center);

    // ----- 右栏：黄历卡 Y 212~322（宜24号绿 / 忌24号红）-----
    int hy = topY + 88;                     // 212
    M5.Display.fillRoundRect(wx, hy, ww, MAIN_AREA_HEIGHT - 16 - (hy - startY), 12, TFT_WHITE);
    M5.Display.drawRoundRect(wx, hy, ww, MAIN_AREA_HEIGHT - 16 - (hy - startY), 12, TFT_RED);
    M5.Display.setTextDatum(textdatum_t::top_center);
    M5.Display.setFont(&fonts::efontCN_12_b);
    M5.Display.setTextColor(TFT_RED, TFT_WHITE);
    M5.Display.drawString("黄历宜忌", wcx, hy + 8);
    // 宜（24号绿）
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextColor(TFT_DARKGREEN, TFT_WHITE);
    M5.Display.drawString(calYi, wcx, hy + 34);
    // 忌（24号红）
    M5.Display.setTextColor(TFT_RED, TFT_WHITE);
    M5.Display.drawString(calJi, wcx, hy + 66);

    M5.Display.setTextDatum(textdatum_t::top_left);
}

// ====================================================================
// 页面 4: 微信二维码 (白底主体 + 底部深蓝标题条)
// ====================================================================
// 读取 PNG 图片宽高（IHDR 第16-24字节，大端）
static bool readPngSize(const char* path, uint16_t& w, uint16_t& h) {
    if (!initSDCard() || !SD.exists(path)) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint8_t hdr[24];
    if (f.read(hdr, 24) != 24) { f.close(); return false; }
    f.close();
    if (hdr[0] != 0x89 || hdr[1] != 0x50 || hdr[2] != 0x4E || hdr[3] != 0x47) return false;
    w = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
    h = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
    return (w > 0 && h > 0);
}

void drawQRCodePage() {
    int startY = STATUS_BAR_HEIGHT;
    int areaW = SCREEN_WIDTH - MARGIN_X * 2;
    int areaH = MAIN_AREA_HEIGHT;

    if (qrCount == 0) scanQRCodeDirectory();

    // 主体区：白底
    M5.Display.fillRect(MARGIN_X, startY, areaW, areaH, TFT_WHITE);

    if (qrCount > 0 && qrIndex < qrCount) {
        const char* path = qrPaths[qrIndex];
        if (initSDCard() && SD.exists(path)) {
            // 等比缩放居中显示（保持宽高比，横着最合适比例加载）
            if (strstr(path, ".png")) {
                uint16_t iw = 0, ih = 0;
                if (readPngSize(path, iw, ih) && iw > 0 && ih > 0) {
                    float sc = (areaW / (float)iw) < (areaH / (float)ih) ? (areaW / (float)iw) : (areaH / (float)ih);
                    if (sc > 1.0f) sc = 1.0f;
                    int dw = (int)(iw * sc), dh = (int)(ih * sc);
                    int dx = MARGIN_X + (areaW - dw) / 2, dy = startY + (areaH - dh) / 2;
                    M5.Display.drawPngFile(SD, path, dx, dy, dw, dh, 0, 0, sc, sc, textdatum_t::top_left);
                } else {
                    M5.Display.drawPngFile(SD, path, MARGIN_X, startY);
                }
            } else if (strstr(path, ".bmp")) M5.Display.drawBmpFile(SD, path, MARGIN_X, startY);
            else M5.Display.drawJpgFile(SD, path, MARGIN_X, startY);
            M5.Display.drawRect(MARGIN_X, startY, areaW, areaH, TFT_NAVY);
        } else {
            M5.Display.setFont(&fonts::efontCN_24_b);
            M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
            M5.Display.setTextDatum(textdatum_t::middle_center);
            M5.Display.drawString("二维码加载失败", SCREEN_WIDTH / 2, startY + 110);
            M5.Display.setTextDatum(textdatum_t::top_left);
        }
    } else {
        // 无二维码时显示占位
        M5.Display.setFont(&fonts::efontCN_24_b);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        M5.Display.setTextDatum(textdatum_t::middle_center);
        M5.Display.drawString("请将二维码图片放入", SCREEN_WIDTH / 2, startY + 70);
        M5.Display.drawString("SD 卡 /qr 目录", SCREEN_WIDTH / 2, startY + 110);
        M5.Display.setFont(&fonts::efontCN_16_b);
        M5.Display.drawString("支持 png / jpg / bmp", SCREEN_WIDTH / 2, startY + 160);
        M5.Display.setTextDatum(textdatum_t::top_left);
    }
    // 已彻底删除底部"微信二维码 1/1"标题栏（避免遮挡二维码），二维码占满主体区且四周留白
}

// ====================================================================
// 页面 6: 智谱 Coding Plan 额度监控
// 数据来源：USB 串口主机推送 JSON（只更新内存，不刷屏）
// ====================================================================

// 从 SD 卡 /config.ini 读取 Coding Plan 配置（缺失则用默认值）
void loadCodingPlanConfig() {
    if (!initSDCard()) return;
    if (!SD.exists("/config.ini")) return;
    File f = SD.open("/config.ini", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String key = line.substring(0, eq); key.trim();
        String val = line.substring(eq + 1); val.trim();
        if (key == "poll_interval_sec") cpPollIntervalSec = val.toInt();
        else if (key == "warn_threshold") cpWarnThreshold = val.toInt();
        else if (key == "alert_threshold") cpAlertThreshold = val.toInt();
        else if (key == "sd_log_enabled") cpSdLogEnabled = val.equalsIgnoreCase("true");
        else if (key == "wifi_ssid") { strncpy(cpWifiSsid[0], val.c_str(), 39); cpWifiSsid[0][39] = '\0'; }
        else if (key == "wifi_pass") { strncpy(cpWifiPass[0], val.c_str(), 39); cpWifiPass[0][39] = '\0'; }
        else if (key == "wifi_ssid2") { strncpy(cpWifiSsid[1], val.c_str(), 39); cpWifiSsid[1][39] = '\0'; }
        else if (key == "wifi_pass2") { strncpy(cpWifiPass[1], val.c_str(), 39); cpWifiPass[1][39] = '\0'; }
        else if (key == "wifi_ssid3") { strncpy(cpWifiSsid[2], val.c_str(), 39); cpWifiSsid[2][39] = '\0'; }
        else if (key == "wifi_pass3") { strncpy(cpWifiPass[2], val.c_str(), 39); cpWifiPass[2][39] = '\0'; }
        else if (key == "zhipu_cookie") { strncpy(cpZhipuCookie, val.c_str(), sizeof(cpZhipuCookie) - 1); cpZhipuCookie[sizeof(cpZhipuCookie) - 1] = '\0'; }
        else if (key == "sf_api_key") { strncpy(sfApiKey, val.c_str(), sizeof(sfApiKey) - 1); sfApiKey[sizeof(sfApiKey) - 1] = '\0'; }
        else if (key == "weather_lat") weatherLat = val.toFloat();
        else if (key == "weather_lon") weatherLon = val.toFloat();
        else if (key == "weather_city") { strncpy(weatherCity, val.c_str(), sizeof(weatherCity) - 1); weatherCity[sizeof(weatherCity) - 1] = '\0'; }
        else if (key == "use_wifi") cpUseWifi = val.equalsIgnoreCase("true");
    }
    f.close();
}

// 解析串口 JSON（ArduinoJson 7）
// 仅更新全局内存变量，绝对禁止在此函数内调用任何刷新函数
void parseCodingPlanJson(const char* json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return;   // JSON 非法则忽略

    if (doc["quota_5h_percent"].is<int>()) {
        cpData.quota_5h_percent = doc["quota_5h_percent"].as<int>();
        cpData.connected = true;
    }
    if (doc["quota_7d_percent"].is<int>()) cpData.quota_7d_percent = doc["quota_7d_percent"].as<int>();
    if (doc["quota_mcp_percent"].is<int>()) cpData.quota_mcp_percent = doc["quota_mcp_percent"].as<int>();
    if (doc["quota_5h_reset_time"].is<const char*>()) {
        strncpy(cpData.quota_5h_reset_time, doc["quota_5h_reset_time"], sizeof(cpData.quota_5h_reset_time) - 1);
    }
    if (doc["quota_7d_reset_time"].is<const char*>()) {
        strncpy(cpData.quota_7d_reset_time, doc["quota_7d_reset_time"], sizeof(cpData.quota_7d_reset_time) - 1);
    }
    if (doc["quota_mcp_reset_time"].is<const char*>()) {
        strncpy(cpData.quota_mcp_reset_time, doc["quota_mcp_reset_time"], sizeof(cpData.quota_mcp_reset_time) - 1);
    }
    if (doc["daily_token_total"].is<long>()) cpData.daily_token_total = doc["daily_token_total"].as<long>();
    if (doc["daily_token_main"].is<long>()) cpData.daily_token_main = doc["daily_token_main"].as<long>();
    if (doc["plan_status"].is<const char*>()) {
        strncpy(cpData.plan_status, doc["plan_status"], sizeof(cpData.plan_status) - 1);
    }
    if (doc["update_time"].is<const char*>()) {
        strncpy(cpData.update_time, doc["update_time"], sizeof(cpData.update_time) - 1);
    }

    cpLastSerialAt = millis();

    // 每日用量追加写 SD（数据解耦：日志与刷新无关）
    if (cpSdLogEnabled && initSDCard()) {
        static String lastLoggedDay = "";
        auto dt = M5.Rtc.getDateTime();
        char day[16];
        snprintf(day, sizeof(day), "%04d-%02d-%02d", dt.date.year, dt.date.month, dt.date.date);
        if (lastLoggedDay != day) {
            lastLoggedDay = day;
            File lf = SD.open("/coding_plan_usage.csv", FILE_APPEND);
            if (lf) {
                lf.printf("%s,%d,%d,%d,%d,%d,%ld,%ld\n",
                          day, cpData.quota_5h_percent, cpData.quota_7d_percent,
                          cpData.quota_mcp_percent, cpData.plan_status,
                          (int)cpData.connected, cpData.daily_token_total, cpData.daily_token_main);
                lf.close();
            }
        }
    }
}

// 智谱用量接口真实返回结构（GET /api/monitor/usage/quota/limit）:
// {"code":200,"data":{"limits":[
//   {"type":"TOKENS_LIMIT","unit":3,"number":5,"percentage":100,"nextResetTime":1786249956626},  // 每5小时
//   {"type":"TOKENS_LIMIT","unit":6,"number":1,"percentage":21,"nextResetTime":...},             // 每周
//   {"type":"TIME_LIMIT","unit":5,"number":1,"percentage":20,...}                                // MCP每月
// ],"level":"pro"},"success":true}
// 说明：接口用 Authorization: Bearer <bigmodel_token_production> 认证（不是 Cookie）。

// 毫秒时间戳(UTC) → 北京时间文本（withDate=false 输出 "HH:MM"，否则 "MM-DD HH:MM"）
static void formatBjtTime(long long tsMs, char* buf, int bufLen, bool withDate) {
    time_t t = (time_t)(tsMs / 1000LL) + 8 * 3600;  // UTC → 北京时间
    struct tm* tm = gmtime(&t);
    if (!tm) { strncpy(buf, "--:--", bufLen); buf[bufLen - 1] = '\0'; return; }
    if (withDate)
        snprintf(buf, bufLen, "%02d-%02d %02d:%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
    else
        snprintf(buf, bufLen, "%02d:%02d", tm->tm_hour, tm->tm_min);
}

// 解析智谱 quota/limit 接口 JSON（仅更新内存，绝不刷屏）
void parseZhipuQuotaJson(const char* json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return;
    if (doc["code"].as<int>() != 200) return;
    JsonArray limits = doc["data"]["limits"].as<JsonArray>();
    if (!limits) return;

    // 页面顺序固定：0=每5小时, 1=每周, 2=MCP月度
    int idx = 0;
    for (JsonObject lim : limits) {
        long long nextReset = lim["nextResetTime"] | 0LL;
        int percentage = lim["percentage"] | 0;
        if (idx == 0) {
            cpData.quota_5h_percent = percentage;
            formatBjtTime(nextReset, cpData.quota_5h_reset_time, sizeof(cpData.quota_5h_reset_time), false);
        } else if (idx == 1) {
            cpData.quota_7d_percent = percentage;
            formatBjtTime(nextReset, cpData.quota_7d_reset_time, sizeof(cpData.quota_7d_reset_time), true);
        } else if (idx == 2) {
            cpData.quota_mcp_percent = percentage;
            formatBjtTime(nextReset, cpData.quota_mcp_reset_time, sizeof(cpData.quota_mcp_reset_time), true);
        }
        idx++;
    }

    // 额度已降回正常 → 重置确认标记，下次再超限时重新提醒（避免一直静默）
    if (cpData.quota_5h_percent < cpAlertThreshold) cpAlertAcknowledged = false;

    cpData.connected = true;
    cpLastSerialAt = millis();   // 复用连接状态判定（30秒内有更新=正常）
    auto rdt = M5.Rtc.getDateTime();
    snprintf(cpData.update_time, sizeof(cpData.update_time),
             "%02d-%02d %02d:%02d", rdt.date.month, rdt.date.date, rdt.time.hours, rdt.time.minutes);

    // 每日用量追加写 SD（数据解耦：日志与刷新无关）
    if (cpSdLogEnabled && initSDCard()) {
        static String lastLoggedDay = "";
        auto dt = M5.Rtc.getDateTime();
        char day[16];
        snprintf(day, sizeof(day), "%04d-%02d-%02d", dt.date.year, dt.date.month, dt.date.date);
        if (lastLoggedDay != day) {
            lastLoggedDay = day;
            File lf = SD.open("/coding_plan_usage.csv", FILE_APPEND);
            if (lf) {
                lf.printf("%s,%d,%d,%d,%d,%d,%ld,%ld\n",
                          day, cpData.quota_5h_percent, cpData.quota_7d_percent,
                          cpData.quota_mcp_percent, cpData.plan_status,
                          (int)cpData.connected, cpData.daily_token_total, cpData.daily_token_main);
                lf.close();
            }
        }
    }
}

// WiFi 直连智谱 Coding Plan 额度接口（方案2）
// 从 config.ini 读取 wifi_ssid/wifi_pass/zhipu_cookie（cookie 字段存 Bearer Token），
// 轮询智谱官网用量接口 /api/monitor/usage/quota/limit。
// 仅更新内存变量，绝不刷屏；失败保留缓存数据。
void pollZhipuCodingPlan() {
    if (!cpUseWifi || strlen(cpZhipuCookie) == 0 || cpPollingNow) return;
    if (strlen(cpWifiSsid[0]) == 0 && strlen(cpWifiSsid[1]) == 0 && strlen(cpWifiSsid[2]) == 0) return;

    cpPollingNow = true;
    cpWifiBusy = true;   // RGB 青色反馈：正在连接/拉取

    // 若未连网则连接（依次尝试 3 组已知 WiFi；cpWifiOk 为全局缓存，待机断网时被重置）
    if (!cpWifiOk) {
        WiFi.mode(WIFI_STA);
        for (int i = 0; i < 3; i++) {
            if (strlen(cpWifiSsid[i]) == 0) continue;
            WiFi.begin(cpWifiSsid[i], cpWifiPass[i]);
            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 8000UL) {
                delay(200);
            }
            if (WiFi.status() == WL_CONNECTED) {
                cpWifiOk = true;
                wifiConnected = true;   // 顶栏显示 WiFi（initWiFiNTP 用占位SSID连不上，这里补）
                // 修复M5: 配时区+SNTP 同步真实时间（否则 RSS 每天8点定时判断永不成立）
                configTime(8 * 3600, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
                syncRtcFromNTP();        // 开机 WiFi 连接成功：把真实时间写回 RTC 芯片（待机页/日历页日期正确）
                fetchHttpTimeSync();     // HTTP 权威校准：SNTP 被拦截时强制覆盖真实时间
                Serial.printf("[CP] WiFi 已连接: %s (IP %s)\n", cpWifiSsid[i], WiFi.localIP().toString().c_str());
                break;
            }
        }
        cpWifiOk = (WiFi.status() == WL_CONNECTED);
        if (!cpWifiOk) {
            cpData.connected = false;
            snprintf(cpData.plan_status, sizeof(cpData.plan_status), "wifi断开");
            Serial.println("[CP] WiFi 连接失败（无可用已知网络）");
            cpWifiBusy = false;
            cpPollingNow = false;
            return;
        }
    }

    // 请求智谱 Coding Plan 用量接口（Authorization: Bearer 认证）
    WiFiClientSecure client;
    client.setInsecure();
    if (client.connect("open.bigmodel.cn", 443)) {
        client.println("GET /api/monitor/usage/quota/limit HTTP/1.1");
        client.println("Host: open.bigmodel.cn");
        client.print("Authorization: Bearer ");
        client.println(cpZhipuCookie);
        client.println("Accept: application/json");
        client.println("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
        client.println("Connection: close");
        client.println();

        unsigned long tout = millis();
        // 读取响应体（跳过响应头）
        bool inBody = false;
        String body = "";
        while (client.connected() && millis() - tout < 10000UL) {
            if (client.available()) {
                String line = client.readStringUntil('\n');
                if (!inBody) {
                    if (line == "\r" || line == "\n") { inBody = true; }
                    continue;
                }
                body += line;
            }
        }
        client.stop();

        // 解析智谱原生 JSON 响应体
        int brace = body.indexOf('{');
        if (brace >= 0) {
            body = body.substring(brace);
            parseZhipuQuotaJson(body.c_str());
            Serial.printf("[CP] 智谱额度: 5h=%d%% 7d=%d%% mcp=%d%% 更新=%s\n",
                          cpData.quota_5h_percent, cpData.quota_7d_percent,
                          cpData.quota_mcp_percent, cpData.update_time);
        } else {
            Serial.println("[CP] 智谱响应无JSON (body=" + body.substring(0, 120) + ")");
        }
    } else {
        cpData.connected = false;
        snprintf(cpData.plan_status, sizeof(cpData.plan_status), "连接失败");
        Serial.println("[CP] 连接 open.bigmodel.cn 失败");
    }

    // 今日 Token 消耗（model-usage 接口）→ 填充 daily_token_total / daily_token_main
    {
        auto rdt = M5.Rtc.getDateTime();
        if (rdt.date.year >= 2024) {
            char st[32], et[32];
            snprintf(st, sizeof(st), "%04d-%02d-%02d+00:00:00", rdt.date.year, rdt.date.month, rdt.date.date);
            snprintf(et, sizeof(et), "%04d-%02d-%02d+23:59:59", rdt.date.year, rdt.date.month, rdt.date.date);
            WiFiClientSecure c2;
            c2.setInsecure();
            if (c2.connect("open.bigmodel.cn", 443)) {
                c2.printf("GET /api/monitor/usage/model-usage?startTime=%s&endTime=%s HTTP/1.1\r\n", st, et);
                c2.println("Host: open.bigmodel.cn");
                c2.print("Authorization: Bearer ");
                c2.println(cpZhipuCookie);
                c2.println("Accept: application/json");
                c2.println("Connection: close");
                c2.println();

                unsigned long t2 = millis();
                bool inB2 = false;
                String body2 = "";
                while (c2.connected() && millis() - t2 < 10000UL) {
                    if (c2.available()) {
                        String line = c2.readStringUntil('\n');
                        if (!inB2) {
                            if (line == "\r" || line == "\n") { inB2 = true; }
                            continue;
                        }
                        body2 += line;
                    }
                }
                c2.stop();
                int br2 = body2.indexOf('{');
                if (br2 >= 0) {
                    JsonDocument doc2;
                    if (deserializeJson(doc2, body2.c_str() + br2) == DeserializationError::Ok) {
                        long tokens = doc2["data"]["totalUsage"]["totalTokensUsage"] | 0L;
                        long mainTok = doc2["data"]["totalUsage"]["modelSummaryList"][0]["totalTokens"] | 0L;
                        cpData.daily_token_total = tokens;
                        cpData.daily_token_main = mainTok;
                        Serial.printf("[CP] 今日token: %ld\n", tokens);
                    }
                }
            }
        }
    }

    // 工具用量（比模型 token 更直观：联网搜索/网页读取 MCP 调用次数）
    // GET /api/monitor/usage/tool-usage?startTime=今天00:00&endTime=23:59:59
    // 返回 data.totalUsage.totalNetworkSearchCount / totalWebReadMcpCount
    {
        auto rdt = M5.Rtc.getDateTime();
        if (rdt.date.year >= 2024) {
            char st[32], et[32];
            snprintf(st, sizeof(st), "%04d-%02d-%02d+00:00:00", rdt.date.year, rdt.date.month, rdt.date.date);
            snprintf(et, sizeof(et), "%04d-%02d-%02d+23:59:59", rdt.date.year, rdt.date.month, rdt.date.date);
            WiFiClientSecure c3;
            c3.setInsecure();
            if (c3.connect("open.bigmodel.cn", 443)) {
                c3.printf("GET /api/monitor/usage/tool-usage?startTime=%s&endTime=%s HTTP/1.1\r\n", st, et);
                c3.println("Host: open.bigmodel.cn");
                c3.print("Authorization: Bearer ");
                c3.println(cpZhipuCookie);
                c3.println("Accept: application/json");
                c3.println("Connection: close");
                c3.println();

                unsigned long t3 = millis();
                bool inB3 = false;
                String body3 = "";
                while (c3.connected() && millis() - t3 < 10000UL) {
                    if (c3.available()) {
                        String line = c3.readStringUntil('\n');
                        if (!inB3) {
                            if (line == "\r" || line == "\n") { inB3 = true; }
                            continue;
                        }
                        body3 += line;
                    }
                }
                c3.stop();
                int br3 = body3.indexOf('{');
                if (br3 >= 0) {
                    JsonDocument doc3;
                    if (deserializeJson(doc3, body3.c_str() + br3) == DeserializationError::Ok) {
                        long ns = doc3["data"]["totalUsage"]["totalNetworkSearchCount"] | 0L;
                        long wr = doc3["data"]["totalUsage"]["totalWebReadMcpCount"] | 0L;
                        cpData.tool_search_count = ns;
                        cpData.tool_webread_count = wr;
                        Serial.printf("[CP] 工具: 搜索%ld 读取%ld\n", ns, wr);
                    }
                }
            }
        }
    }

    cpWifiBusy = false;
    cpPollingNow = false;
}

// 从串口逐字节接收并组包（以换行为结束符），只更新内存
// 支持两种命令：
//   {"quota...":...}         → Coding Plan JSON 数据
//   #CFG|<config.ini内容>    → 写入 SD 卡 /config.ini（主机远程配置）
void handleCodingPlanSerial() {
    while (Serial.available()) {
        char c = Serial.read();

        // 文件接收模式：字节入缓冲，攒够512批量写SD；主机固定延时发送，无需逐块ACK
        if (fileReceiving) {
            fileBuf[fileBufLen++] = (uint8_t)c;
            if (fileBufLen >= 512) {
                uploadFile.write(fileBuf, fileBufLen);
                fileBufLen = 0;
            }
            fileReceived++;
            if (fileReceived >= fileExpected) {
                if (fileBufLen > 0) uploadFile.write(fileBuf, fileBufLen);
                fileBufLen = 0;
                uploadFile.close();
                fileReceiving = false;
                Serial.println("[OK] file done");
                // 若上传到 /qr/ 则重新扫描二维码
                if (strstr(cpSerialBuf, "/qr/")) {
                    qrCount = 0;
                    scanQRCodeDirectory();
                }
            }
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (cpSerialLen > 0) {
                cpSerialBuf[cpSerialLen] = '\0';
                // 收到串口命令视为活动：唤醒待机 + 刷新闲置计时（远程诊断/测试可用）
                if (standbyMode) { wakeFromStandby(); Serial.println("[WAKE] 串口唤醒"); }
                lastActivityMs = millis();
                // 修复M7: 录音中仅放行状态查询(#PSTATE)与停止录音(#REC|)，其余命令忽略，防破坏录音会话
                if (isRecordingNow && strncmp(cpSerialBuf, "#PSTATE", 7) != 0 && strncmp(cpSerialBuf, "#REC|", 5) != 0) {
                    Serial.println("[OK] rec busy");
                    cpSerialLen = 0;
                    continue;
                }
                if (strncmp(cpSerialBuf, "#FILEUPLOAD|", 12) == 0) {
                    // 开始文件上传：#FILEUPLOAD|/path|size
                    String cmd = String(cpSerialBuf + 12);
                    int bar = cmd.indexOf('|');
                    if (bar > 0) {
                        String path = cmd.substring(0, bar);
                        fileExpected = cmd.substring(bar + 1).toInt();
                        if (initSDCard()) {
                            // 确保父目录存在（如 /qr）
                            int slash = path.lastIndexOf('/');
                            if (slash > 0) {
                                String dir = path.substring(0, slash);
                                SD.mkdir(dir.c_str());
                            }
                            uploadFile = SD.open(path.c_str(), FILE_WRITE);
                            if (uploadFile) {
                                fileReceiving = true;
                                fileReceived = 0;
                                fileChunkCount = 0;
                                Serial.println("[OK] file start");
                            } else {
                                Serial.println("[ERR] open fail");
                            }
                        } else {
                            Serial.println("[ERR] no sd");
                        }
                    }
                } else if (cpSerialBuf[0] == '{') {
                    parseCodingPlanJson(cpSerialBuf);
                } else if (strncmp(cpSerialBuf, "#CFG|", 5) == 0) {
                    // 远程写 config.ini
                    // 注意：串口用 \n 作命令结束符，故主机把 config 内换行转义为 \x01，这里还原
                    // 修复P2-2: 诊断打印按实际长度安全截断（原 cpSerialBuf+144 可能读未初始化区输出垃圾）
                    int cfDiag = (int)cpSerialLen - 5 - 139;   // token 在偏移144处
                    if (cfDiag < 0) cfDiag = 0;
                    if (cfDiag > 20) cfDiag = 20;
                    Serial.printf("[CFG] recv_len=%d cookie_first=%.*s\n", (int)cpSerialLen, cfDiag, cpSerialBuf + 5 + 139);
                    if (initSDCard()) {
                        File cf = SD.open("/config.ini", FILE_WRITE);
                        if (cf) {
                            const char* p = cpSerialBuf + 5;
                            for (size_t i = 0; p[i] != '\0'; i++) {
                                if (p[i] == '\x01') cf.write('\n');
                                else cf.write((uint8_t)p[i]);
                            }
                            cf.close();
                            loadCodingPlanConfig();
                            Serial.println("[OK] config saved");
                        } else {
                            Serial.println("[ERR] sd write fail");
                        }
                    } else {
                        Serial.println("[ERR] no sd");
                    }
                } else if (strncmp(cpSerialBuf, "#CFGCLEAR", 9) == 0) {
                    // 清空 config.ini（配合逐行写入 #CFGLINE）
                    if (initSDCard()) {
                        File cf = SD.open("/config.ini", FILE_WRITE);
                        if (cf) cf.close();
                        Serial.println("[OK] cfg cleared");
                    } else {
                        Serial.println("[ERR] no sd");
                    }
                } else if (strncmp(cpSerialBuf, "#CFGLINE|", 9) == 0) {
                    // 逐行追加 config.ini（每行独立命令 <100 字节，避免 USB CDC 缓冲溢出）
                    if (initSDCard()) {
                        File cf = SD.open("/config.ini", FILE_APPEND);
                        if (cf) {
                            cf.println(cpSerialBuf + 9);
                            cf.close();
                        }
                    }
                    Serial.println("[OK] line");   // ACK：主机逐条确认，防 USB CDC 积压溢出
                } else if (strncmp(cpSerialBuf, "#CFGTOK|", 8) == 0) {
                    // 分块累积智谱 token（USB CDC 长行溢出，主机分段发送）
                    const char* chunk = cpSerialBuf + 8;
                    size_t len = strlen(chunk);
                    if (len > 0 && cpTokenLen + len < 512) {
                        memcpy(cpTokenBuf + cpTokenLen, chunk, len);
                        cpTokenLen += len;
                        cpTokenBuf[cpTokenLen] = '\0';
                    }
                    Serial.println("[OK] tok");    // ACK：主机逐条确认
                } else if (strncmp(cpSerialBuf, "#CFGDONE", 8) == 0) {
                    // config 写完：追加 token 行，重新加载
                    if (initSDCard()) {
                        File cf = SD.open("/config.ini", FILE_APPEND);
                        if (cf) {
                            cf.print("zhipu_cookie=");
                            cf.write((const uint8_t*)cpTokenBuf, cpTokenLen);
                            cf.println();
                            cf.close();
                        }
                    }
                    loadCodingPlanConfig();
                    Serial.printf("[OK] config saved (token_len=%d)\n", (int)cpTokenLen);
                } else if (strncmp(cpSerialBuf, "#LED|", 5) == 0) {
                    // #LED|r,g,b 亮灯3秒（确认 LED 硬件是否工作）+ 诊断
                    int rr = 255, gg = 0, bb = 0;
                    sscanf(cpSerialBuf + 5, "%d,%d,%d", &rr, &gg, &bb);
                    ledTestR = rr; ledTestG = gg; ledTestB = bb;
                    ledTestUntil = millis() + 3000;
                    rgbStrip.begin();
                    ledSetBrightness(220);
                    ledSetAll(255, 0, 0);
                    Serial.println("[LED] Adafruit NeoPixel OK (GPIO21, 2x WS2812)");
                    Serial.println("[OK] led");
                } else if (strncmp(cpSerialBuf, "#TONE", 5) == 0) {
                    // 喇叭测试：#TONE 响三声（880/1046/784Hz），确认 ES8311 喇叭硬件是否工作
                    if (M5.Mic.isEnabled()) M5.Mic.end();
                    M5.Speaker.begin();
                    M5.Speaker.stop();
                    Serial.println("[TONE] 播放三声...");
                    M5.Speaker.tone(880, 150);
                    delay(220);
                    M5.Speaker.tone(1046, 200);
                    delay(280);
                    M5.Speaker.tone(784, 150);
                    delay(220);
                    M5.Speaker.stop();
                    Serial.println("[OK] tone done");
                } else if (strncmp(cpSerialBuf, "#WAVDIAG", 8) == 0) {
                    // WAV 文件诊断：检查 /record 下每个 WAV 的大小和头信息（确认文件是否有效）
                    if (initSDCard()) {
                        for (int i = 0; i < 30; i++) {
                            char p[64];
                            snprintf(p, sizeof(p), "/record/todo_%d.wav", i);
                            if (!SD.exists(p)) continue;
                            File wf = SD.open(p, FILE_READ);
                            if (!wf) { Serial.printf("[WAV] %s open fail\n", p); continue; }
                            size_t sz = wf.size();
                            uint8_t hdr[44];
                            size_t rd = wf.read(hdr, 44);
                            wf.close();
                            if (rd == 44 && memcmp(hdr, "RIFF", 4) == 0) {
                                uint32_t dataSz;
                                memcpy(&dataSz, hdr + 40, 4);
                                uint16_t sr;
                                memcpy(&sr, hdr + 24, 2);
                                uint16_t ch;
                                memcpy(&ch, hdr + 22, 2);
                                Serial.printf("[WAV] %s size=%u hdr_data=%u sr=%u ch=%u %s\n",
                                              p, (unsigned)sz, (unsigned)dataSz, (unsigned)sr, (unsigned)ch,
                                              (sz == dataSz + 44) ? "OK" : "MISMATCH");
                            } else {
                                Serial.printf("[WAV] %s size=%u BAD_HEADER\n", p, (unsigned)sz);
                            }
                        }
                    } else {
                        Serial.println("[ERR] no sd");
                    }
                    Serial.println("[OK] wavdiag");
                } else if (strncmp(cpSerialBuf, "#REC|", 5) == 0) {
                    // 完整录音链路测试：#REC|秒数 — 直接调 recordTodoVoice() 开始→等N秒→saveRecording()
                    // 走与按键完全相同的代码路径，验证"开始→录制→保存"整条链路
                    int sec = atoi(cpSerialBuf + 5);
                    if (sec < 1) sec = 5;
                    if (sec > 30) sec = 30;
                    Serial.printf("[REC] #REC 触发完整录音 %d 秒...\n", sec);
                    recordTodoVoice();    // 开始录音（与短按C相同）
                    unsigned long tStart = millis();
                    while (millis() - tStart < (unsigned long)sec * 1000UL) {
                        processRecording();   // 与 loop 相同的分块采样
                        delay(5);
                    }
                    if (isRecordingNow) saveRecording();   // 停止并保存（与再按C相同）
                    Serial.println("[OK] rec done");
                } else if (strncmp(cpSerialBuf, "#RECA|", 6) == 0) {
                    // 录音诊断：#RECA|秒数 — 自动录N秒并打印各声道峰值/均值（定位麦克风是否采到、人声在哪个声道、是否削波）
                    // 用固定小块缓冲循环分析（不占大内存），最多分析 3 秒
                    int sec = atoi(cpSerialBuf + 6);
                    if (sec < 1) sec = 3;
                    if (sec > 3) sec = 3;
                    if (M5.Speaker.isEnabled()) { M5.Speaker.end(); delay(50); }
                    m5::mic_config_t mc;
                    mc.pin_data_in = 39; mc.pin_ws = 41; mc.pin_bck = 40; mc.pin_mck = 42;
                    mc.input_channel = m5::input_stereo;
                    mc.over_sampling = 2;
                    mc.sample_rate = REC_SAMPLE_RATE;
                    mc.magnification = 64;
                    M5.Mic.config(mc);
                    Serial.println("[RECA] Mic begin...");
                    M5.Mic.begin();
                    int chunk = REC_CHUNK_SAMPLES * 2;
                    int blocks = (sec * REC_SAMPLE_RATE) / REC_CHUNK_SAMPLES;  // 采样块数
                    int64_t sumAbsL = 0, sumAbsR = 0;
                    int16_t maxL = 0, maxR = 0;
                    int samples = 0;
                    unsigned long t0 = millis();
                    // 乒乓：提交 A、读 B（官方模式）
                    int16_t* sub = recBufA;
                    int16_t* rd = recBufB;
                    bool first = true;
                    for (int b = 0; b < blocks; b++) {
                        if (!M5.Mic.record(sub, chunk, REC_SAMPLE_RATE, true)) break;
                        if (!first) {
                            for (int i = 0; i < REC_CHUNK_SAMPLES; i++) {
                                int16_t l = rd[i * 2];
                                int16_t r = rd[i * 2 + 1];
                                int al = l < 0 ? -l : l;
                                int ar = r < 0 ? -r : r;
                                sumAbsL += al; sumAbsR += ar;
                                if (al > maxL) maxL = al;
                                if (ar > maxR) maxR = ar;
                                samples++;
                            }
                        }
                        first = false;
                        int16_t* tmp = sub; sub = rd; rd = tmp;
                    }
                    M5.Mic.end();
                    unsigned long dt = millis() - t0;
                    int avgL = samples ? (int)(sumAbsL / samples) : 0;
                    int avgR = samples ? (int)(sumAbsR / samples) : 0;
                    Serial.printf("[RECA] samples=%d dur=%ums L:pk=%d avg=%d R:pk=%d avg=%d\n",
                                  samples, (unsigned)dt, (int)maxL, avgL, (int)maxR, avgR);
                    Serial.println("[OK] reca");
                } else if (strncmp(cpSerialBuf, "#PSTATE", 7) == 0) {
                    // 播放/录音/按键状态诊断
                    Serial.printf("[PSTATE] voicePlaying=%d isPlaying=%d isRec=%d micReady=%d page=%d todoCount=%d\n",
                                  (int)voicePlaying, (int)M5.Speaker.isPlaying(), (int)isRecordingNow,
                                  (int)recMicReady, (int)currentPage, (int)todoCount);
                    Serial.printf("[PSTATE] A:lvl=%d clk=%d hold=%d B:lvl=%d clk=%d hold=%d C:lvl=%d clk=%d hold=%d\n",
                                  (int)keyA.level, (int)keyA.clickPending, (int)keyA.holdPending,
                                  (int)keyB.level, (int)keyB.clickPending, (int)keyB.holdPending,
                                  (int)keyC.level, (int)keyC.clickPending, (int)keyC.holdPending);
                    Serial.println("[OK] pstate");
                } else if (strncmp(cpSerialBuf, "#PLAY|", 6) == 0) {
                    // 回放诊断：#PLAY|序号 — 强制回放 /record/todo_N.wav 并打印详情
                    // 定位回放链路：文件读取→Speaker 初始化→playWav 播放
                    int idx = atoi(cpSerialBuf + 6);
                    char pp[64];
                    snprintf(pp, sizeof(pp), "/record/todo_%d.wav", idx);
                    if (!initSDCard() || !SD.exists(pp)) {
                        Serial.printf("[PLAY] %s 不存在\n", pp);
                        Serial.println("[OK] play");
                        cpSerialLen = 0;
                        continue;   // 修复P2-1: 原 break 跳出串口循环且不清空缓冲，导致下条命令残留拼接
                    }
                    File pf = SD.open(pp, FILE_READ);
                    if (!pf) { Serial.println("[PLAY] open fail"); Serial.println("[OK] play"); cpSerialLen = 0; continue; }   // 修复P2-1: 同上去 break
                    size_t pfs = pf.size();
                    Serial.printf("[PLAY] %s size=%u\n", pp, (unsigned)pfs);
                    uint8_t* pbuf = (uint8_t*)heap_caps_malloc(pfs, MALLOC_CAP_SPIRAM);
                    if (!pbuf) pbuf = (uint8_t*)malloc(pfs);
                    if (!pbuf) { pf.close(); Serial.println("[PLAY] no mem"); Serial.println("[OK] play"); break; }
                    pf.read(pbuf, pfs);
                    pf.close();
                    if (M5.Mic.isEnabled()) M5.Mic.end();
                    Serial.println("[PLAY] Speaker.begin...");
                    bool spkOk = M5.Speaker.begin();
                    Serial.printf("[PLAY] begin ret=%d isEnabled=%d\n", (int)spkOk, (int)M5.Speaker.isEnabled());
                    M5.Speaker.stop();
                    Serial.println("[PLAY] playWav...");
                    bool ok = M5.Speaker.playWav(pbuf, pfs);
                    Serial.printf("[PLAY] playWav ret=%d isPlaying=%d\n", (int)ok, (int)M5.Speaker.isPlaying());
                    unsigned long pws = millis();
                    while (M5.Speaker.isPlaying() && millis() - pws < 60000UL) {
                        delay(20);
                    }
                    Serial.printf("[PLAY] 结束 isPlaying=%d 耗时=%ums\n", (int)M5.Speaker.isPlaying(), (unsigned)(millis() - pws));
                    M5.Speaker.stop();
                    free(pbuf);
                    Serial.println("[OK] play");
                } else if (strncmp(cpSerialBuf, "#KEY", 4) == 0) {
                    // 按键诊断：持续采样 5 秒，每 150ms 打印三键电平（按下=0）+ 事件
                    Serial.println("[KEY] 持续采样5秒，请按A/B/C...");
                    for (int ki = 0; ki < 33; ki++) {
                        // 原始电平打印（digitalRead 直接值：空闲应=1，按下=0）
                        int ra = digitalRead(BTN_A_GPIO);
                        int rb = digitalRead(BTN_B_GPIO);
                        int rc = digitalRead(BTN_C_GPIO);
                        Serial.printf("[KEY] drA=%d drB=%d drC=%d page=%d\n", ra, rb, rc, (int)currentPage);
                        delay(150);
                    }
                    Serial.println("[OK] key done");
                } else if (strncmp(cpSerialBuf, "#CLEARREC", 9) == 0) {
                    // 清除旧录音文件 + /todo.txt + 内存待办数组（彻底重置待办列表）
                    if (initSDCard()) {
                        int deleted = 0;
                        for (int i = 0; i < 30; i++) {
                            char p[64];
                            snprintf(p, sizeof(p), "/record/todo_%d.wav", i);
                            if (SD.exists(p)) { SD.remove(p); deleted++; }
                        }
                        // 同时清掉持久化待办 txt（否则 loadTodoFromSD 会把旧待办读回来）
                        if (SD.exists("/todo.txt")) { SD.remove("/todo.txt"); deleted++; }
                        if (SD.exists("/todos.txt")) { SD.remove("/todos.txt"); deleted++; }
                        todoCount = 0;
                        todoIndex = 0;
                        for (size_t i = 0; i < TODO_MAX; i++) {
                            todoItems[i].text[0] = '\0';
                            todoItems[i].audioFile[0] = '\0';
                        }
                        Serial.printf("[OK] cleared %d rec files + todo.txt, todos reset\n", deleted);
                        // 若当前在待办页，刷新屏幕让用户立即看到已清空（显式用户操作，允许刷屏）
                        if (currentPage == 4) {
                            renderScreen(false);
                        }
                    } else {
                        Serial.println("[ERR] no sd");
                    }
                } else if (strncmp(cpSerialBuf, "#RMFILE|", 8) == 0) {
                    // 删除 SD 卡文件
                    if (initSDCard()) {
                        if (SD.remove(cpSerialBuf + 8)) {
                            Serial.println("[OK] removed");
                            qrCount = 0;
                            scanQRCodeDirectory();   // 重扫二维码目录
                        } else {
                            Serial.println("[ERR] remove fail");
                        }
                    } else {
                        Serial.println("[ERR] no sd");
                    }
                } else if (strncmp(cpSerialBuf, "#POLL", 5) == 0) {
                    // 强制立即轮询智谱（待机唤醒后 WiFi 未连则先重连，与 #WX 同路径；置 0 下次 loop 即触发）
                    if (WiFi.status() != WL_CONNECTED) reconnectWifiFromConfig();
                    cpLastPollAt = 0;
                    Serial.println("[OK] poll triggered");
                } else if (strncmp(cpSerialBuf, "#NEWS", 5) == 0) {
                    // 手动触发 IT之家 RSS 拉取（诊断）
                    Serial.println("[NEWS] 手动触发拉取");
                    newsUpdatedAt = millis();
                    fetchDailyNews();
                    Serial.printf("[NEWS] 完成 count=%d\n", newsCount);
                } else if (strncmp(cpSerialBuf, "#WX", 3) == 0) {
                    // 手动触发天气拉取（诊断；唤醒后 WiFi 未连则先重连，避免立即失败）
                    Serial.println("[WX] 手动触发拉取");
                    if (WiFi.status() != WL_CONNECTED) reconnectWifiFromConfig();
                    weatherUpdatedAt = 0;
                    fetchWeather();
                    Serial.printf("[WX] 完成 今日%s %d°/%d° 明日%s %d°/%d° 成功=%d\n",
                                  weatherText, weatherHigh, weatherLow, weatherTextTmr, weatherHighTmr, weatherLowTmr, (int)weatherSuccess);
                } else if (strncmp(cpSerialBuf, "#ASRDIAG", 8) == 0) {
                    // 诊断：SiliconFlow HTTP(80) 连接耗时（TLS 在设备上会卡，验证明文 HTTP 是否快）
                    Serial.println("[ASRDIAG] 测试 api.siliconflow.cn HTTP(80)...");
                    if (WiFi.status() != WL_CONNECTED) Serial.println("[ASRDIAG] WiFi未连接");
                    unsigned long t0 = millis();
                    WiFiClient c2;
                    c2.setTimeout(10);
                    bool ok = c2.connect("api.siliconflow.cn", 80);
                    Serial.printf("[ASRDIAG] connect %s 耗时 %lums\n", ok ? "OK" : "FAIL", (unsigned long)(millis() - t0));
                    if (ok) {
                        c2.println("GET /v1/models HTTP/1.0");
                        c2.println("Host: api.siliconflow.cn");
                        c2.println();
                        unsigned long t1 = millis();
                        int got = 0;
                        while (c2.connected() && millis() - t1 < 10000) {
                            if (c2.available()) { c2.read(); got++; }
                            else delay(2);
                        }
                        c2.stop();
                        Serial.printf("[ASRDIAG] 响应 %d 字节 耗时 %lums\n", got, (unsigned long)(millis() - t1));
                    }
                    Serial.println("[ASRDIAG] done");
                } else if (strncmp(cpSerialBuf, "#ASRTEST", 8) == 0) {
                    // 诊断：直接转写设备 SD 上的 /record/todo_0.wav（复用已有录音，验证转写链路与失败环节）
                    // 成功后顺带回填对应待办的转写文字
                    Serial.println("[ASRTEST] 开始转写 /record/todo_0.wav");
                    if (initSDCard() && SD.exists("/record/todo_0.wav")) {
                        File rf = SD.open("/record/todo_0.wav", FILE_READ);
                        if (rf) {
                            size_t sz = rf.size();
                            if (sz >= 44 && sz <= 2 * 1024 * 1024UL) {
                                uint8_t* ab = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
                                if (ab) {
                                    rf.read(ab, sz);
                                    char txt[512];
                                    bool ok = asrTranscribe(ab, sz, txt, sizeof(txt));
                                    Serial.printf("[ASRTEST] 转写%s: %s\n", ok ? "成功" : "失败", txt[0] ? txt : "(无文字)");
                                    // 成功后回填到匹配该录音的待办
                                    if (ok && txt[0]) {
                                        for (size_t i = 0; i < todoCount && i < TODO_MAX; i++) {
                                            if (strcmp(todoItems[i].audioFile, "/record/todo_0.wav") == 0) {
                                                strncpy(todoItems[i].asr, txt, sizeof(todoItems[i].asr) - 1);
                                                todoItems[i].asr[sizeof(todoItems[i].asr) - 1] = '\0';
                                                saveTodoListToSD();
                                                Serial.printf("[ASRTEST] 已回填待办%u: %s\n", (unsigned)i, txt);
                                                break;
                                            }
                                        }
                                    }
                                    free(ab);
                                } else Serial.println("[ASRTEST] PSRAM分配失败");
                            } else Serial.printf("[ASRTEST] 文件大小异常 %u\n", (unsigned)sz);
                            rf.close();
                        } else Serial.println("[ASRTEST] 打开文件失败");
                    } else Serial.println("[ASRTEST] 无 /record/todo_0.wav");
                    Serial.println("[ASRTEST] done");
                } else if (strncmp(cpSerialBuf, "#CP", 3) == 0) {
                    // 诊断：打印当前 CodingPlan 额度数据（cpData 内存值）
                    Serial.printf("[CP] 5h=%d%% 7d=%d%% mcp=%d%% conn=%d status=%s update=%s\n"
                                  "[CP] 5hReset=%s 7dReset=%s mcpReset=%s token=%ld main=%ld\n",
                                  cpData.quota_5h_percent, cpData.quota_7d_percent, cpData.quota_mcp_percent,
                                  (int)cpData.connected, cpData.plan_status, cpData.update_time,
                                  cpData.quota_5h_reset_time, cpData.quota_7d_reset_time, cpData.quota_mcp_reset_time,
                                  (long)cpData.daily_token_total, (long)cpData.daily_token_main);
                } else if (strncmp(cpSerialBuf, "#TODODUMP", 9) == 0) {
                    // 诊断：打印所有待办（含转写文字），验证语音转写是否回填
                    Serial.printf("[TODO] count=%d\n", (int)todoCount);
                    for (size_t i = 0; i < todoCount && i < TODO_MAX; i++) {
                        Serial.printf("[TODO] %d audio=%s rt=%s sec=%d text=%s asr=%s\n",
                            (int)i,
                            todoItems[i].audioFile[0] ? todoItems[i].audioFile : "-",
                            todoItems[i].recTime[0] ? todoItems[i].recTime : "-",
                            (int)todoItems[i].recSec,
                            todoItems[i].text,
                            todoItems[i].asr[0] ? todoItems[i].asr : "-");
                    }
                } else if (strncmp(cpSerialBuf, "#REM|", 5) == 0) {
                    // 添加定时提醒：#REM|HH:MM|内容
                    const char* p1 = cpSerialBuf + 5;
                    if (strlen(p1) >= 6 && p1[2] == ':' && p1[5] == '|') {
                        if (reminderCount < REMINDER_MAX) {
                            char hm[6] = {p1[0], p1[1], p1[2], p1[3], p1[4], 0};
                            memcpy(reminders[reminderCount].time, hm, 6);
                            strncpy(reminders[reminderCount].text, p1 + 6, sizeof(reminders[reminderCount].text) - 1);
                            reminders[reminderCount].text[sizeof(reminders[reminderCount].text) - 1] = '\0';
                            reminders[reminderCount].fired = false;
                            reminderCount++;
                            saveReminders();
                            Serial.printf("[REM] 已添加 %s %s\n", hm, reminders[reminderCount - 1].text);
                        } else {
                            Serial.println("[REM] 提醒已满");
                        }
                    } else {
                        Serial.println("[REM] 格式: #REM|HH:MM|内容");
                    }
                } else if (strncmp(cpSerialBuf, "#REMLIST", 8) == 0) {
                    Serial.printf("[REM] count=%d\n", reminderCount);
                    for (int i = 0; i < reminderCount; i++) {
                        Serial.printf("[REM] %d %s %s fired=%d\n", i, reminders[i].time, reminders[i].text, (int)reminders[i].fired);
                    }
                } else if (strncmp(cpSerialBuf, "#ASRSIM|", 8) == 0) {
                    // 模拟语音转写结果，自动测试语音命令（B）：#ASRSIM|今天天气
                    const char* simText = cpSerialBuf + 8;
                    Serial.printf("[ASRSIM] 输入: %s\n", simText);
                    bool handled = handleVoiceCommand(simText);
                    Serial.printf("[ASRSIM] handled=%d\n", (int)handled);
                } else if (strncmp(cpSerialBuf, "#REMCLEAR", 9) == 0) {
                    // 清空所有定时提醒（含 /reminders.txt）
                    reminderCount = 0;
                    if (initSDCard() && SD.exists("/reminders.txt")) SD.remove("/reminders.txt");
                    Serial.println("[REM] 已清空提醒");
                } else if (strncmp(cpSerialBuf, "#DNS", 4) == 0) {
                    // 诊断：对比测试多个域名解析
                    const char* hosts[] = {"www.ithome.com", "open.bigmodel.cn", "www.baidu.com", "www.qq.com"};
                    for (int i = 0; i < 4; i++) {
                        IPAddress ip;
                        bool ok = WiFi.hostByName(hosts[i], ip);
                        Serial.printf("[DNS] %s -> %s\n", hosts[i], ok ? ip.toString().c_str() : "FAIL");
                    }
                } else if (strncmp(cpSerialBuf, "#STANDBY", 8) == 0) {
                    // 诊断：强制立即进入待机（三键唤醒验证用；把闲置计时设为过去 → 待机条件立即成立）
                    lastActivityMs = millis() - IDLE_SLEEP_MS - 1;
                    Serial.println("[CMD] #STANDBY 已触发，下一轮 loop 进入待机");
                } else if (strncmp(cpSerialBuf, "#STATUS", 7) == 0) {
                    // 诊断：打印固件版本 + Coding Plan 配置状态（版本=编译时间，确认固件是否最新）
                    Serial.printf("[STATUS] fw=%s %s page=%d use_wifi=%d ssid=%s cookie_len=%d pollnow=%d last=%lu cfg_ok=%d\n",
                                  __DATE__, __TIME__, (int)currentPage,
                                  (int)cpUseWifi, cpWifiSsid[0], (int)strlen(cpZhipuCookie),
                                  (int)cpPollingNow, cpLastPollAt, (int)cpData.connected);
                } else if (strncmp(cpSerialBuf, "#TIME", 5) == 0) {
                    // 诊断：对比系统 time() 与 RTC 芯片当前值（定位日期不同步）
                    time_t now = time(nullptr);
                    struct tm tmi;
                    localtime_r(&now, &tmi);
                    auto rdt = M5.Rtc.getDateTime();
                    Serial.printf("[TIME] sys=%04d-%02d-%02d %02d:%02d:%02d rtc=%04d-%02d-%02d %02d:%02d:%02d\n",
                                  tmi.tm_year + 1900, tmi.tm_mon + 1, tmi.tm_mday, tmi.tm_hour, tmi.tm_min, tmi.tm_sec,
                                  rdt.date.year, rdt.date.month, rdt.date.date, rdt.time.hours, rdt.time.minutes, rdt.time.seconds);
                } else if (strncmp(cpSerialBuf, "#CFGREAD", 8) == 0) {
                    // 诊断：读取 SD 卡 config.ini 实际内容（token 只显示长度，防泄露）
                    if (initSDCard()) {
                        File rf = SD.open("/config.ini");
                        if (rf) {
                            Serial.println("--- config.ini ---");
                            while (rf.available()) {
                                String ln = rf.readStringUntil('\n');
                                ln.trim();
                                if (ln.startsWith("zhipu_cookie="))
                                    Serial.printf("zhipu_cookie=<%d chars>\n", (int)ln.length() - 13);
                                else
                                    Serial.println(ln);
                            }
                            rf.close();
                            Serial.println("--- end ---");
                        } else {
                            Serial.println("[ERR] config not found");
                        }
                    } else {
                        Serial.println("[ERR] no sd");
                    }
                }
                cpSerialLen = 0;
            }
            continue;
        }
        if (cpSerialLen < CP_SERIAL_BUF_LEN - 1) {
            cpSerialBuf[cpSerialLen++] = c;
        } else {
            cpSerialLen = 0;   // 超长丢弃
        }
    }
}

// 绘制进度条（8px 高；<70%绿 / 70-90%橙 / ≥90%红）
static void drawQuotaBar(int x, int y, int w, int percent) {
    uint32_t barColor = TFT_DARKGREEN;
    if (percent >= cpAlertThreshold) barColor = TFT_RED;
    else if (percent >= cpWarnThreshold) barColor = TFT_ORANGE;

    M5.Display.drawRoundRect(x, y, w, 8, 2, TFT_BLACK);
    int fillW = (w - 4) * percent / 100;
    if (fillW > 0) {
        M5.Display.fillRoundRect(x + 2, y + 2, fillW, 4, 2, barColor);
    }
}

// 绘制 Coding Plan 页面（上三卡大字号 + 下两栏，Spectra6 高饱和色）
void drawCodingPlanPage() {
    int x0 = MARGIN_X;
    int areaW = SCREEN_WIDTH - MARGIN_X * 2;
    // 是否有额度缓存数据（update_time 非 "--"）：有缓存就显示实际值，完全无数据才显示占位
    // （修复：conn 短暂为 0 不代表无数据，按 update_time 判断避免"图标显示但百分比 --"的矛盾）
    bool hasData = (strncmp(cpData.update_time, "--", 2) != 0);

    // ============ 上部：三张额度卡片（更大更粗，颜色更高饱和） ============
    int cardY = STATUS_BAR_HEIGHT + 4;
    int cardH = 172;
    int cardGap = 12;
    int cardW = (areaW - cardGap * 2) / 3;
    const char* const cardTitles[3] = {"5小时额度", "每周额度", "月额度"};
    const int percents[3] = {cpData.quota_5h_percent, cpData.quota_7d_percent, cpData.quota_mcp_percent};
    const char* const resets[3] = {cpData.quota_5h_reset_time, cpData.quota_7d_reset_time, cpData.quota_mcp_reset_time};
    // Spectra6 面板精确高饱和色（ED2208 红料偏暗→用亮橙红；蓝=EPD_BLUE；绿=EPD_GREEN）
    const uint32_t cardColors[3] = {
        M5.Display.color565(255, 140, 0),
        M5.Display.color565(100, 64, 255),
        M5.Display.color565(67, 138, 28)
    };
    for (int i = 0; i < 3; i++) {
        int cx = x0 + i * (cardW + cardGap);
        int cy = cardY;
        // 卡片白底 + 同色粗描边（更醒目）
        M5.Display.fillRoundRect(cx, cy, cardW, cardH, 12, TFT_WHITE);
        M5.Display.drawRoundRect(cx, cy, cardW, cardH, 12, cardColors[i]);

        // 顶部标题（16号粗体，同色）
        M5.Display.setFont(&fonts::efontCN_16_b);
        M5.Display.setTextColor(cardColors[i], TFT_WHITE);
        M5.Display.setTextDatum(textdatum_t::middle_center);
        M5.Display.drawString(cardTitles[i], cx + cardW / 2, cy + 20);

        // 环形饼图（更大更粗：rO=40 rI=22，环宽 18px 密度高）
        int cx2 = cx + cardW / 2, cy2 = cy + 84;
        int rO = 40, rI = 22;
        M5.Display.fillArc(cx2, cy2, rO, rI, 0, 360, TFT_LIGHTGREY);
        float a1 = -90.0f + percents[i] * 3.6f;
        if (a1 > 270.0f) a1 = 270.0f;
        M5.Display.fillArc(cx2, cy2, rO, rI, -90.0f, a1, cardColors[i]);

        // 中心百分比（efontCN_24_b 放大 2x=48px 大而显著）；有缓存显示实际值，无数据显示 --
        M5.Display.setFont(&fonts::efontCN_24_b);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(cardColors[i], TFT_WHITE);
        char pct[16];
        if (hasData) snprintf(pct, sizeof(pct), "%d%%", percents[i]);
        else snprintf(pct, sizeof(pct), "--");
        M5.Display.drawString(pct, cx2, cy2);
        M5.Display.setTextSize(1);

        // 底部重置时间（14号，更清晰）
        M5.Display.setFont(&fonts::efontCN_14_b);
        M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
        M5.Display.setTextDatum(textdatum_t::bottom_center);
        char resetBuf[24];
        const char* r = (resets[i][0] == '\0' || !hasData) ? "--" : resets[i];
        snprintf(resetBuf, sizeof(resetBuf), "重置 %s", r);
        M5.Display.drawString(resetBuf, cx + cardW / 2, cy + cardH - 6);
    }
    M5.Display.setTextDatum(textdatum_t::top_left);

    // ============ 下部：5:5 分栏（加高防文字遮盖） ============
    int lowY = cardY + cardH + 6;
    int lowH = (STATUS_BAR_HEIGHT + MAIN_AREA_HEIGHT - 4) - lowY;
    int halfW = areaW / 2;

    // 左下：今日Token消耗（数值 K/M 单位格式化更易读）
    int lx = x0, ly = lowY;
    M5.Display.fillRoundRect(lx, ly, halfW - 6, lowH, 10, TFT_WHITE);
    M5.Display.drawRoundRect(lx, ly, halfW - 6, lowH, 10, cardColors[1]);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(cardColors[1], TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::top_center);
    M5.Display.drawString("今日Token消耗", lx + (halfW - 6) / 2, ly + 8);
    // 总消耗数值（K/M 单位，24号放大2x=48px 显著）
    long tok = cpData.daily_token_total;
    char tokBuf[24];
    if (tok >= 1000000) snprintf(tokBuf, sizeof(tokBuf), "%.2fM", tok / 1000000.0);
    else if (tok >= 1000) snprintf(tokBuf, sizeof(tokBuf), "%.1fK", tok / 1000.0);
    else snprintf(tokBuf, sizeof(tokBuf), "%ld", tok);
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.drawString(tokBuf, lx + (halfW - 6) / 2, ly + 46);
    M5.Display.setTextSize(1);
    // 主模型占比（14号）
    long mainPct = 0;
    if (tok > 0) mainPct = cpData.daily_token_main * 100 / tok;
    M5.Display.setFont(&fonts::efontCN_14_b);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    char mainBuf[32];
    snprintf(mainBuf, sizeof(mainBuf), "主模型占比 %ld%%", mainPct);
    M5.Display.drawString(mainBuf, lx + (halfW - 6) / 2, ly + lowH - 24);
    M5.Display.setTextDatum(textdatum_t::top_left);

    // 右下：工具用量（比模型 token 更有意义的 plan 实时工作量）
    int rx = x0 + halfW + 6, ry = lowY;
    M5.Display.fillRoundRect(rx, ry, halfW - 6, lowH, 10, TFT_WHITE);
    M5.Display.drawRoundRect(rx, ry, halfW - 6, lowH, 10, cardColors[2]);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(cardColors[2], TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::top_center);
    M5.Display.drawString("工具用量", rx + (halfW - 6) / 2, ry + 8);
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setFont(&fonts::efontCN_14_b);
    char statusBuf[64];
    int yLine = ry + 44;
    // 联网搜索 MCP 次数（同色高亮）
    M5.Display.setTextColor(cardColors[2], TFT_WHITE);
    snprintf(statusBuf, sizeof(statusBuf), "联网搜索 %ld次", cpData.tool_search_count);
    M5.Display.drawString(statusBuf, rx + 12, yLine); yLine += 26;
    // 网页读取 MCP 次数
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    snprintf(statusBuf, sizeof(statusBuf), "网页读取 %ld次", cpData.tool_webread_count);
    M5.Display.drawString(statusBuf, rx + 12, yLine);
    // 更新时间（底部小字）
    bool overLimit = (cpData.quota_5h_percent >= cpAlertThreshold);
    M5.Display.setFont(&fonts::efontCN_12_b);
    M5.Display.setTextColor(overLimit ? TFT_RED : TFT_NAVY, TFT_WHITE);
    snprintf(statusBuf, sizeof(statusBuf), "%s%s", overLimit ? "⚠超限 " : "", cpData.update_time);
    M5.Display.drawString(statusBuf, rx + 12, ry + lowH - 16);
    M5.Display.setTextDatum(textdatum_t::top_left);
}

// ====================================================================
// 关机/重启菜单（二维码页长按C 弹出）
// 电源键是硬件开关（软件无法捕获双击），此处提供软重启入口。
// ====================================================================
// ===== 待机页（纯白 · 日期+天气，无时钟）=====
// 规格（用户指定）：背景纯白；只放当天日期+天气（用缓存，不刷新）；
// 数字用 Font8 电子粗体（75px 实心粗数字，epd_text 彩色抖动下稳定）；
// 年份=红、日期=深蓝。进入待机画一次，之后不定时刷新（省电 + 墨水屏寿命）
// ===== 待机页 v9（纯白 · 大日期 + 有趣元素，字号全部放大）=====
// 年份 Font8 红 75px；日期「8月11日」= Font8 大数字 + efontCN_24 中文（深蓝）；
// 顶栏去掉周几（只 ●WiFi + ●电量）；星期红色圆角块；农历+宜忌 16号；天气图标+24号
// 颜色：年份用 TFT_RED（用户规格「红」；勿用深紫 0x4810，Spectra6 调色板最近邻会匹配黑/蓝导致偏暗）
void drawStandbyPage() {
    const int W = SCREEN_WIDTH, H = SCREEN_HEIGHT;
    M5.Display.fillRect(0, 0, W, H, TFT_WHITE);

    auto dt = M5.Rtc.getDateTime();
    int yr = dt.date.year < 2024 ? 2026 : dt.date.year;
    static const char* const WD3[7] = {"日","一","二","三","四","五","六"};
    struct tm tm0 = {0};
    tm0.tm_year = yr - 1900; tm0.tm_mon = dt.date.month - 1; tm0.tm_mday = dt.date.date;
    mktime(&tm0);
    int wd3 = (tm0.tm_wday + 7) % 7;
    lunarCalc(yr, dt.date.month, dt.date.date, calLunar, sizeof(calLunar), calGanzhi, sizeof(calGanzhi));

    // ---- 年份：2026年（红 Font8 大数字 + 中文"年" efontCN_24 放大3x，中线对齐）----
    char yBuf[8];
    snprintf(yBuf, sizeof(yBuf), "%d", yr);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setFont(&fonts::Font8);
    M5.Display.setTextSize(1);
    int yW = M5.Display.textWidth(yBuf);
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextSize(3);   // 24px×3=72px，与 Font8(75px) 等高
    int ycw3 = M5.Display.textWidth("年");
    int yTotal = yW + ycw3;
    int yX = (W - yTotal) / 2;
    int yY = 78;
    M5.Display.setFont(&fonts::Font8);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_RED, TFT_WHITE);
    M5.Display.drawString(yBuf, yX + yW / 2, yY);
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(TFT_RED, TFT_WHITE);
    M5.Display.drawString("年", yX + yW + ycw3 / 2, yY);
    M5.Display.setTextSize(1);

    // ---- 日期：8月11日（数字 Font8 75px + 中文 efontCN_24 放大3x=72px，中线对齐统一）----
    char mBuf[4], dBuf[4];
    snprintf(mBuf, sizeof(mBuf), "%d", dt.date.month);
    snprintf(dBuf, sizeof(dBuf), "%d", dt.date.date);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.setFont(&fonts::Font8);
    M5.Display.setTextSize(1);
    int mW2 = M5.Display.textWidth(mBuf);
    int dW2 = M5.Display.textWidth(dBuf);
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextSize(3);   // 24px×3=72px，与 Font8(75px) 接近等高，大小才一致
    int cw3 = M5.Display.textWidth("月");
    int total = mW2 + cw3 + dW2 + cw3;
    int sx = (W - total) / 2;
    int dY = 166;
    // 数字（Font8 不缩放）
    M5.Display.setFont(&fonts::Font8);
    M5.Display.setTextSize(1);
    M5.Display.drawString(mBuf, sx + mW2 / 2, dY);
    M5.Display.drawString(dBuf, sx + mW2 + cw3 + dW2 / 2, dY);
    // 中文月/日（放大3倍，与数字中线对齐）
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextSize(3);
    M5.Display.drawString("月", sx + mW2 + cw3 / 2, dY);
    M5.Display.drawString("日", sx + mW2 + cw3 + dW2 + cw3 / 2, dY);
    M5.Display.setTextSize(1);

    // ---- 星期：红色圆角块反白（有趣焦点）----
    char wkBuf[16];
    snprintf(wkBuf, sizeof(wkBuf), "星期%s", WD3[wd3]);
    M5.Display.setFont(&fonts::efontCN_24_b);
    int wkW = M5.Display.textWidth(wkBuf) + 48;
    int wkX = (W - wkW) / 2;
    M5.Display.fillRoundRect(wkX, 216, wkW, 48, 12, TFT_RED);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextColor(TFT_WHITE, TFT_RED);
    M5.Display.drawString(wkBuf, W / 2, 240);

    // ---- 农历 + 宜忌（16号放大）----
    // 缓冲需 ≥ 农历15字节 + " · "×2 + 宜/忌各30字节 = 81字节；原56截断导致"忌"显示不全
    char lrBuf[96];
    snprintf(lrBuf, sizeof(lrBuf), "%s · %s · %s", calLunar, calYi, calJi);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_NAVY, TFT_WHITE);
    M5.Display.drawString(lrBuf, W / 2, 288);

    // ---- 天气：图标 + 24号文字 ----
    char wxBuf[32];
    snprintf(wxBuf, sizeof(wxBuf), "%s  %d°/%d°", weatherText, weatherHigh, weatherLow);
    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    int wxW = M5.Display.textWidth(wxBuf);
    M5.Display.drawString(wxBuf, W / 2, 324);
    drawWeatherIcon(W / 2 - wxW / 2 - 22, 318, 15, weatherText);

    // ---- 提示（16号放大）----
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_DARKGREY, TFT_WHITE);
    M5.Display.drawString("▲ 按任意键唤醒", W / 2, 356);

    M5.Display.setTextDatum(textdatum_t::top_left);
}

void drawPowerMenu() {
    int startY = STATUS_BAR_HEIGHT;
    int areaW = SCREEN_WIDTH - MARGIN_X * 2;
    int areaH = MAIN_AREA_HEIGHT;

    M5.Display.fillRect(MARGIN_X, startY, areaW, areaH, TFT_NAVY);
    M5.Display.setTextDatum(textdatum_t::middle_center);

    M5.Display.setFont(&fonts::efontCN_24_b);
    M5.Display.setTextColor(TFT_RED, TFT_NAVY);
    M5.Display.drawString("关机 / 重启", SCREEN_WIDTH / 2, startY + 60);
    M5.Display.setFont(&fonts::efontCN_16_b);
    M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
    M5.Display.drawString("电源键 = 硬件关机", SCREEN_WIDTH / 2, startY + 120);
    M5.Display.drawString("A键 = 软重启", SCREEN_WIDTH / 2, startY + 155);
    M5.Display.drawString("C键 = 返回", SCREEN_WIDTH / 2, startY + 190);
    M5.Display.setFont(&fonts::efontCN_14_b);
    M5.Display.setTextColor(TFT_ORANGE, TFT_NAVY);
    M5.Display.drawString("（本机电源键为硬件开关）", SCREEN_WIDTH / 2, startY + 235);

    M5.Display.setTextDatum(textdatum_t::top_left);
}

// ====================================================================
// 刷新管控核心：全屏渲染唯一入口（事件驱动，白名单制）
// forceQuality=true  → 请求全彩刷新（受 30 分钟冷却限制，冷却期内自动降级黑白快刷）
// forceQuality=false → 黑白快刷（日常翻页/切页/数字更新）
// ====================================================================
void renderScreen(bool forceQuality) {
    unsigned long now = millis();

    // 全彩刷新冷却控制：两次全彩最小间隔 30 分钟，不足则降级为黑白快刷
    bool useQuality = false;
    if (forceQuality) {
        if (isFirstBoot || (now - lastFullRefreshTime >= FULL_REFRESH_COOLDOWN_MS)) {
            useQuality = true;
            lastFullRefreshTime = now;
            isFirstBoot = false;
            cooldownBlocked = false;
        } else {
            cooldownBlocked = true;
            useQuality = false;
        }
    } else {
        cooldownBlocked = false;
        useQuality = false;
    }

    // 刷新模式：待机赛博时钟需彩色(RGB抖动)用 epd_text（比 epd_quality 快）；
    // 正常页全彩(受冷却)用 epd_quality，日常黑白用 epd_fast
    M5.Display.setEpdMode(standbyMode ? epd_mode_t::epd_text
                          : (useQuality ? epd_mode_t::epd_quality : epd_mode_t::epd_fast));

    // startWrite/endWrite 包裹绘制；因已关闭 auto_display，endWrite 不会触发刷新
    M5.Display.startWrite();

    // 关键：每次渲染前先全区域清屏为白，避免上一页内容残留
    //（Spectra 6 快刷模式只更新变化的像素，不清屏会导致旧内容叠加）
    M5.Display.fillRect(0, STATUS_BAR_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - STATUS_BAR_HEIGHT, TFT_WHITE);

    // 顶栏与底栏也先清为各自底色，防止切页时不同页顶栏底色/文字残留
    M5.Display.fillRect(0, 0, SCREEN_WIDTH, STATUS_BAR_HEIGHT, TFT_NAVY);
    M5.Display.fillRect(0, STATUS_BAR_HEIGHT + MAIN_AREA_HEIGHT, SCREEN_WIDTH, ACTION_BAR_HEIGHT, TFT_NAVY);

    // 待机模式：画待机页并返回（省电，不再画顶栏/内容）
    if (standbyMode) {
        drawStandbyPage();
        M5.Display.endWrite();
        M5.Display.display();
        return;
    }

    drawTopBar();

    switch (currentPage) {
        case 0: drawCalendarPage();   break;   // 日历黄历天气
        case 1: drawNewsPage();       break;   // 资讯早报
        case 2: drawEbbinghausPage(); break;   // 考点闪卡
        case 3:
            cpAlertAcknowledged = true;   // 用户已看到 Coding Plan 页具体内容 → 解除超限/预警报警
            drawCodingPlanPage(); break;   // 智谱Coding Plan 额度监控
        case 4: drawTodoPage();       break;   // 语音速记待办
        case 5: drawDashboardPage();  break;   // 状态仪表盘
        case 6:                       // 微信二维码（最后一页；长按C弹关机菜单）
            if (powerMenuShown) drawPowerMenu();
            else drawQRCodePage();
            break;
    }

    drawActionBar();

    M5.Display.endWrite();

    // 唯一一次显式刷屏（auto_display 已关闭，不会重复刷新）
    M5.Display.display();
}

// ====================================================================
// 九、Arduino 标准 setup 与 loop
// ====================================================================

void setup() {
    Serial.begin(115200);                   // 显式初始化串口（USB CDC）
    delay(100);

    auto cfg = M5.config();
    cfg.clear_display = false;              // 禁止 M5.begin 内部自动清屏刷新（避免启动即触发 EPD 刷新）
    M5.begin(cfg);

    // 初始化 RGB 状态灯（Adafruit NeoPixel 驱动 WS2812，GPIO21×2颗）
    rgbStrip.begin();
    rgbStrip.setBrightness(180);
    rgbStrip.show();
    ledSetAll(0, 0, 0);

    // 扬声器音量全开（ES8311 DAC 增益最大，保证朗读/回放/提示音清晰）
    M5.Speaker.setVolume(255);
    M5.Speaker.setAllChannelVolume(255);

    // 自研按键检测：直接读 GPIO，不受墨水屏刷新阻塞影响（彻底修复 B 键等短按失效）
    initCustomButtons();

    // ===== 刷新管控核心：关闭 auto_display =====
    // Panel_FrameBufferBase 默认 _auto_display=true，会导致每次 endWrite() 自动全屏刷新。
    // 此处显式关闭，让屏幕刷新完全由本程序的白名单机制控制（仅在 display() 时发生一次）。
    M5.Display.setAutoDisplay(false);

    Wire.begin();
    readSHT40();
    lastSensorReadTime = millis();

    initSDCard();
    // 考点卡片数组移到 PSRAM（156KB，腾出内部 RAM 给 WiFi/TLS，避免 esp-aes/esp-sha OOM）
    kpCards = (RuntimeCard*)heap_caps_malloc(KP_MAX_CARDS * sizeof(RuntimeCard), MALLOC_CAP_SPIRAM);
    if (!kpCards) Serial.println("[CARD] kpCards PSRAM 分配失败，使用内置考点");
    loadCardsFromSD();        // 先加载 SD 考点卡片（kpCount 决定后续用 SD 还是内置）
    loadTodoFromSD();
    loadReminders();          // 加载定时提醒（/reminders.txt）
    loadCodingPlanConfig();   // 读取 Coding Plan 配置（缺失则默认）
    loadMemoryStates();       // 依赖 cardTotal()，必须在 loadCardsFromSD 之后
    initTTS();                // 中文语音合成（模型来自 flash voice_data 分区；失败静默降级）

    // 时间同步：先用编译时间无条件写入 RTC（修正 RTC 中的旧/错误时间），
    // 再尝试 NTP 精确同步（若 WiFi 可用则覆盖为准确北京时间）
    initRtcFromBuildTime();
    initWiFiNTP();

    M5.Display.setRotation(1);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);

    // 开机首屏：黑白快刷（epd_fast 约 2~3 秒），避免全彩刷新(16秒+)导致开机长时间按键无响应
    renderScreen(false);
    lastScreenUpdate = millis();
    lastActivityMs = millis();   // 开机即视为活动：闲置 5 分钟自动待机（否则 lastActivityMs=0 永不待机）
}

// ====================================================================
// loop 主循环：严格事件驱动，绝无无理由自动刷新
// 刷新白名单：
//   1. 用户按键操作（翻页/切页/标记/手动刷新）
//   2. 预设定时任务（番茄钟每分钟数字、每日 8 点资讯）
//   3. 设备状态变更（传感器采样但仅当数值显著变化才刷）
// 其余场景一律禁止调用 renderScreen
// ====================================================================
void loop() {
    M5.update();               // M5Unified 内部外设更新
    updateCustomButtons();     // 自研按键：电平边沿检测，不受刷新阻塞影响
    // ★ 唤醒抑制：light sleep 唤醒用的按键动作（按下+松开）不应被当成一次点击/长按，
    // 否则长按 C 唤醒会误触发 C 的功能（切页跳转到语音待办）。唤醒后第一轮清空事件并重置电平基准。
    if (suppressWakeClick) {
        suppressWakeClick = false;
        keyA.clickPending = keyB.clickPending = keyC.clickPending = false;
        keyA.holdPending = keyB.holdPending = keyC.holdPending = false;
        keyA.prevLevel = !digitalRead(BTN_A_GPIO);
        keyB.prevLevel = !digitalRead(BTN_B_GPIO);
        keyC.prevLevel = !digitalRead(BTN_C_GPIO);
    }
    unsigned long now = millis();

    // 温湿度采样：仅记录数值，不触发刷新（避免频繁刷新屏幕）；待机中跳过（省电）
    if (!standbyMode && now - lastSensorReadTime >= SENSOR_READ_INTERVAL_MS) {
        lastSensorReadTime = now;
        readSHT40();
    }

    // 串口接收 Coding Plan 数据（仅更新内存变量，绝不刷屏）
    handleCodingPlanSerial();

    // 按键事件待处理时，跳过阻塞型定时任务（智谱/RSS/天气），让按键优先响应
    bool keyBusy = keyA.clickPending || keyB.clickPending || keyC.clickPending ||
                   keyA.holdPending || keyB.holdPending || keyC.holdPending;

    // 任意按键 = 活动；待机中任意键唤醒（恢复频率，页面由按键处理刷新）
    if (keyBusy) {
        lastActivityMs = now;
        if (standbyMode) {
            wakeFromStandby();
            Serial.println("[WAKE] 按键唤醒");
        }
    }

    // 闲置自动待机（省电）：无操作超时 → 待机页 + 降频 + 断WiFi；任意键唤醒
    // 转写排队中（asrPending）禁止待机：否则录音后 5 分钟无操作进 light sleep，转写任务永不执行
    if (!standbyMode && !asrPending && !isRecordingNow && !voiceCmdMode && !voicePlaying && lastActivityMs != 0 && now - lastActivityMs > IDLE_SLEEP_MS) {
        standbyMode = true;
        setCpuFrequencyMhz(80);
        if (WiFi.status() == WL_CONNECTED) { WiFi.disconnect(); wifiConnected = false; }
        WiFi.mode(WIFI_OFF);  // ★ 彻底关闭 WiFi 无线电（仅 disconnect 会残留 modem 供电，省 20~35mA）
        cpWifiOk = false;   // 重置智谱轮询连接缓存，唤醒后可重新连接
        Serial.println("[STANDBY] 进入待机(降频+关WiFi无线电)");
        renderScreen(false);   // 待机页纯白日期+天气（epd_text 彩色），进入画一次
        // ===== ★ 轻睡眠：CPU 停转（省最大功耗 40~80mA），按键 GPIO 唤醒 =====
        // light sleep 保留 RAM 状态，唤醒瞬时继续；待机功耗从 ~80mA 降到 ~2mA
        // ⚠️ USB 修复：USB-Serial/JTAG 外设进 light sleep 会停摆 → 主机 COM 数据通路卡死
        // （打开报 PermissionError/设备没有发挥作用），需物理重插 USB 才能恢复。
        // 因此仅当 USB 未插入（纯电池场景）才进 light sleep；USB 插着（桌面插电/开发调试）
        // 则跳过 light sleep，只降频+关WiFi，串口保持可用随时可刷机/调试。
        if (HWCDC::isPlugged()) {
            Serial.println("[STANDBY] USB 已插入，跳过 light sleep（保持串口可用）");
        } else {
            // 三键 RTC 上拉：仅 INPUT_PULLUP 在 light sleep 时 RTC 域上拉可能失效→GPIO9/10 浮空
            // 导致 EXT1 ANY_LOW 只有 GPIO1(C键) 能唤醒；显式 RTC 上拉保证睡眠期间输入稳定
            rtc_gpio_pullup_en(BTN_A_GPIO); rtc_gpio_pulldown_dis(BTN_A_GPIO);
            rtc_gpio_pullup_en(BTN_B_GPIO); rtc_gpio_pulldown_dis(BTN_B_GPIO);
            rtc_gpio_pullup_en(BTN_C_GPIO); rtc_gpio_pulldown_dis(BTN_C_GPIO);
            esp_sleep_enable_ext1_wakeup((1ULL << BTN_C_GPIO) | (1ULL << BTN_B_GPIO) | (1ULL << BTN_A_GPIO), ESP_EXT1_WAKEUP_ANY_LOW);
            Serial.printf("[SLEEP] A=%d B=%d C=%d\n", digitalRead(BTN_A_GPIO), digitalRead(BTN_B_GPIO), digitalRead(BTN_C_GPIO));
            esp_light_sleep_start();   // 阻塞直到按键唤醒
            Serial.printf("[WAKE] 轻睡眠唤醒 cause=%d ext1=0x%llx\n", (int)esp_sleep_get_wakeup_cause(), (unsigned long long)esp_sleep_get_ext1_wakeup_status());
            wakeFromStandby();         // 恢复 240MHz + 标记 WiFi 重连
        }
    }
    // 待机页不定时刷新：只显示当天日期+天气缓存（无时钟），省电 + 保护墨水屏寿命

    // 待机唤醒后延迟重连 WiFi：等按键处理完再连（不阻塞按键响应），避开录音/上传/新闻加载
    if (wifiReconnectPending && !keyBusy && !isRecordingNow && !voiceCmdMode && !fileReceiving && !newsLoading) {
        wifiReconnectPending = false;
        bool wasConn = wifiConnected;
        reconnectWifiFromConfig();            // 成功 → wifiConnected=true + NTP 同步
        if (wifiConnected) {
            cpLastPollAt = 0;                 // 唤醒后立即拉一次 CodingPlan 额度
            if (!wasConn) renderScreen(false); // 顶栏"离线"→"WiFi"：轻量刷新一次
        } else {
            wifiRetryAt = now + 30000UL;      // 首次重连失败：30 秒后自动重试（热点晚开/切换中）
        }
    }
    // WiFi 重连失败定时重试（非阻塞；连上后与唤醒同路径：cpLastPollAt=0 立即拉额度）
    if (!wifiReconnectPending && !wifiConnected && wifiRetryAt != 0 && now >= wifiRetryAt
        && !standbyMode && !isRecordingNow && !voiceCmdMode && !fileReceiving && !newsLoading && !keyBusy) {
        wifiRetryAt = 0;
        wifiReconnectPending = true;
    }

    // WiFi 直连智谱轮询（按配置间隔；仅更新内存，绝不刷屏，不阻塞主循环）
    // 开机首次立即轮询一次（cpLastPollAt==0），之后按 poll_interval_sec 周期轮询
    // 录音/上传文件/待机期间禁止轮询（避免阻塞 loop 导致录音丢采样/串口溢出/待机耗电）
    if (cpUseWifi && !standbyMode && !isRecordingNow && !voiceCmdMode && !fileReceiving && !keyBusy && (cpLastPollAt == 0 || now - cpLastPollAt >= (unsigned long)cpPollIntervalSec * 1000UL)) {
        cpLastPollAt = now;
        pollZhipuCodingPlan();
    }

    // 录音分块采样（主循环，每块约250ms；录音期间已禁WiFi轮询）
    processRecording();
    processVoiceCommand();   // 全局语音命令录音（与待办录音互斥）

    // 非阻塞回放轮询：播放完成或超时(60s)自动停止释放缓冲
    // 注意：playWav 启动后 isPlaying() 可能延迟变 true，给 800ms 启动宽限避免误判"播放结束"而瞬间停止
    if (voicePlaying && !isRecordingNow) {
        if (now - voicePlayStart > 800 && !M5.Speaker.isPlaying()) {
            stopVoicePlayback();   // 播放确实结束（已过启动宽限且不在播）
        } else if (now - voicePlayStart > 60000UL) {
            stopVoicePlayback();   // 超时保护
        }
    }

    // 早报定时更新：开机首次联网拉取 + 每天 8 点（拉取阻塞约5-12秒；按键待处理时跳过让按键优先）
    if (wifiConnected && !isRecordingNow && !voiceCmdMode && !fileReceiving && !newsLoading && !voicePlaying && !keyBusy) {
        struct tm tmi;
        if (getLocalTime(&tmi, 5)) {
            if (newsCount == 0) {
                // 开机首次 + 失败后每10分钟自动重试（修复M5: 原首次失败后不再重试）
                if (newsUpdatedAt == 0 || millis() - newsUpdatedAt > 600000UL) {
                    newsUpdatedAt = millis();
                    fetchDailyNews();
                }
            } else if (tmi.tm_hour == 8 && (tmi.tm_mday != newsLastDay || tmi.tm_mon != newsLastMonth)) {
                newsLastDay = tmi.tm_mday;
                newsLastMonth = tmi.tm_mon;
                fetchDailyNews();
            }
        }
    }

    // 天气定时拉取：成功3小时/失败5分钟重试（真实天气 Open-Meteo；按键待处理时跳过）
    if (wifiConnected && !isRecordingNow && !voiceCmdMode && !fileReceiving && !newsLoading && !voicePlaying && !keyBusy) {
        unsigned long wxRetry = weatherSuccess ? 3UL * 3600 * 1000UL : 5UL * 60 * 1000UL;
        if (weatherUpdatedAt == 0 || millis() - weatherUpdatedAt > wxRetry) {
            weatherUpdatedAt = millis();
            fetchWeather();
        }
    }

    // 定时提醒检查：每分钟匹配 RTC 时间，到点 TTS 播报 + 屏幕提示（待机/录音/回放中不打断）
    {
        static int lastRemMin = -1;
        auto rdt = M5.Rtc.getDateTime();
        int curMin = rdt.time.hours * 60 + rdt.time.minutes;
        if (lastRemindDay != rdt.date.date) {   // 跨天重置
            lastRemindDay = rdt.date.date;
            for (int i = 0; i < reminderCount; i++) reminders[i].fired = false;
        }
        if (curMin != lastRemMin) {
            lastRemMin = curMin;
            if (!standbyMode && !isRecordingNow && !voiceCmdMode && !voicePlaying) {
                char hm[6];
                snprintf(hm, sizeof(hm), "%02d:%02d", rdt.time.hours, rdt.time.minutes);
                for (int i = 0; i < reminderCount; i++) {
                    if (!reminders[i].fired && strcmp(reminders[i].time, hm) == 0) {
                        reminders[i].fired = true;
                        saveReminders();
                        char txt[96];
                        snprintf(txt, sizeof(txt), "提醒，%s", reminders[i].text);
                        ttsSpeak(txt);
                        snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[提醒] %s", reminders[i].text);
                        memoryNoticeTime = millis();
                        Serial.printf("[REM] 触发 %s %s\n", hm, reminders[i].text);
                        renderScreen(false);
                        break;
                    }
                }
            }
        }
    }

    // 语音转写：启动独立任务联网转写（任务只用网络不碰 SD/EPD，不阻塞主循环；待机时不转写）
    if (asrPending && !asrRunning && !standbyMode && !keyBusy && !isRecordingNow && !voiceCmdMode && !voicePlaying && !fileReceiving && !newsLoading) {
        asrRunning = true;
        BaseType_t rc = xTaskCreatePinnedToCore(asrTaskEntry, "asrTask", 12288, NULL, 1, NULL, 0);
        if (rc != pdPASS) {
            Serial.println("[ASR] 任务创建失败");
            asrRunning = false;
            asrPending = false;
            if (asrAudioBuf) { free(asrAudioBuf); asrAudioBuf = nullptr; asrAudioLen = 0; }
        }
    }
    // 转写结果消费：语音命令识别（方案B）
    // 仅当转写文字含命令前缀「小彩」才尝试识别为命令；否则一律存为普通待办。
    // 根治"正常录音含关键词被误判/误删"：如「打开」「天气」「提醒」在普通句子里出现也不触发命令。
    if (asrResultReady) {
        asrResultReady = false;
        if (asrResultIdx == -1) {
            // 全局语音命令结果：直接识别命令（最小路径——命令内部已 renderScreen 一次刷屏进页）
            if (asrResult[0] == '\0') {
                snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "识别失败，请重试");
                memoryNoticeTime = millis();
                Serial.println("[CMD] 语音转写失败");
            } else if (handleVoiceCommand(asrResult)) {
                Serial.printf("[CMD] 语音命令已执行: %s\n", asrResult);
            } else {
                snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "未识别指令");
                memoryNoticeTime = millis();
                Serial.printf("[CMD] 未识别: %s\n", asrResult);
            }
        } else if (asrResultIdx >= 0 && asrResultIdx < (int)todoCount) {
            bool hasWake = (strstr(asrResult, "小彩") != nullptr);   // 命令前缀「小彩」
            bool isCmd = false;
            if (hasWake) isCmd = handleVoiceCommand(asrResult);      // 含前缀才尝试命令匹配
            if (hasWake && !isCmd) {
                // 有「小彩」前缀但未匹配任何命令 → 仍存为待办，去掉前缀字保持文字干净
                char* p = strstr(asrResult, "小彩");
                if (p) {
                    size_t rest = strlen(p + 6);
                    memmove(p, p + 6, rest + 1);          // 去掉"小彩"(UTF-8 每字3字节=6字节)
                    while (p[0] == ' ') memmove(p, p + 1, strlen(p + 1) + 1);   // 去前导空格
                }
                strncpy(todoItems[asrResultIdx].asr, asrResult, sizeof(todoItems[asrResultIdx].asr) - 1);
                todoItems[asrResultIdx].asr[sizeof(todoItems[asrResultIdx].asr) - 1] = '\0';
                saveTodoListToSD();
                Serial.printf("[ASR] 回填待办%d: %s\n", asrResultIdx, asrResult);
                if (currentPage == 4) renderScreen(false);
            } else if (isCmd) {
                // 已作为语音命令执行：删除刚创建的语音待办条目（含录音文件）
                Serial.printf("[CMD] 语音命令执行，撤销待办%d\n", asrResultIdx);
                if (todoItems[asrResultIdx].audioFile[0] && initSDCard() && SD.exists(todoItems[asrResultIdx].audioFile)) {
                    SD.remove(todoItems[asrResultIdx].audioFile);
                }
                for (size_t k = (size_t)asrResultIdx; k + 1 < todoCount; k++) todoItems[k] = todoItems[k + 1];
                todoCount--;
                if (todoIndex >= todoCount && todoCount > 0) todoIndex = todoCount - 1;
                saveTodoListToSD();
            } else {
                // 无命令前缀：正常待办，回填转写文字
                strncpy(todoItems[asrResultIdx].asr, asrResult, sizeof(todoItems[asrResultIdx].asr) - 1);
                todoItems[asrResultIdx].asr[sizeof(todoItems[asrResultIdx].asr) - 1] = '\0';
                saveTodoListToSD();
                Serial.printf("[ASR] 回填待办%d: %s\n", asrResultIdx, asrResult);
                if (currentPage == 4) renderScreen(false);
            }
        }
    }

    // ===== 定时白名单任务 =====
    // 1) 番茄钟：仅在仪表盘页(5)且番茄运行中，每分钟用黑白快刷更新一次倒计时数字
    if (currentPage == 5 && pomodoroRunning && !isRecordingNow && !voiceCmdMode) {   // 修复M6: 录音中不刷屏/播报打断采样
        uint32_t elapsedMin = (now - pomodoroStartMillis) / 60000UL;
        uint32_t targetMin = pomodoroInRest ? POMODORO_REST_MIN : POMODORO_FOCUS_MIN;
        if (elapsedMin >= targetMin) {
            // 番茄完成：状态变更 → 黑白快刷一次 + 语音播报
            bool wasFocus = !pomodoroInRest;   // 完成前是否专注阶段
            if (wasFocus) {
                pomodoroDoneCount++;      // 完成一个专注番茄
                todayReviewedCount++;     // 计入今日复习
            }
            pomodoroInRest = !pomodoroInRest;
            pomodoroStartMillis = now;
            pomodoroRemainMin = pomodoroInRest ? POMODORO_REST_MIN : POMODORO_FOCUS_MIN;
            lastScreenUpdate = now;
            renderScreen(false);
            // 番茄切换语音播报（界面刷新后再播，避免叠加卡顿）
            if (wasFocus) ttsSpeak("番茄钟完成，休息一下。");
            else ttsSpeak("休息结束，开始专注。");
        } else if ((now - lastScreenUpdate >= 60000UL)) {
            // 每分钟数字变化 → 黑白快刷
            lastScreenUpdate = now;
            pomodoroRemainMin = targetMin - elapsedMin;
            renderScreen(false);
        }
    } else {
        // 非仪表盘页时保持 lastScreenUpdate 新鲜，避免切回仪表盘时误触发
        lastScreenUpdate = now;
    }

    // ===== 全局状态灯系统（不刷屏，仅驱动 RGB 状态灯） =====
    // 多报警轮流机制：同时有多个报警时，每个轮流亮 ALERT_ROTATE_MS 后切下一个，
    // 避免高优先级报警长期独占、低优先级永远看不到。
    // 颜色语义：
    //   低电量红(0xFF0000) / CP超限淡粉(0xFF66AA) / CP预警橙(0xFF8C00)
    //   番茄黄(0xFFFF00) / planning青(0x00FFFF) / coding绿(0x00FF66) / waiting橙快闪(0xFF8C00)
    // 瞬时操作反馈独占（不参与轮流）：录音蓝灯 > WiFi拉取青灯 > 按键白灯 > #LED测试灯
    // 解除规则：
    //   - CP超限/预警：进入 Coding Plan 页看到内容(cpAlertAcknowledged)即解除；额度降回正常重置
    //   - 番茄：专注时间到(自动切休息) 或 用户按B关闭(pomodoroRunning=false) 才停
    //   - 低电量：电量>=20% 自动解除；CP任务状态：任务完成自然消失
    static uint32_t ledToggle = 0;
    static uint32_t ledFlashMs = 1000;   // 当前状态的闪烁周期
    static int alertIdx = 0;             // 当前轮到第几个报警
    static unsigned long alertSwitchAt = 0;   // 下次切换时刻
    int battLevel = M5.Power.getBatteryLevel();
    bool lowBatt = battLevel >= 0 && battLevel < 20;

    // 收集所有激活的报警源（颜色, 闪烁周期ms）
    struct Alert { uint32_t color; uint32_t period; };
    Alert alerts[6];
    int nAlert = 0;

    bool cpAlert5h = (cpData.quota_5h_percent >= cpAlertThreshold) && !cpAlertAcknowledged;
    bool cpWarn5h  = (cpData.quota_5h_percent >= cpWarnThreshold) && !cpAlertAcknowledged;
    const char* cpStatus = cpData.plan_status;

    if (lowBatt)                           alerts[nAlert++] = {0xFF0000, 1000};  // 低电量红
    if (cpAlert5h)                         alerts[nAlert++] = {0xFF66AA, 1000};  // 超限淡粉
    if (cpStatus[0] && strcmp(cpStatus, "planning") == 0)
                                           alerts[nAlert++] = {0x00FFFF, 500};   // 规划青
    if (cpStatus[0] && strcmp(cpStatus, "coding") == 0)
                                           alerts[nAlert++] = {0x00FF66, 500};   // 编码绿
    if (cpStatus[0] && strcmp(cpStatus, "waiting_confirm") == 0)
                                           alerts[nAlert++] = {0xFF8C00, 350};   // 待确认橙快闪
    if (cpWarn5h)                          alerts[nAlert++] = {0xFF8C00, 1000};  // 预警橙
    if (pomodoroRunning && !pomodoroInRest) alerts[nAlert++] = {0xFFFF00, 1000}; // 番茄黄

    bool ledBlink = false;
    uint32_t ledColor = 0;
    ledFlashMs = 1000;

    // 瞬时操作反馈独占（最高优先，不参与轮流）
    if (isRecordingNow || voiceCmdMode) {
        ledBlink = true; ledColor = 0x0066FF; ledFlashMs = 350;    // 蓝灯快闪（录音/语音命令）
    } else if (asrPending && asrPendingIdx == -1) {
        ledBlink = true; ledColor = 0xFF8C00; ledFlashMs = 600;    // 命令转写中 橙灯慢闪（区分识别中）
    } else if (cpWifiBusy) {
        ledBlink = true; ledColor = 0x00FFFF; ledFlashMs = 500;    // 青色慢闪（WiFi连接/智谱拉取中）
    } else if (nAlert > 0) {
        // 轮流切换：每个报警亮 ALERT_ROTATE_MS 后切下一个
        if (alertIdx >= nAlert) alertIdx = 0;
        if (now >= alertSwitchAt || alertIdx >= nAlert) {
            alertIdx = (alertIdx + 1) % nAlert;
            alertSwitchAt = now + 2500;    // 每个报警轮流显示 2.5 秒
        }
        ledColor = alerts[alertIdx].color;
        ledFlashMs = alerts[alertIdx].period;
        ledBlink = true;
    }

    // 按键 LED 反馈优先：任意键按下/长按，白灯闪 300ms（让用户明确看到灯有反应）
    bool anyKey = keyA.clickPending || keyB.clickPending || keyC.clickPending ||
                  keyA.holdPending || keyB.holdPending || keyC.holdPending;
    if (anyKey) keyLedUntil = now + 300;

    if (now < ledTestUntil) {
        // #LED 测试灯（最高优先，确认硬件）
        ledSetBrightness(220);
        ledSetAll(ledTestR, ledTestG, ledTestB);
    } else if (now < keyLedUntil) {
        ledSetBrightness(200);
        ledSetAll(255, 255, 255);
    } else if (ledBlink) {
        uint32_t half = ledFlashMs / 2;
        if (now - ledToggle >= ledFlashMs) { ledToggle = now; ledSetBrightness(180); ledSetAll((ledColor >> 16) & 0xFF, (ledColor >> 8) & 0xFF, ledColor & 0xFF); }
        else if (now - ledToggle >= half) ledSetAll(0, 0, 0);
    } else {
        ledSetAll(0, 0, 0);
    }

    // ===== 按键白名单（全部为显式用户操作；自研 GPIO 检测不受刷新阻塞影响） =====
    // 注意：updateCustomButtons 仅在 loop 顶部调用一次，
    // 不能在此重复调用，否则会清掉顶部扫描已产生的 click/hold 事件。
    // ===== 待机唤醒直达页：唤醒键决定跳转（A/B→首页, C→语音待办），先于录音保护执行 =====
    if (wakeJumpPage >= 0) {
        int pg = wakeJumpPage;
        wakeJumpPage = -1;
        if (pg != currentPage) {
            currentPage = pg;
            refreshPageData();
            renderScreen(false);
        }
    }
    // ===== 录音保护：录音期间只响应 C 键（停止录音），
    // 其他键全部忽略——避免误触 renderScreen 阻塞主循环导致丢采样！ =====
    if (isRecordingNow || voiceCmdMode) {
        // 待办录音：短按C停止；语音命令：3.5s自动停，忽略按键（防误触）
        if (isRecordingNow && keyC.clickPending) {
            keyCLast = now;
            recordTodoVoice();    // isRecordingNow==true → saveRecording()
        }
        return;   // 录音中直接跳过后续所有按键处理（不刷屏、不切页）
    }

    // ===== 播放保护：回放进行中任意键停止播放（不执行其他操作），让用户随时打断 =====
    // 注意：长按A本身是"启动回放"的键，必须先放行它，否则永远启动不了播放。
    // 因此这里只拦截"非长按A"的键（长按B/C、短按A/B/C），长按A走下面正常回放分支。
    if (voicePlaying) {
        bool stopByKey = keyC.clickPending || keyC.holdPending ||
                         keyB.clickPending || keyB.holdPending ||
                         keyA.clickPending;
        if (stopByKey) {
            stopVoicePlayback();
            snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[已停止播放]");
            memoryNoticeTime = now;
        }
        return;
    }

    // 长按 C: 待办页=切页；二维码页=关机菜单；其他页=全局语音命令（最小路径：不刷屏+蓝灯，识别后一次刷屏进页）
    if (keyC.holdPending) {
        if (now - keyCLast >= BUTTON_DEBOUNCE_MS) {
            keyCLast = now;
            if (currentPage == 4) {
                // 待办页长按C = 切页（短按C被录音占用，长按C用于切走）
                todoDetailMode = false;      // 切页退出详情模式
                currentPage = (currentPage + 1) % 7;
                refreshPageData();
                renderScreen(false);
            } else if (currentPage == 6) {
                // 二维码页长按C = 关机菜单（首次）或全彩强刷
                if (!powerMenuShown) { powerMenuShown = true; renderScreen(false); }
                else { refreshPageData(); renderScreen(true); }
            } else {
                // 其他页（0日历 1早报 2考点 3CodingPlan 5仪表盘）= 全局语音命令
                startVoiceCommand();
            }
        }
        return;
    }

    // 长按 A: 首页(0)=跳到最后一页(二维码)；考点页(2)=朗读；待办页(4)=回放语音
    if (keyA.holdPending) {
        Serial.printf("[KEY] 长按A触发 page=%d voicePlaying=%d\n", (int)currentPage, (int)voicePlaying);
        if (now - keyALast >= BUTTON_DEBOUNCE_MS) {
            keyALast = now;
            if (currentPage == 0) {
                currentPage = 6;             // 首页(日历)长按A → 最后一页(二维码)
                refreshPageData();
                renderScreen(false);
            } else if (currentPage == 2) {
                // 考点页长按A = 标记掌握（统一：长按A=状态/标记类操作）
                saveMemoryState(ebbinghausIndex, 1);
                snprintf(memoryNoticeBuf, sizeof(memoryNoticeBuf), "[已掌握 ✅]");
                memoryNoticeTime = now;
                ebbinghausIndex = (ebbinghausIndex + 1) % cardTotal();
                renderScreen(false);
            } else if (currentPage == 4) {
                playCurrentTodoVoice();    // 回放待办语音
            }
        }
        return;
    }

    // 长按 B: 考点页(2)=标记掌握；待办页(4)=删除当前待办
    if (keyB.holdPending) {
        if (now - keyBLast >= BUTTON_DEBOUNCE_MS) {
            keyBLast = now;
            if (currentPage == 0) {
                // 首页长按B = TTS 报今日天气+明日预报
                char wtxt[256];
                snprintf(wtxt, sizeof(wtxt), "%s天气，%s，气温 %d 到 %d 摄氏度。明天%s，%d 到 %d 度。",
                    weatherCity, weatherText, weatherHigh, weatherLow,
                    weatherTextTmr, weatherHighTmr, weatherLowTmr);
                ttsSpeak(wtxt);
            } else if (currentPage == 1) {
                // 早报页长按B = TTS 朗读当前早报（标题+摘要前200字，比80字更完整）
                char txt[512];
                size_t tl = snprintf(txt, sizeof(txt), "%s。", newsTitle(newsIndex));
                if (tl < sizeof(txt) - 1) {
                    size_t slen = strlen(newsSummary(newsIndex));
                    if (slen > 200) slen = 200;
                    snprintf(txt + tl, sizeof(txt) - tl, "%.*s", (int)slen, newsSummary(newsIndex));
                }
                ttsSpeak(txt);
            } else if (currentPage == 2) {
                // 考点页长按B = 朗读（统一：长按B=语音朗读）
                speakCurrentKnowledge();
            } else if (currentPage == 4) {
                deleteCurrentTodo();   // 待办页长按B=删除当前待办（语音待办同时删 WAV）
            }
        }
        return;
    }



    // 短按 C: 待办页(4)=录音开关；关机菜单=返回；其他页=循环切换（黑白快刷）
    if (keyC.clickPending) {
        if (now - keyCLast >= BUTTON_DEBOUNCE_MS) {
            keyCLast = now;
            if (currentPage == 6 && powerMenuShown) {
                powerMenuShown = false;      // 关机菜单：C 返回二维码页
                renderScreen(false);
            } else if (currentPage == 4) {
                if (todoDetailMode) {
                    // 详情模式：播放中短按C = 立即停止播放（留在详情看文字）；未播放 = 返回列表
                    if (voicePlaying) {
                        stopVoicePlayback();
                        renderScreen(false);
                    } else {
                        todoDetailMode = false;
                        renderScreen(false);
                    }
                } else {
                    recordTodoVoice();       // 开始/停止录音
                }
            } else {
                currentPage = (currentPage + 1) % 7;
                refreshPageData();
                renderScreen(false);
            }
        }
        return;
    }

    // 短按 A: 页面导航 + 内容翻页。
    // 用户要求：第2/4/6/7页(1/3/5/6)短按A翻到上一页；首页(0日历)短按A到第7页(6二维码)。
    // 第3页(2考点)/第5页(4待办)保留"上一条"内容翻页。
    if (keyA.clickPending) {
        if (now - keyALast >= BUTTON_DEBOUNCE_MS) {
            keyALast = now;
            if (currentPage == 6 && powerMenuShown) {
                esp_restart();               // 关机菜单：A = 软重启
            } else if (currentPage == 0) {
                if (calOffset > -1) { calOffset--; updateCalendarData(); }   // 首页(日历)短按A = 前一天（同步刷新农历/宜忌/天气）
                renderScreen(false);
            } else if (currentPage == 1) {
                newsIndex = (newsIndex + newsTotal() - 1) % newsTotal();   // 早报：A=上条（与B下条对称）
                renderScreen(false);
            } else if (currentPage == 2) {
                ebbinghausIndex = (ebbinghausIndex + cardTotal() - 1) % cardTotal();  // 考点上一条
                renderScreen(false);
            } else if (currentPage == 3) {
                currentPage = 2;             // 第4页(CodingPlan) → 上一页(考点)
                refreshPageData();
                renderScreen(false);
            } else if (currentPage == 4) {
                if (todoDetailMode) {
                    // 详情模式：翻上一页
                    if (asrPageIdx > 0) { asrPageIdx--; renderScreen(false); }
                } else {
                    asrPageIdx = 0;
                    todoIndex = (todoIndex + todoCount - 1) % (todoCount > 0 ? todoCount : 1);  // 待办上一条
                    renderScreen(false);
                }
            } else if (currentPage == 5) {
                currentPage = 4;             // 第6页(仪表盘) → 上一页(待办)
                refreshPageData();
                renderScreen(false);
            } else if (currentPage == 6) {
                powerMenuShown = false;      // 修复M8: 切页离开清关机菜单状态
                currentPage = 5;             // 第7页(二维码) → 上一页(仪表盘)
                refreshPageData();
                renderScreen(false);
            }
        }
        return;
    }

    // 短按 B: 内容翻下一条 / 仪表盘番茄开关
    if (keyB.clickPending) {
        if (now - keyBLast >= BUTTON_DEBOUNCE_MS) {
            keyBLast = now;
            if (currentPage == 0) {
                if (calOffset < 2) { calOffset++; updateCalendarData(); }    // 日历：B = 后一天（同步刷新农历/宜忌/天气）
                renderScreen(false);
            } else if (currentPage == 1) {
                newsIndex = (newsIndex + 1) % newsTotal();   // 快讯模式：B=下一条（newsTotal 兼容联网10条）
                renderScreen(false);   // 早报页 B 下一条必须刷新！
            } else if (currentPage == 2) {
                ebbinghausIndex = (ebbinghausIndex + 1) % cardTotal();
                renderScreen(false);   // 考点页 B 下一条必须刷新！
            } else if (currentPage == 4) {
                if (todoDetailMode) {
                    // 详情模式：翻下一页
                    if (asrPageIdx < asrPages - 1) { asrPageIdx++; renderScreen(false); }
                } else {
                    asrPageIdx = 0;
                    todoIndex = (todoIndex + 1) % (todoCount > 0 ? todoCount : 1);
                    renderScreen(false);
                }
            } else if (currentPage == 5) {
                // 仪表盘：B 开停番茄钟
                pomodoroRunning = !pomodoroRunning;
                pomodoroInRest = false;
                if (pomodoroRunning) {
                    pomodoroStartMillis = now;
                    pomodoroRemainMin = POMODORO_FOCUS_MIN;
                }
                renderScreen(false);
                // 番茄启动语音播报（界面先反馈，再语音）
                if (pomodoroRunning) ttsSpeak("番茄钟开始，专注二十五分钟。");
            } else if (currentPage == 6) {
                // 二维码：B = 下一页（循环回首页日历）
                powerMenuShown = false;      // 修复M8: 切页离开清关机菜单状态
                currentPage = 0;
                refreshPageData();
                renderScreen(false);
            }
            // 页面3(CodingPlan) 短按B无动作
        }
        return;
    }

    delay(20);
}
