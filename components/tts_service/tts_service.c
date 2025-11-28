/**
 * 本地 TTS 语音合成服务实现
 * 简化版本：使用蜂鸣提示音代替语音合成
 * 后续可扩展为真正的本地 TTS
 */

#include "tts_service.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "LOCAL_TTS";

// ES8311 音频编解码器地址和寄存器
#define ES8311_ADDR 0x18
#define ES8311_REG_RESET 0x00
#define ES8311_REG_CLK_MANAGER1 0x01
#define ES8311_REG_CLK_MANAGER2 0x02
#define ES8311_REG_CLK_MANAGER3 0x03
#define ES8311_REG_CLK_MANAGER4 0x04
#define ES8311_REG_CLK_MANAGER5 0x05
#define ES8311_REG_CLK_MANAGER6 0x06
#define ES8311_REG_CLK_MANAGER7 0x07
#define ES8311_REG_CLK_MANAGER8 0x08
#define ES8311_REG_SDP_IN 0x09
#define ES8311_REG_SDP_OUT 0x0A
#define ES8311_REG_SYSTEM 0x0D
#define ES8311_REG_ADC_CTRL1 0x0F
#define ES8311_REG_ADC_CTRL2 0x10
#define ES8311_REG_DAC_CTRL1 0x12
#define ES8311_REG_DAC_CTRL2 0x13
#define ES8311_REG_DAC_CTRL3 0x14
#define ES8311_REG_GPIO 0x44
#define ES8311_REG_DAC_VOL 0x32

// PCA9557 IO 扩展芯片 (用于控制音频放大器)
#define PCA9557_ADDR 0x19
#define PCA9557_REG_OUTPUT 0x01
#define PCA9557_REG_CONFIG 0x03

#define TTS_TEXT_QUEUE_SIZE 10
#define TTS_MAX_TEXT_LEN 512
#define SAMPLE_RATE 16000
#define TONE_DURATION_MS 100

typedef struct {
    tts_config_t config;
    i2s_chan_handle_t i2s_tx_handle;
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t es8311_dev;
    i2c_master_dev_handle_t pca9557_dev;
    QueueHandle_t text_queue;
    TaskHandle_t task_handle;
    bool is_playing;
    bool should_stop;
    bool initialized;
    bool pa_enabled;
} tts_service_t;

static tts_service_t *s_tts = NULL;

