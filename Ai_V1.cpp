#define ENABLE_GxEPD2_GFX 1
#include <Arduino.h>
#include <WiFi.h>
#include <Audio.h>
#include "time.h" 

#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "gb2312.c"
#include "GxEPD2_display_selection.h"
#include "GxEPD2_display_selection_added.h"
#include "GxEPD2_display_selection_new_style.h"
#include <Bounce2.h>

#include "driver/i2c.h"
#include "driver/i2s.h"
#include "es8311.h"
#include "esp_heap_caps.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD_MMC.h> 

// ================= SDMMC 引脚定义 =================
#define SDMMC_CLK_PIN  39
#define SDMMC_D0_PIN   40
#define SDMMC_CMD_PIN  41

uint8_t *custom_font_buffer = nullptr;

// ================= API 凭证 =================
String baiduApiUrl = "https://aip.baidubce.com/oauth/2.0/token?client_id=xxxxx&client_secret=xxxxx&grant_type=client_credentials";
String baiduAccessToken = ""; 
String qwenApiKey = "sk-xxxxxxx";  
String qwenApiUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"; 
String notionToken = "ntn_xxxxxxx"; 
String notionDatabaseId = "xxxxxxx";     
String notionApiUrl = "https://api.notion.com/v1/databases/" + notionDatabaseId + "/query";

String lastMessageId = "";
unsigned long lastNotionCheck = 0;
const unsigned long NOTION_CHECK_INTERVAL = 60000 *2;

#include <WiFiClientSecure.h>

const char* ssid = "DESKTOP";
const char* password = "88888888";
const char* ntpServer = "ntp.aliyun.com"; 
const long  gmtOffset_sec = 8 * 3600;     
const int   daylightOffset_sec = 0;

unsigned long pomoBtnDownTime = 0;
bool isWaitingForPomoLongPress = false;

Audio audio;  // 全局 Audio 对象

// ================= 引脚 =================
#define GPIO6_PIN      6
#define BUTTON_PIN     18
#define POMO_BUTTON_PIN 0
#define baise          GxEPD_WHITE  
#define heise          GxEPD_BLACK  

#define I2C_SDA        47
#define I2C_SCL        48
#define I2S_MCLK       14
#define I2S_BCLK       15
#define I2S_LRCK       38
#define I2S_DOUT       45
#define I2S_DIN        16
#define PIN_PA_EN      42
#define PIN_PA_CTRL    46

#define I2C_PORT_NUM   I2C_NUM_0
#define I2S_PORT_NUM   I2S_NUM_0
#define SAMPLE_RATE    16000

Bounce button = Bounce();
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;
es8311_handle_t es8311_dev;
Bounce pomoButton = Bounce();

bool isPomodoroMode = false;
int remainingMinutes = 25;
int currentSound = 0;
unsigned long lastPomoTick = 0;

const char* soundFiles[] = {"", "/rain.mp3", "/white.mp3"};

uint8_t *psram_audio_buffer = nullptr;
const size_t MAX_AUDIO_SIZE = 2 * 1024 * 1024;
size_t recorded_size = 0;
bool isRecording = false;
bool longPressHandled = false;
bool pomoLongPressHandled = false;
const unsigned long LONG_PRESS_MS = 600; 

#define MAX_TODOS 20
#define ITEMS_PER_PAGE 5
int currentPage = 0;
struct TodoItem {
    String task;
    String deadline;
    bool isDone;
    bool isAlarmed;
};

TodoItem todoList[MAX_TODOS];
int todoCount = 0;
String currentAiMessage = "你好！按住按钮告诉我你的安排~"; 
unsigned long lastTimeCheck = 0;

// 函数声明
void connectWiFi();
void syncTime();
String getCurrentDateString();
String getCurrentShortTime();
void init_i2c();
void init_es8311();
void init_i2s();
void getBaiduToken();
String audioToText(uint8_t* audioData, size_t size);
String askAI(String userInput);
void parseAndRefreshUI(String jsonStr);
void drawFullUI();
void updateAIText_Partial(const char* newText);  // 改为 const char*
void updateTodoList_Partial();
void checkAlarms();
void playTTS(String text);
String urlEncode(String str);
void checkNotionMessages();
void setup_SD_Font();
void updatePomodoroUI(bool isFullRefresh);
void togglePomodoro();

