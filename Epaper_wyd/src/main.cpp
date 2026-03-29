#define ENABLE_GxEPD2_GFX 1
#include <Arduino.h>
#include <WiFi.h>          // 新增：用于连接网络
#include <Audio.h>         // 新增：ESP32-audioI2S 库，用于解析在线 MP3

#include "BitmapDisplay.h"
#include <GxEPD2_BW.h>
#include "image.h" 
#include <U8g2_for_Adafruit_GFX.h>
#include "gb2312.c"
#include "GxEPD2_display_selection.h"
#include "GxEPD2_display_selection_added.h"
#include "GxEPD2_display_selection_new_style.h"
#include "bitmaps/Bitmaps200x200.h"
#include "TextDisplay.h"
#include <Bounce2.h>

// --- 音频相关头文件 ---
#include "driver/i2c.h"
#include "driver/i2s.h"
#include "es8311.h"
#include "esp_heap_caps.h"

// ================= 网络与在线音频配置 =================
// --- 配置你的 WiFi ---
const char* ssid = "CMCC-7Sy6";
const char* password = "59hwhnbx";
const char* online_mp3_url = "http://music.163.com/song/media/outer/url?id=3347088458.mp3"; // TODO: 替换为真实的 MP3 直链

Audio audio; // 实例化 Audio 对象

// ================= 引脚与系统定义 =================
// 墨水屏与按键
#define GPIO6_PIN      6
#define BUTTON_PIN     18
#define baise          GxEPD_WHITE  
#define heise          GxEPD_BLACK  

// 音频 I2C & I2S
#define I2C_SDA        47
#define I2C_SCL        48
#define I2S_MCLK       14
#define I2S_BCLK       15
#define I2S_LRCK       38
#define I2S_DOUT       45   // 扬声器输出
#define I2S_DIN        16   // 麦克风输入
#define PIN_PA_EN      42
#define PIN_PA_CTRL    46

#define I2C_PORT_NUM   I2C_NUM_0
#define I2S_PORT_NUM   I2S_NUM_0
#define SAMPLE_RATE    16000

// ================= 全局变量 =================
Bounce button = Bounce();

// 墨水屏相关
int currentImageIndex = 0; // 0:苏念雪, 1:柳如意, 2:在线电台
BitmapDisplay bitmaps(display);
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

// 按键逻辑相关
const unsigned long LONG_PRESS_MS = 600; 
bool longPressHandled = false;           

// ================= 音频状态与缓冲 =================
enum AudioState {
    STATE_IDLE,
    STATE_RECORDING,
    STATE_PLAYING_PCM,   // 播放 PSRAM 录音或内置裸流
    STATE_PLAYING_ONLINE // 新增：播放在线 MP3
};
AudioState audio_state = STATE_IDLE;

es8311_handle_t es8311_dev;
uint8_t *psram_audio_buffer = nullptr;
const size_t MAX_AUDIO_SIZE = 2 * 1024 * 1024; // 2MB
size_t recorded_size = 0;

// 用于播放 PCM 的指针
const uint8_t *current_play_buffer = nullptr;  
size_t current_play_total_size = 0;            
size_t play_offset = 0;                        

// ================= 函数声明 =================
void showImageAndName(int imageIndex);
void init_i2c();
void init_es8311();
void init_i2s();
void connectWiFi();
void setup_i2s_for_mode(bool is_online_mp3);

// ================= 初始化 =================
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Ink Screen & Audio System Start ---");

    // 1. 初始化 GPIO6 并拉低
    pinMode(GPIO6_PIN, OUTPUT); 
    digitalWrite(GPIO6_PIN, LOW); 

    // 2. 按键初始化
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    button.attach(BUTTON_PIN);
    button.interval(20);

    // 3. 分配 PSRAM 内存用于录音
    Serial.println("Allocating PSRAM for Audio...");
    psram_audio_buffer = (uint8_t *)heap_caps_malloc(MAX_AUDIO_SIZE, MALLOC_CAP_SPIRAM);
    if (psram_audio_buffer == nullptr) {
        Serial.println("Failed to allocate PSRAM! Halt.");
        while (true) delay(1000);
    }

    // 4. 连接 WiFi (为在线音频做准备)
    connectWiFi();

    // 5. 音频硬件初始化 (你的原生底层驱动)
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, LOW);
    pinMode(PIN_PA_CTRL, OUTPUT);
    digitalWrite(PIN_PA_CTRL, HIGH); 
    init_i2c();
    init_es8311();
    init_i2s();

    // 6. Audio 库初始化 (复用 I2S 引脚输出解码后的音频)
    audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
    audio.setVolume(15); 

    // 7. 墨水屏初始化
    display.init(115200, true, 2, false); 
    display.setRotation(2); 
    u8g2Fonts.begin(display);              
    u8g2Fonts.setFontMode(1);              
    u8g2Fonts.setFontDirection(0);         
    u8g2Fonts.setForegroundColor(heise);  
    u8g2Fonts.setBackgroundColor(baise);  
    u8g2Fonts.setFont(chinese_gb2312);    

    // 显示初始画面
    showImageAndName(currentImageIndex);
    Serial.println("System Ready. Short press to switch, Long press to record.");
}