// PCA9557 写寄存器
static esp_err_t pca9557_write_reg(uint8_t reg, uint8_t data) {
    if (s_tts == NULL || s_tts->pca9557_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t write_buf[2] = {reg, data};
    return i2c_master_transmit(s_tts->pca9557_dev, write_buf, 2, -1);
}

// PCA9557 读寄存器
static esp_err_t pca9557_read_reg(uint8_t reg, uint8_t *data) {
    if (s_tts == NULL || s_tts->pca9557_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_tts->pca9557_dev, &reg, 1, data, 1, -1);
}

// 使能/禁用音频放大器 (通过 PCA9557 bit 1)
static esp_err_t enable_audio_pa(bool enable) {
    uint8_t data;
    esp_err_t ret = pca9557_read_reg(PCA9557_REG_OUTPUT, &data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read PCA9557: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (enable) {
        data |= (1 << 1);  // Set bit 1 high to enable PA
    } else {
        data &= ~(1 << 1); // Set bit 1 low to disable PA
    }
    
    ret = pca9557_write_reg(PCA9557_REG_OUTPUT, data);
    if (ret == ESP_OK) {
        s_tts->pa_enabled = enable;
        ESP_LOGI(TAG, "Audio PA %s", enable ? "enabled" : "disabled");
    }
    return ret;
}

// ES8311 写寄存器
static esp_err_t es8311_write_reg(uint8_t reg, uint8_t data) {
    if (s_tts == NULL || s_tts->es8311_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t write_buf[2] = {reg, data};
    return i2c_master_transmit(s_tts->es8311_dev, write_buf, 2, -1);
}

// 初始化 ES8311 音频编解码器 (简化配置，仅 DAC 输出)
static esp_err_t init_es8311(void) {
    ESP_LOGI(TAG, "Initializing ES8311 audio codec...");
    
    // 软复位
    es8311_write_reg(ES8311_REG_RESET, 0x80);
    vTaskDelay(pdMS_TO_TICKS(20));
    es8311_write_reg(ES8311_REG_RESET, 0x00);
    vTaskDelay(pdMS_TO_TICKS(20));
    
    // 时钟配置 - 使用 MCLK
    es8311_write_reg(ES8311_REG_CLK_MANAGER1, 0x3F);  // MCLK power on
    es8311_write_reg(ES8311_REG_CLK_MANAGER2, 0x00);  // MCLK divider
    es8311_write_reg(ES8311_REG_CLK_MANAGER3, 0x10);  // LRCK divider
    es8311_write_reg(ES8311_REG_CLK_MANAGER4, 0x10);  // BCLK divider
    es8311_write_reg(ES8311_REG_CLK_MANAGER5, 0x00);  // ADC/DAC clock
    es8311_write_reg(ES8311_REG_CLK_MANAGER6, 0x00);  // ADC osr
    es8311_write_reg(ES8311_REG_CLK_MANAGER7, 0x00);  // DAC osr
    es8311_write_reg(ES8311_REG_CLK_MANAGER8, 0xFF);  // Clock enable
    
    // SDP 配置 - I2S 16bit
    es8311_write_reg(ES8311_REG_SDP_IN, 0x00);   // I2S, 16bit
    es8311_write_reg(ES8311_REG_SDP_OUT, 0x00);  // I2S, 16bit
    
    // 系统配置
    es8311_write_reg(ES8311_REG_SYSTEM, 0x00);   // Power up
    
    // DAC 配置
    es8311_write_reg(ES8311_REG_DAC_CTRL1, 0x12);  // DAC power on
    es8311_write_reg(ES8311_REG_DAC_CTRL2, 0x02);  // DAC unmute
    es8311_write_reg(ES8311_REG_DAC_CTRL3, 0x00);  // DAC volume ramp
    es8311_write_reg(ES8311_REG_DAC_VOL, 0xBF);    // DAC volume (0xBF = 0dB)
    
    // GPIO 配置
    es8311_write_reg(ES8311_REG_GPIO, 0x00);
    
    ESP_LOGI(TAG, "ES8311 initialized");
    return ESP_OK;
}

// 初始化 I2C 总线和设备
static esp_err_t init_i2c_devices(i2c_master_bus_handle_t external_i2c_bus) {
    ESP_LOGI(TAG, "Initializing I2C devices for audio...");
    
    // 使用外部传入的 I2C 总线
    if (external_i2c_bus != NULL) {
        s_tts->i2c_bus = external_i2c_bus;
        ESP_LOGI(TAG, "Using external I2C bus");
    } else {
        // 创建新的 I2C 总线 (使用 I2C_NUM_1)
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = 1,  // GPIO1
            .scl_io_num = 2,  // GPIO2
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        
        esp_err_t ret = i2c_new_master_bus(&i2c_bus_cfg, &s_tts->i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create I2C bus: %s, PA control disabled", esp_err_to_name(ret));
            s_tts->i2c_bus = NULL;
            return ESP_OK;  // 继续，只是没有 PA 控制
        }
    }
    
    // 添加 PCA9557 设备
    if (s_tts->i2c_bus != NULL) {
        i2c_device_config_t pca9557_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = PCA9557_ADDR,
            .scl_speed_hz = 100000,
        };
        esp_err_t ret = i2c_master_bus_add_device(s_tts->i2c_bus, &pca9557_cfg, &s_tts->pca9557_dev);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to add PCA9557 device: %s", esp_err_to_name(ret));
            s_tts->pca9557_dev = NULL;
        }
        
        // 添加 ES8311 设备
        i2c_device_config_t es8311_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ES8311_ADDR,
            .scl_speed_hz = 100000,
        };
        ret = i2c_master_bus_add_device(s_tts->i2c_bus, &es8311_cfg, &s_tts->es8311_dev);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to add ES8311 device: %s", esp_err_to_name(ret));
            s_tts->es8311_dev = NULL;
        } else {
            // 初始化 ES8311
            ret = init_es8311();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "ES8311 init failed, audio may not work properly");
            }
        }
    }
    
    return ESP_OK;
}