// ================= 辅助函数：安全停止音频 =================
void safeStopAudio() {
    audio.stopSong();
    // 等待音频彻底停止（最多等待 500ms）
    unsigned long start = millis();
    while (audio.isRunning() && millis() - start < 500) {
        delay(1);
    }
    delay(20); // 额外稳定时间
}

// ================= 初始化 =================
void setup() {
    Serial.begin(115200);
    
    pinMode(GPIO6_PIN, OUTPUT); 
    digitalWrite(GPIO6_PIN, LOW); 

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    button.attach(BUTTON_PIN);
    button.interval(20);

    psram_audio_buffer = (uint8_t *)heap_caps_malloc(MAX_AUDIO_SIZE, MALLOC_CAP_SPIRAM);
    if (!psram_audio_buffer) {
        Serial.println("PSRAM 分配失败，重启");
        ESP.restart();
    }

    connectWiFi();
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    syncTime();

    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, LOW);
    pinMode(PIN_PA_CTRL, OUTPUT);
    digitalWrite(PIN_PA_CTRL, HIGH); 
    init_i2c();
    init_es8311();
    init_i2s();               // 一次性配置双声道全双工

    audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
    audio.setVolume(21); 

    display.init(115200, true, 2, false); 
    display.setRotation(2); 
    u8g2Fonts.begin(display);            
    u8g2Fonts.setFontMode(1);            
    u8g2Fonts.setFontDirection(0);         
    u8g2Fonts.setForegroundColor(heise);  
    u8g2Fonts.setBackgroundColor(baise);  
    setup_SD_Font();

    drawFullUI();
    Serial.println("System Ready.");
}