// ================= 主循环 =================
void loop() {
    button.update();

    const size_t chunk_size = 1024; 
    size_t bytes_transferred = 0;

    // --- 1. 按键刚按下瞬间 ---
    if (button.fell()) { 
        longPressHandled = false; 
        if (audio_state == STATE_PLAYING_PCM || audio_state == STATE_PLAYING_ONLINE) {
            // 打断当前的所有播放
            audio_state = STATE_IDLE;
            audio.stopSong(); // 停止 Audio 库的在线播放
            Serial.println("Playback interrupted by button press.");
        }
    }

    // --- 2. 按钮持续按住的状态 (触发录音) ---
    if (button.read() == LOW) {
        if (button.currentDuration() >= LONG_PRESS_MS && !longPressHandled) {
            longPressHandled = true; 
            Serial.println(">>> LONG PRESS detected! Start Recording...");
            audio.stopSong(); // 确保在线音乐停止，释放 I2S 资源
            setup_i2s_for_mode(false); // <--- 改为这句，恢复单声道与RX模式
            audio_state = STATE_RECORDING;
            recorded_size = 0;
        }

        if (audio_state == STATE_RECORDING) {
            if (recorded_size + chunk_size <= MAX_AUDIO_SIZE) {
                i2s_read(I2S_PORT_NUM, psram_audio_buffer + recorded_size, chunk_size, &bytes_transferred, portMAX_DELAY);
                recorded_size += bytes_transferred;
            }
        }
    }

    // --- 3. 按钮松开瞬间 ---
    if (button.rose()) {
        if (longPressHandled) {
            // === 情况 A：长按结束，播放录音 (PCM) ===
            Serial.printf(">>> Recording Finished. Size: %d bytes. Start Playing...\n", recorded_size);
            current_play_buffer = psram_audio_buffer;
            current_play_total_size = recorded_size;
            play_offset = 0;
            audio_state = STATE_PLAYING_PCM;
            
        } else {
            // === 情况 B：短按，切换墨水屏并播放对应音频 ===
            Serial.println(">>> SHORT PRESS detected! Switching Mode...");
            currentImageIndex = (currentImageIndex + 1) % 3; // 扩展为 3 个界面
            
            showImageAndName(currentImageIndex); // 刷新屏幕
            
            if (currentImageIndex == 0 || currentImageIndex == 1) {
                audio.stopSong(); // 新增：确保在线播放已停止
                setup_i2s_for_mode(false);
                // 播放内置音频 (PCM)
                if (currentImageIndex == 0) {
                    current_play_buffer = audio_sunianxue_pcm;
                    current_play_total_size = audio_sunianxue_len;
                } else {
                    current_play_buffer = audio_liuruyi_pcm;
                    current_play_total_size = audio_liuruyi_len;
                }
                play_offset = 0;
                audio_state = STATE_PLAYING_PCM;
                Serial.println(">>> Start playing built-in PCM audio...");

            } else if (currentImageIndex == 2) {
                // 播放在线 MP3 (Audio 库)
                audio_state = STATE_PLAYING_ONLINE;
                Serial.println(">>> Start streaming online MP3...");
                setup_i2s_for_mode(true); // <--- 新增这句！告诉 I2S 准备接收双声道 MP3 数据！
                audio.connecttohost(online_mp3_url);
            }
        }
    }

    // --- 4. 维持 PCM 播放 (录音或内置裸流) ---
    if (audio_state == STATE_PLAYING_PCM && button.read() == HIGH) {
        if (play_offset < current_play_total_size) {
            size_t bytes_to_write = min(chunk_size, current_play_total_size - play_offset);
            i2s_write(I2S_PORT_NUM, current_play_buffer + play_offset, bytes_to_write, &bytes_transferred, portMAX_DELAY);
            play_offset += bytes_transferred;
        } else {
            Serial.println(">>> PCM Playback Complete.");
            audio_state = STATE_IDLE;
        }
    }

    // --- 5. 维持在线 MP3 播放 (Audio 库) ---
    if (audio_state == STATE_PLAYING_ONLINE) {
        audio.loop(); 
        if (!audio.isRunning()) {
            // 如果音频流自然结束或断开连接
            // audio_state = STATE_IDLE; 
        }
    }
}

