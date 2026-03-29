#define ENABLE_GxEPD2_GFX 1
#include <Arduino.h>
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
int currentImageIndex = 0; 
BitmapDisplay bitmaps(display);
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

// 按键逻辑相关
const unsigned long LONG_PRESS_MS = 600; // 长按判定时间：600毫秒 (稍微缩短点，提升长按录音的响应感)
bool longPressHandled = false;           // 标记是否触发了长按

// ================= 音频状态与缓冲 =================
enum AudioState {
    STATE_IDLE,
    STATE_RECORDING,
    STATE_PLAYING
};
AudioState audio_state = STATE_IDLE;
es8311_handle_t es8311_dev;
uint8_t *psram_audio_buffer = nullptr;
const size_t MAX_AUDIO_SIZE = 2 * 1024 * 1024; // 2MB PSRAM 音频缓存
size_t recorded_size = 0;

// === 新增：用于兼容播放“录音”和“内置音频”的通用播放指针 ===
const uint8_t *current_play_buffer = nullptr;  // 当前播放的数据源指针
size_t current_play_total_size = 0;            // 当前播放的总长度
size_t play_offset = 0;                        // 当前播放的偏移量

// ================= 函数声明 =================
void showImageAndName(int imageIndex);
void init_i2c();
void init_es8311();
void init_i2s();

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
    Serial.printf("PSRAM Allocated: %d bytes\n", MAX_AUDIO_SIZE);

    // 4. 音频硬件初始化
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, LOW);
    pinMode(PIN_PA_CTRL, OUTPUT);
    digitalWrite(PIN_PA_CTRL, HIGH); 
    init_i2c();
    init_es8311();
    init_i2s();

    // 5. 墨水屏初始化
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
    Serial.println("System Ready. Short press to switch image, Long press to record.");
}

// ================= 主循环 =================
void loop() {
    button.update();

    const size_t chunk_size = 1024; 
    size_t bytes_transferred = 0;

    // --- 1. 按键刚按下瞬间 ---
    if (button.fell()) { 
        longPressHandled = false; 
        if (audio_state == STATE_PLAYING) {
            // 如果正在播放（无论是录音还是内置音频），按下按钮立刻打断
            audio_state = STATE_IDLE;
            Serial.println("Playback interrupted by button press.");
        }
    }

    // --- 2. 按钮持续按住的状态 ---
    if (button.read() == LOW) {
        if (button.currentDuration() >= LONG_PRESS_MS && !longPressHandled) {
            longPressHandled = true; 
            Serial.println(">>> LONG PRESS detected! Start Recording...");
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
            // === 情况 A：长按结束，播放刚录制的声音 ===
            Serial.printf(">>> Recording Finished. Size: %d bytes. Start Playing...\n", recorded_size);
            
            // 将播放数据源指向 PSRAM 里的录音数据
            current_play_buffer = psram_audio_buffer;
            current_play_total_size = recorded_size;
            play_offset = 0;
            audio_state = STATE_PLAYING;
            
        } else {
            // === 情况 B：短按，切换墨水屏并播放内置音频 ===
            Serial.println(">>> SHORT PRESS detected! Switching Image...");
            currentImageIndex = (currentImageIndex + 1) % 2;
            
            // 1. 刷新墨水屏 (这是阻塞过程，需要几秒钟)
            showImageAndName(currentImageIndex);
            
            // 2. 屏幕刷新完毕后，配置对应的内置音频开始播放
            if (currentImageIndex == 0) {
                current_play_buffer = audio_sunianxue_pcm;
                current_play_total_size = audio_sunianxue_len;
            } else {
                current_play_buffer = audio_liuruyi_pcm;
                current_play_total_size = audio_liuruyi_len;
            }
            
            play_offset = 0;
            audio_state = STATE_PLAYING;
            Serial.println(">>> Start playing built-in audio...");
        }
    }

    // --- 4. 空闲时的持续播放逻辑 (兼容录音和内置音频) ---
    if (audio_state == STATE_PLAYING && button.read() == HIGH) {
        if (play_offset < current_play_total_size) {
            // 每次最多写入 chunk_size，防止越界
            size_t bytes_to_write = min(chunk_size, current_play_total_size - play_offset);
            
            // 从 current_play_buffer 写入 I2S 播放
            i2s_write(I2S_PORT_NUM, current_play_buffer + play_offset, bytes_to_write, &bytes_transferred, portMAX_DELAY);
            play_offset += bytes_transferred;
        } else {
            Serial.println(">>> Playback Complete.");
            audio_state = STATE_IDLE;
        }
    }
}

// ================= 辅助模块实现 =================

// 墨水屏刷新函数 (保留你原本的代码)
void showImageAndName(int imageIndex) {
    Serial.println("Updating e-ink display...");
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_BLACK);
        if (imageIndex == 0) {
            display.drawInvertedBitmap(0, 0, gImage_sunianxue, 200, 200, GxEPD_WHITE);
        } else {
            display.drawInvertedBitmap(0, 0, gImage_liuruyi, 200, 200, GxEPD_WHITE);
        }
        int textX = 10;        
        int startY = 30;       
        int lineSpacing = 30;  
        if (imageIndex == 0) {
            u8g2Fonts.setCursor(textX, startY); u8g2Fonts.print("苏");
            u8g2Fonts.setCursor(textX, startY + lineSpacing); u8g2Fonts.print("念");
            u8g2Fonts.setCursor(textX, startY + lineSpacing * 2); u8g2Fonts.print("雪");
        } else {
            u8g2Fonts.setCursor(textX, startY); u8g2Fonts.print("柳");
            u8g2Fonts.setCursor(textX, startY + lineSpacing); u8g2Fonts.print("如");
            u8g2Fonts.setCursor(textX, startY + lineSpacing * 2); u8g2Fonts.print("意");
        }
    } while (display.nextPage()); 
    Serial.println("E-ink update complete.");
}

// 音频底层初始化函数 (复用前面的代码)
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