void loop() {
    pomoButton.update();
    button.update();
    audio.loop(); 

    // 番茄钟按键逻辑
    if (pomoButton.fell()) {
        pomoLongPressHandled = false; 
    }
    if (pomoButton.read() == LOW) {
        if (pomoButton.currentDuration() >= 1000 && !pomoLongPressHandled) {
            pomoLongPressHandled = true;
            togglePomodoro();
        }
    }
    if (pomoButton.rose()) {
        if (!pomoLongPressHandled && isPomodoroMode) {
            currentSound = (currentSound + 1) % 3; 
            safeStopAudio();  // 安全停止
            if (currentSound > 0) {
                audio.connecttoFS(SD_MMC, soundFiles[currentSound]);
                audio.setFileLoop(true);
            }
        }
    }

    // 番茄钟倒计时
    if (isPomodoroMode) {
        if (millis() - lastPomoTick >= 60000) {
            lastPomoTick = millis();
            remainingMinutes--;
            if (remainingMinutes > 0) {
                updatePomodoroUI(false); 
            } else {
                isPomodoroMode = false;
                safeStopAudio();
                playTTS("专注时间结束，休息一下吧！");
                drawFullUI();
            }
        }
        return; 
    }

    // 非番茄钟模式：闹钟、Notion、录音
    if (millis() - lastTimeCheck > 5000) {
        checkAlarms();
        lastTimeCheck = millis();
    }
    if (millis() - lastNotionCheck > NOTION_CHECK_INTERVAL) {
        checkNotionMessages();
        lastNotionCheck = millis();
    }

    if (button.fell()) { 
        longPressHandled = false; 
        safeStopAudio();  // 停止任何正在播放的音频
    }

    if (button.read() == LOW) {
        if (button.currentDuration() >= LONG_PRESS_MS && !longPressHandled) {
            longPressHandled = true; 
            isRecording = true;
            recorded_size = 0;
            updateAIText_Partial("正在倾听你的安排..."); 
        }

        if (isRecording) {
            uint8_t tmp_buf[2048];
            size_t bytes_transferred = 0;
            if (recorded_size + 1024 <= MAX_AUDIO_SIZE) {
                i2s_read(I2S_PORT_NUM, tmp_buf, 2048, &bytes_transferred, portMAX_DELAY);
                // 双声道转单声道（取左声道）
                uint16_t sample_count = bytes_transferred / 4;
                uint8_t* dst = psram_audio_buffer + recorded_size;
                for (uint16_t i = 0; i < sample_count; i++) {
                    dst[i*2]   = tmp_buf[i*4];
                    dst[i*2+1] = tmp_buf[i*4+1];
                }
                recorded_size += sample_count * 2;
            }
        }
    }

    if (button.rose()) {
        if (longPressHandled) {
            isRecording = false;
            updateAIText_Partial("正在思考并整理..."); 
            String userText = audioToText(psram_audio_buffer, recorded_size);
            if (userText != "") {
                String aiJsonReply = askAI(userText);
                parseAndRefreshUI(aiJsonReply);
            } else {
                updateAIText_Partial("没听清，请重试~");
            }
        } else {
            if (todoCount > ITEMS_PER_PAGE) {
                int totalPages = (todoCount + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
                currentPage = (currentPage + 1) % totalPages;
                updateTodoList_Partial();
            }
        }
    }
}

// ================= AI 解析 =================
String askAI(String userInput) {
    HTTPClient http;
    http.begin(qwenApiUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + qwenApiKey);
    
    String systemPrompt = "你是一个贴心的桌面待办助手。用户会告诉你安排，请提取新增的待办事项。今天日期：" + getCurrentDateString() + "。严格返回JSON，不要Markdown。格式：{\"ai_reply\":\"已为您添加。\",\"todos\":[{\"task\":\"拿快递\",\"deadline\":\"04-03 18:30\"}]}";

    DynamicJsonDocument doc(2048);
    doc["model"] = "qwen-turbo"; 
    JsonArray messages = doc.createNestedArray("messages");
    JsonObject msg1 = messages.createNestedObject();
    msg1["role"] = "system"; msg1["content"] = systemPrompt;
    JsonObject msg2 = messages.createNestedObject();
    msg2["role"] = "user"; msg2["content"] = userInput;

    String requestBody;
    serializeJson(doc, requestBody);
    
    int httpCode = http.POST(requestBody);
    String aiReply = "";

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument resp(2048);
        deserializeJson(resp, payload);
        aiReply = resp["choices"][0]["message"]["content"].as<String>();
        aiReply.replace("```json\n", "");
        aiReply.replace("```", "");
    }
    http.end();
    return aiReply;
}

void parseAndRefreshUI(String jsonStr) {
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, jsonStr);
    if (error) {
        updateAIText_Partial("抱歉，理解安排时出错了。");
        return;
    }
   
    currentAiMessage = doc["ai_reply"].as<String>();
    updateAIText_Partial(currentAiMessage.c_str());
    
    JsonArray todos = doc["todos"].as<JsonArray>();
    for (JsonObject t : todos) {
        if (todoCount >= MAX_TODOS) {
            for (int j = 0; j < MAX_TODOS - 1; j++) {
                todoList[j] = todoList[j + 1];
            }
            todoCount = MAX_TODOS - 1;
        }
        todoList[todoCount].task = t["task"].as<String>();
        todoList[todoCount].deadline = t["deadline"].as<String>();
        todoList[todoCount].isDone = false; 
        todoList[todoCount].isAlarmed = false; 
        todoCount++;
    }
    updateTodoList_Partial();
}

// ================= UI 绘制 =================
void drawFullUI() {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(baise);
        u8g2Fonts.setCursor(5, 25);
        u8g2Fonts.print(getCurrentDateString()); 
        u8g2Fonts.setCursor(5, 50);
        u8g2Fonts.print("AI: " + currentAiMessage); 
        display.drawFastHLine(0, 60, display.width(), heise);
        u8g2Fonts.setCursor(5, 85);
        u8g2Fonts.print("等待安排待办...");
    } while (display.nextPage());
}