// ================= 辅助模块实现 =================

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

void showImageAndName(int imageIndex) {
    Serial.println("Updating e-ink display...");
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_BLACK);
        
        // --- 绘制图片 ---
        if (imageIndex == 0) {
            display.drawInvertedBitmap(0, 0, gImage_sunianxue, 200, 200, GxEPD_WHITE);
        } else if (imageIndex == 1) {
            display.drawInvertedBitmap(0, 0, gImage_liuruyi, 200, 200, GxEPD_WHITE);
        } else if (imageIndex == 2) {
            // 界面 3: 在线音乐专属界面 (此处可替换为你自己的 Bitmap)
            // 这里用纯黑底色加上特定的文字排版作为占位示例
            display.fillScreen(GxEPD_BLACK); 
        }

        // --- 绘制文字 ---
        int textX = 10;        
        int startY = 30;       
        int lineSpacing = 30;  
        
        if (imageIndex == 0) {
            u8g2Fonts.setCursor(textX, startY); u8g2Fonts.print("苏");
            u8g2Fonts.setCursor(textX, startY + lineSpacing); u8g2Fonts.print("念");
            u8g2Fonts.setCursor(textX, startY + lineSpacing * 2); u8g2Fonts.print("雪");
        } else if (imageIndex == 1) {
            u8g2Fonts.setCursor(textX, startY); u8g2Fonts.print("柳");
            u8g2Fonts.setCursor(textX, startY + lineSpacing); u8g2Fonts.print("如");
            u8g2Fonts.setCursor(textX, startY + lineSpacing * 2); u8g2Fonts.print("意");
        } else if (imageIndex == 2) {
            u8g2Fonts.setCursor(textX, startY); u8g2Fonts.print("在");
            u8g2Fonts.setCursor(textX, startY + lineSpacing); u8g2Fonts.print("线");
            u8g2Fonts.setCursor(textX, startY + lineSpacing * 2); u8g2Fonts.print("电");
            u8g2Fonts.setCursor(textX, startY + lineSpacing * 3); u8g2Fonts.print("台");
        }
        
    } while (display.nextPage()); 
    Serial.println("E-ink update complete.");
}

// ---------------- 以下为保留的底层硬件驱动 ----------------

void init_i2c() {
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = { .clk_speed = 100000 }
    };
    i2c_param_config(I2C_PORT_NUM, &i2c_conf);
    i2c_driver_install(I2C_PORT_NUM, i2c_conf.mode, 0, 0, 0);
}

void init_es8311() {
    es8311_dev = es8311_create(I2C_PORT_NUM, ES8311_ADDRESS_0); 
    es8311_clock_config_t clk_cfg = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = SAMPLE_RATE * 256, 
        .sample_frequency = SAMPLE_RATE
    };
    es8311_init(es8311_dev, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    es8311_microphone_config(es8311_dev, false); 
    es8311_microphone_gain_set(es8311_dev, ES8311_MIC_GAIN_30DB); 
    es8311_voice_volume_set(es8311_dev, 75, NULL); 
}

void init_i2s() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, 
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
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

// 动态配置 I2S 驱动：is_online_mp3 为 true 时用立体声，false 时用单声道
void setup_i2s_for_mode(bool is_online_mp3) {
    i2s_driver_uninstall(I2S_PORT_NUM); // 先卸载现有驱动

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = 16000, // 初始给 16kHz 即可，Audio库播放在线音乐时会自动拉高真实采样率
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        // 核心修复点：在线 MP3 必须用双声道(RIGHT_LEFT)，本地 PCM 和录音用单声道(ONLY_LEFT)
        .channel_format = is_online_mp3 ? I2S_CHANNEL_FMT_RIGHT_LEFT : I2S_CHANNEL_FMT_ONLY_LEFT, 
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
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