// 生成简单的提示音
static void generate_beep(int16_t *buffer, size_t samples, int freq) {
    for (size_t i = 0; i < samples; i++) {
        float t = (float)i / SAMPLE_RATE;
        buffer[i] = (int16_t)(16000 * sinf(2.0f * M_PI * freq * t));
    }
}

// 初始化 I2S 音频输出
static esp_err_t init_i2s_output(void) {
    if (s_tts == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // 配置 I2S 通道
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tts->i2s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置 I2S 标准模式
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = s_tts->config.i2s_mclk_pin,
            .bclk = s_tts->config.i2s_bclk_pin,
            .ws = s_tts->config.i2s_ws_pin,
            .dout = s_tts->config.i2s_dout_pin,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_tts->i2s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S std mode: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tts->i2s_tx_handle);
        return ret;
    }

    ret = i2s_channel_enable(s_tts->i2s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tts->i2s_tx_handle);
        return ret;
    }

    ESP_LOGI(TAG, "I2S output initialized (MCLK=%d, BCLK=%d, WS=%d, DOUT=%d)",
             s_tts->config.i2s_mclk_pin, s_tts->config.i2s_bclk_pin,
             s_tts->config.i2s_ws_pin, s_tts->config.i2s_dout_pin);
    return ESP_OK;
}