// 修改为 const char* 避免 String 临时分配
void updateAIText_Partial(const char* newText) {
    display.setPartialWindow(0, 30, display.width(), 25); 
    
    // 强力清洗
    display.firstPage();
    do { display.fillScreen(heise); } while (display.nextPage());
    display.firstPage();
    do { display.fillScreen(baise); } while (display.nextPage());
    
    display.firstPage();
    do {
        display.fillScreen(baise); 
        u8g2Fonts.setCursor(5, 50); 
        u8g2Fonts.print("AI: ");
        u8g2Fonts.print(newText);
    } while (display.nextPage());
}

void updateTodoList_Partial() {
    uint16_t y_start = 62;
    uint16_t h = display.height() - y_start;
    display.setPartialWindow(0, y_start, display.width(), h);
    
    display.firstPage();
    do { display.fillScreen(baise); } while (display.nextPage());

    int startIdx = currentPage * ITEMS_PER_PAGE;
    int endIdx = min(startIdx + ITEMS_PER_PAGE, todoCount);
    int totalPages = (todoCount + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;

    display.firstPage();
    do {
        display.fillScreen(baise);
        if (todoCount == 0) {
            u8g2Fonts.setCursor(5, 85);
            u8g2Fonts.print("等待安排待办...");
        } else {
            int drawY = 85;
            for (int i = startIdx; i < endIdx; i++) {
                String line = String(i + 1) + ". " + todoList[i].task + " (" + todoList[i].deadline + ")";
                u8g2Fonts.setCursor(5, drawY); 
                u8g2Fonts.print(line);
                if (todoList[i].isDone) {
                    int textWidth = u8g2Fonts.getUTF8Width(line.c_str());
                    display.drawFastHLine(5, drawY - 6, textWidth, heise);
                }
                display.drawFastHLine(5, drawY + 8, display.width() - 10, heise);
                drawY += 28;
            }
            if (totalPages > 1) {
                String pageInfo = String(currentPage + 1) + "/" + String(totalPages);
                int pWidth = u8g2Fonts.getUTF8Width(pageInfo.c_str());
                u8g2Fonts.setCursor(display.width() - pWidth - 5, display.height() - 5);
                u8g2Fonts.print(pageInfo);
            }
        }
    } while (display.nextPage());
}

// ================= 闹钟 =================
void checkAlarms() {
    if (todoCount == 0) return;
    String nowTime = getCurrentShortTime();
    bool needRefreshScreen = false;

    for (int i = 0; i < todoCount; i++) {
        if (nowTime == todoList[i].deadline && !todoList[i].isAlarmed) {
            todoList[i].isAlarmed = true; 
            todoList[i].isDone = true;
            needRefreshScreen = true;
            String alertText = "提醒您，关于 " + todoList[i].task + " 的时间到了！";
            playTTS(alertText);
        }
    }
    if (needRefreshScreen) {
        updateTodoList_Partial();
    }
}

// ================= Notion =================
void handleNewMessage(String sender, String message) {
    String ttsText = sender + " 给你发来一条专属留言：" + message;
    playTTS(ttsText);
    String displayText = "[" + sender + "留言] " + message;
    updateAIText_Partial(displayText.c_str());
    currentAiMessage = displayText; 
}

void checkNotionMessages() {
    Serial.println("\n>>> 检查 Notion...");
    WiFiClientSecure *client = new WiFiClientSecure;
    if (client) {
        client->setInsecure();
        HTTPClient http;
        http.setTimeout(15000);
        if (http.begin(*client, notionApiUrl)) {
            http.addHeader("Authorization", "Bearer " + notionToken);
            http.addHeader("Notion-Version", "2022-06-28");
            String payload = "{\"sorts\":[{\"timestamp\":\"created_time\",\"direction\":\"descending\"}],\"page_size\":1}";
            int httpCode = http.POST(payload);
            if (httpCode == HTTP_CODE_OK) {
                String response = http.getString();
                DynamicJsonDocument doc(4096);
                DeserializationError error = deserializeJson(doc, response);
                if (!error) {
                    JsonArray results = doc["results"];
                    if (results.size() > 0) {
                        String pageId = results[0]["id"].as<String>();
                        if (lastMessageId == "") {
                            lastMessageId = pageId;
                        } else if (pageId != lastMessageId) {
                            lastMessageId = pageId;
                            String msgText = "空内容";
                            if (!results[0]["properties"]["Message"]["title"].isNull() && results[0]["properties"]["Message"]["title"].size() > 0) {
                                msgText = results[0]["properties"]["Message"]["title"][0]["plain_text"].as<String>();
                            }
                            String senderText = "神秘人";
                            if (!results[0]["properties"]["Sender"]["rich_text"].isNull() && results[0]["properties"]["Sender"]["rich_text"].size() > 0) {
                                senderText = results[0]["properties"]["Sender"]["rich_text"][0]["plain_text"].as<String>();
                            }
                            handleNewMessage(senderText, msgText);
                        }
                    }
                }
            }
            http.end();
        }
        delete client;
    }
}

void syncTime() {
    struct tm timeinfo;
    Serial.print("Syncing Time...");
    while (!getLocalTime(&timeinfo)) { delay(1000); Serial.print("."); }
    Serial.println(" OK!");
}

String getCurrentDateString() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return "时间未同步";
    char dateStr[32];
    const char* weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    sprintf(dateStr, "%02d月%02d日 星期%s", timeinfo.tm_mon + 1, timeinfo.tm_mday, weekdays[timeinfo.tm_wday]);
    return String(dateStr);
}

String getCurrentShortTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return "";
    char timeStr[16];
    sprintf(timeStr, "%02d-%02d %02d:%02d", timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min);
    return String(timeStr);
}

void connectWiFi() {
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected!");
}

void init_i2c() {
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER, .sda_io_num = I2C_SDA, .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = { .clk_speed = 100000 }
    };
    i2c_param_config(I2C_PORT_NUM, &i2c_conf);
    i2c_driver_install(I2C_PORT_NUM, i2c_conf.mode, 0, 0, 0);
}

void init_es8311() {
    es8311_dev = es8311_create(I2C_PORT_NUM, ES8311_ADDRESS_0); 
    es8311_clock_config_t clk_cfg = {
        .mclk_inverted = false, .sclk_inverted = false, .mclk_from_mclk_pin = true,
        .mclk_frequency = SAMPLE_RATE * 256, .sample_frequency = SAMPLE_RATE
    };
    es8311_init(es8311_dev, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    es8311_microphone_config(es8311_dev, false); 
    es8311_microphone_gain_set(es8311_dev, ES8311_MIC_GAIN_30DB); 
    es8311_voice_volume_set(es8311_dev, 75, NULL); 
}

void init_i2s() {
    i2s_driver_uninstall(I2S_PORT_NUM);
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = true,
        .tx_desc_auto_clear = true
    };
    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_MCLK,
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRCK,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_DIN
    };
    i2s_driver_install(I2S_PORT_NUM, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT_NUM, &pin_config);
}

void getBaiduToken() {
    if (baiduAccessToken != "") return;
    HTTPClient http;
    http.begin(baiduApiUrl);
    if (http.GET() == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument doc(2048);
        deserializeJson(doc, payload);
        baiduAccessToken = doc["access_token"].as<String>();
    }
    http.end();
}

String audioToText(uint8_t* audioData, size_t size) {
    if (baiduAccessToken == "") getBaiduToken();
    HTTPClient http;
    String sttUrl = "http://vop.baidu.com/server_api?cuid=esp32_device&token=" + baiduAccessToken + "&dev_pid=1537";
    http.begin(sttUrl);
    http.addHeader("Content-Type", "audio/pcm;rate=16000"); 
    int httpCode = http.POST(audioData, size);
    String resultText = "";
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, payload);
        if (doc["err_no"] == 0) resultText = doc["result"][0].as<String>();
    }
    http.end();
    return resultText;
}

String urlEncode(String str) {
    String encodedString = "";
    for (int i = 0; i < str.length(); i++){
        char c = str.charAt(i);
        if (c == ' ') encodedString += '+'; 
        else if (isalnum(c)) encodedString += c; 
        else {
            char code1 = (c & 0xf) + '0'; if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
            c = (c >> 4) & 0xf;
            char code0 = c + '0'; if (c > 9) code0 = c - 10 + 'A';
            encodedString += '%'; encodedString += code0; encodedString += code1;
        }
    }
    return encodedString;
}