// 播放提示音 (简化版 TTS)
static esp_err_t play_notification_sound(const char *text) {
    if (s_tts == NULL || s_tts->i2s_tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "播放提示音 (文本: %s)", text);

    // 使能音频放大器
    if (!s_tts->pa_enabled && s_tts->pca9557_dev != NULL) {
        enable_audio_pa(true);
    }

    // 通知播放开始
    if (s_tts->config.callback) {
        s_tts->config.callback(TTS_EVENT_START, s_tts->config.user_data);
    }
    s_tts->is_playing = true;

    // 生成提示音
    size_t samples = SAMPLE_RATE * TONE_DURATION_MS / 1000;
    int16_t *buffer = malloc(samples * sizeof(int16_t));
    if (buffer == NULL) {
        s_tts->is_playing = false;
        return ESP_ERR_NO_MEM;
    }

    // 根据文本长度播放不同次数的提示音
    int beep_count = (strlen(text) / 10) + 1;
    if (beep_count > 5) beep_count = 5;

    for (int i = 0; i < beep_count && !s_tts->should_stop; i++) {
        // 生成不同频率的提示音
        int freq = 800 + (i * 100);
        generate_beep(buffer, samples, freq);

        // 播放
        size_t bytes_written = 0;
        i2s_channel_write(s_tts->i2s_tx_handle, buffer,
                          samples * sizeof(int16_t), &bytes_written,
                          pdMS_TO_TICKS(500));

        // 短暂停顿
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    free(buffer);
    s_tts->is_playing = false;

    // 通知播放结束
    if (s_tts->config.callback) {
        s_tts->config.callback(TTS_EVENT_STOP, s_tts->config.user_data);
    }

    return ESP_OK;
}

// TTS 任务
static void tts_task(void *arg) {
    char text[TTS_MAX_TEXT_LEN];

    while (!s_tts->should_stop) {
        if (xQueueReceive(s_tts->text_queue, text, pdMS_TO_TICKS(100)) == pdTRUE) {
            play_notification_sound(text);
        }
    }

    vTaskDelete(NULL);
}


// 初始化 TTS 服务
esp_err_t tts_service_init(const tts_config_t *config) {
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_tts != NULL) {
        ESP_LOGW(TAG, "TTS service already initialized");
        return ESP_OK;
    }

    s_tts = calloc(1, sizeof(tts_service_t));
    if (s_tts == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // 复制配置
    s_tts->config = *config;

    // 设置默认值
    if (s_tts->config.sample_rate == 0) s_tts->config.sample_rate = SAMPLE_RATE;
    if (s_tts->config.speed == 0) s_tts->config.speed = 1;

    // 默认 I2S 引脚 (立创实战派 ESP32-S3)
    if (s_tts->config.i2s_mclk_pin == 0) s_tts->config.i2s_mclk_pin = 38;
    if (s_tts->config.i2s_bclk_pin == 0) s_tts->config.i2s_bclk_pin = 14;
    if (s_tts->config.i2s_ws_pin == 0) s_tts->config.i2s_ws_pin = 13;
    if (s_tts->config.i2s_dout_pin == 0) s_tts->config.i2s_dout_pin = 45;

    ESP_LOGI(TAG, "Initializing local TTS service (beep mode)...");

    // 初始化 I2C 设备 (PCA9557 和 ES8311)
    esp_err_t ret = init_i2c_devices((i2c_master_bus_handle_t)s_tts->config.i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C devices init failed, PA control may not work");
        // 继续执行，I2S 可能仍然可以工作
    }

    // 初始化 I2S 输出
    ret = init_i2s_output();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S output");
        free(s_tts);
        s_tts = NULL;
        return ret;
    }
    
    // 使能音频放大器
    if (s_tts->pca9557_dev != NULL) {
        enable_audio_pa(true);
    }

    // 创建文本队列
    s_tts->text_queue = xQueueCreate(TTS_TEXT_QUEUE_SIZE, TTS_MAX_TEXT_LEN);
    if (s_tts->text_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create text queue");
        i2s_channel_disable(s_tts->i2s_tx_handle);
        i2s_del_channel(s_tts->i2s_tx_handle);
        free(s_tts);
        s_tts = NULL;
        return ESP_ERR_NO_MEM;
    }

    // 创建 TTS 任务
    BaseType_t task_ret = xTaskCreate(tts_task, "local_tts", 4096, NULL, 5, &s_tts->task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TTS task");
        vQueueDelete(s_tts->text_queue);
        i2s_channel_disable(s_tts->i2s_tx_handle);
        i2s_del_channel(s_tts->i2s_tx_handle);
        free(s_tts);
        s_tts = NULL;
        return ESP_FAIL;
    }

    s_tts->initialized = true;
    ESP_LOGI(TAG, "Local TTS service initialized (beep notification mode)");
    return ESP_OK;
}

// 文本转语音并播放 (同步)
esp_err_t tts_speak(const char *text) {
    if (s_tts == NULL || text == NULL || !s_tts->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return play_notification_sound(text);
}

// 将文本添加到播放队列 (异步)
esp_err_t tts_speak_async(const char *text) {
    if (s_tts == NULL || text == NULL || !s_tts->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    char text_buf[TTS_MAX_TEXT_LEN];
    strncpy(text_buf, text, TTS_MAX_TEXT_LEN - 1);
    text_buf[TTS_MAX_TEXT_LEN - 1] = '\0';

    if (xQueueSend(s_tts->text_queue, text_buf, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "TTS queue full");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

// 停止当前播放
esp_err_t tts_stop(void) {
    if (s_tts == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xQueueReset(s_tts->text_queue);
    return ESP_OK;
}

// 检查是否正在播放
bool tts_is_playing(void) {
    return s_tts != NULL && s_tts->is_playing;
}

// 销毁 TTS 服务
void tts_service_destroy(void) {
    if (s_tts == NULL) {
        return;
    }

    s_tts->should_stop = true;

    if (s_tts->task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (s_tts->text_queue != NULL) {
        vQueueDelete(s_tts->text_queue);
    }

    if (s_tts->i2s_tx_handle != NULL) {
        i2s_channel_disable(s_tts->i2s_tx_handle);
        i2s_del_channel(s_tts->i2s_tx_handle);
    }

    free(s_tts);
    s_tts = NULL;

    ESP_LOGI(TAG, "Local TTS service destroyed");
}