void playTTS(String text) {
    if (baiduAccessToken == "") getBaiduToken();
    String per = "0";
    String ttsUrl = "http://tsn.baidu.com/text2audio?tex=" + urlEncode(text) + "&lan=zh&cuid=esp32_device&ctp=1&tok=" + baiduAccessToken + "&per=" + per;
    
    safeStopAudio();  // 停止当前播放
    audio.connecttohost(ttsUrl.c_str());
}

void setup_SD_Font() {
    Serial.println("\n>>> 初始化 SDMMC...");
    SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN);
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println(">>> SD 卡挂载失败！");
        return;
    }
    File fontFile = SD_MMC.open("/XuanZongTi_Regular_16.bin", FILE_READ);
    if (!fontFile) {
        Serial.println(">>> 找不到字库文件");
        return;
    }
    size_t fontSize = fontFile.size();
    custom_font_buffer = (uint8_t *)heap_caps_malloc(fontSize, MALLOC_CAP_SPIRAM);
    if (custom_font_buffer == nullptr) {
        Serial.println(">>> PSRAM 不足，无法加载字库");
        fontFile.close();
        return;
    }
    fontFile.read(custom_font_buffer, fontSize);
    fontFile.close();
    u8g2Fonts.setFont((const uint8_t*)custom_font_buffer); 
}

void updatePomodoroUI(bool isFullRefresh) {
    if (isFullRefresh) {
        display.setFullWindow(); 
        display.firstPage();
        do {
            display.fillScreen(baise);
            u8g2Fonts.setFont((const uint8_t*)custom_font_buffer); 
            u8g2Fonts.setCursor(60, 40);
            u8g2Fonts.print("番茄专注中");
            u8g2Fonts.setFont(u8g2_font_logisoso62_tn); 
            String timeStr = remainingMinutes < 10 ? "0" + String(remainingMinutes) : String(remainingMinutes);
            u8g2Fonts.setCursor(45, 130); 
            u8g2Fonts.print(timeStr);
            u8g2Fonts.setFont((const uint8_t*)custom_font_buffer); 
            u8g2Fonts.setCursor(130, 130); 
            u8g2Fonts.print("min");
            u8g2Fonts.setCursor(10, 180);
            u8g2Fonts.print("环境音: 无声 (短按切换)");
            display.drawRect(5, 5, 190, 190, heise);
        } while (display.nextPage());
        u8g2Fonts.setFont((const uint8_t*)custom_font_buffer);
    } else {
        uint16_t box_x = 40, box_y = 65, box_w = 140, box_h = 70;
        display.setPartialWindow(box_x, box_y, box_w, box_h);
        display.firstPage();
        do { display.fillScreen(heise); } while (display.nextPage());
        display.firstPage();
        do { display.fillScreen(baise); } while (display.nextPage());
        display.firstPage();
        do {
            display.fillScreen(baise);
            u8g2Fonts.setFont(u8g2_font_logisoso62_tn);
            String timeStr = remainingMinutes < 10 ? "0" + String(remainingMinutes) : String(remainingMinutes);
            u8g2Fonts.setCursor(45, 130); 
            u8g2Fonts.print(timeStr);
            u8g2Fonts.setFont((const uint8_t*)custom_font_buffer); 
            u8g2Fonts.setCursor(130, 130); 
            u8g2Fonts.print("min");
        } while (display.nextPage());
        u8g2Fonts.setFont((const uint8_t*)custom_font_buffer);
    }
}

void togglePomodoro() {
    isPomodoroMode = !isPomodoroMode;
    if (isPomodoroMode) {
        remainingMinutes = 25;
        lastPomoTick = millis();
        currentSound = 0; 
        updatePomodoroUI(true); 
    } else {
        safeStopAudio();  // 退出前完全停止音频
        drawFullUI();
    }
}
