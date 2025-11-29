/**
 * 流式 TTS 语音合成服务实现
 * 
 * 从 SSE 流接收文本，按标点符号分句，通过百度 TTS API 进行流式语音合成和播放。
 * 采用生产者-消费者模式，通过两级队列实现低延迟的流式播报。
 */

#include "streaming_tts.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "STREAMING_TTS";

// 队列配置
#define RAW_TEXT_QUEUE_SIZE     20      // 原始文本队列大小
#define SENTENCE_QUEUE_SIZE     10      // 分句队列大小
#define RAW_TEXT_MAX_LEN        256     // 单个原始文本片段最大长度
#define SENTENCE_MAX_LEN        512     // 单个句子最大长度
#define SENTENCE_BUFFER_SIZE    512     // 分句缓冲区大小

// 音频配置
#define SAMPLE_RATE             16000
#define AUDIO_BUFFER_SIZE       (32 * 1024)  // 32KB 音频缓冲区

// 百度 TTS API
#define BAIDU_TOKEN_URL         "https://aip.baidubce.com/oauth/2.0/token"
#define BAIDU_TTS_URL           "https://tsn.baidu.com/text2audio"

// ES8311 音频编解码器地址
#define ES8311_ADDR             0x30

// PCA9557 IO 扩展芯片
#define PCA9557_ADDR            0x19
#define PCA9557_REG_OUTPUT      0x01

// 队列超时
#define QUEUE_SEND_TIMEOUT_MS   5000
#define QUEUE_RECV_TIMEOUT_MS   100

/**
 * 流式 TTS 内部状态结构体
 */
typedef struct {
    // 配置
    streaming_tts_config_t config;
    
    // 队列
    QueueHandle_t raw_text_queue;       // 原始文本队列
    QueueHandle_t sentence_queue;       // 分句队列
    
    // 任务
    TaskHandle_t splitter_task;         // 分句任务
    TaskHandle_t player_task;           // TTS 播放任务
    
    // 状态
    volatile bool stream_ended;         // 流是否结束
    volatile bool is_playing;           // 是否正在播放
    volatile bool should_stop;          // 停止标志
    volatile bool initialized;          // 是否已初始化

    // 分句缓冲区
    char sentence_buffer[SENTENCE_BUFFER_SIZE];
    size_t buffer_pos;
    
    // I2S 和音频编解码器
    i2s_chan_handle_t i2s_tx_handle;
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t pca9557_dev;
    
    // ESP Codec Dev
    const audio_codec_data_if_t *data_if;
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_gpio_if_t *gpio_if;
    const audio_codec_if_t *codec_if;
    esp_codec_dev_handle_t codec_dev;
    
    // 百度 TTS
    char *access_token;
    int64_t token_expire_time;
    
    // 音频缓冲区
    uint8_t *audio_buffer;
    size_t audio_buffer_size;
    
    // PA 状态
    bool pa_enabled;
    
    // 播放完成信号量
    SemaphoreHandle_t play_done_sem;
    volatile size_t pending_bytes;      // 待播放的字节数
} streaming_tts_t;

// 全局实例
static streaming_tts_t *s_tts = NULL;

// ============================================================================
// 内部辅助函数声明
// ============================================================================

static esp_err_t init_i2c_devices(i2c_master_bus_handle_t external_i2c_bus);
static esp_err_t init_es8311_codec(void);
static esp_err_t enable_audio_pa(bool enable);
static void splitter_task(void *arg);
static void player_task(void *arg);
static bool is_chinese_punctuation(const char *str, size_t *char_len);
static size_t split_by_punctuation(const char *input, char *sentence_out, size_t sentence_max_len);
static size_t flush_remaining_text(char *sentence_out, size_t sentence_max_len);
static size_t utf8_char_count(const char *str);

// ============================================================================
// I2S 事件回调
// ============================================================================

/**
 * I2S TX 发送完成回调
 * 
 * 当 DMA 缓冲区中的数据发送完成时触发。
 * 用于精确检测音频播放完成。
 */
static IRAM_ATTR bool i2s_tx_sent_callback(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
    BaseType_t high_task_wakeup = pdFALSE;
    
    if (s_tts != NULL && s_tts->play_done_sem != NULL) {
        // 减少待播放字节数
        if (s_tts->pending_bytes >= event->size) {
            s_tts->pending_bytes -= event->size;
        } else {
            s_tts->pending_bytes = 0;
        }
        
        // 当所有数据播放完成时，发送信号量
        if (s_tts->pending_bytes == 0) {
            xSemaphoreGiveFromISR(s_tts->play_done_sem, &high_task_wakeup);
        }
    }
    
    return high_task_wakeup == pdTRUE;
}

// ============================================================================
// PCA9557 IO 扩展芯片操作
// ============================================================================

static esp_err_t pca9557_write_reg(uint8_t reg, uint8_t data) {
    if (s_tts == NULL || s_tts->pca9557_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t write_buf[2] = {reg, data};
    return i2c_master_transmit(s_tts->pca9557_dev, write_buf, 2, -1);
}

static esp_err_t pca9557_read_reg(uint8_t reg, uint8_t *data) {
    if (s_tts == NULL || s_tts->pca9557_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_tts->pca9557_dev, &reg, 1, data, 1, -1);
}

static esp_err_t enable_audio_pa(bool enable) {
    if (s_tts == NULL || s_tts->pca9557_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    uint8_t data;
    esp_err_t ret = pca9557_read_reg(PCA9557_REG_OUTPUT, &data);
    if (ret != ESP_OK) {
        return ret;
    }
    
    if (enable) {
        data |= (1 << 1);
    } else {
        data &= ~(1 << 1);
    }
    
    ret = pca9557_write_reg(PCA9557_REG_OUTPUT, data);
    if (ret == ESP_OK) {
        s_tts->pa_enabled = enable;
        ESP_LOGI(TAG, "Audio PA %s", enable ? "enabled" : "disabled");
    }
    return ret;
}

// ============================================================================
// 分句逻辑实现
// ============================================================================

/**
 * 检查字符串开头是否为中文标点符号
 * 
 * 支持的中文标点：。！？，；：（UTF-8 编码）
 * 
 * @param str 输入字符串
 * @param char_len 输出参数，返回标点符号的字节长度
 * @return true 如果是中文标点，false 否则
 * 
 * Requirements: 2.2
 */
static bool is_chinese_punctuation(const char *str, size_t *char_len) {
    if (str == NULL || *str == '\0') {
        return false;
    }
    
    // 中文标点符号 UTF-8 编码（3字节）
    // 。= E3 80 82
    // ！= EF BC 81
    // ？= EF BC 9F
    // ，= EF BC 8C
    // ；= EF BC 9B
    // ：= EF BC 9A
    static const char *punctuations[] = {
        "\xE3\x80\x82",  // 。
        "\xEF\xBC\x81",  // ！
        "\xEF\xBC\x9F",  // ？
        "\xEF\xBC\x8C",  // ，
        "\xEF\xBC\x9B",  // ；
        "\xEF\xBC\x9A",  // ：
    };
    static const size_t punct_count = sizeof(punctuations) / sizeof(punctuations[0]);
    
    for (size_t i = 0; i < punct_count; i++) {
        size_t len = strlen(punctuations[i]);
        if (strncmp(str, punctuations[i], len) == 0) {
            if (char_len != NULL) {
                *char_len = len;
            }
            return true;
        }
    }
    
    return false;
}

/**
 * 计算 UTF-8 字符串的字符数（非字节数）
 * 
 * @param str UTF-8 字符串
 * @return 字符数
 */
static size_t utf8_char_count(const char *str) {
    if (str == NULL) {
        return 0;
    }
    
    size_t count = 0;
    const unsigned char *p = (const unsigned char *)str;
    
    while (*p != '\0') {
        // UTF-8 编码规则：
        // 0xxxxxxx - 单字节 (ASCII)
        // 110xxxxx - 双字节起始
        // 1110xxxx - 三字节起始
        // 11110xxx - 四字节起始
        // 10xxxxxx - 续字节
        if ((*p & 0xC0) != 0x80) {
            // 不是续字节，是一个新字符的开始
            count++;
        }
        p++;
    }
    
    return count;
}

/**
 * 按中文标点符号分句
 * 
 * 从内部缓冲区中查找第一个中文标点符号，将标点前的内容（包含标点）作为一个句子输出。
 * 如果找到句子，会从内部缓冲区中移除该句子。
 * 
 * @param input 输入文本（追加到内部缓冲区）
 * @param sentence_out 输出句子缓冲区
 * @param sentence_max_len 输出缓冲区最大长度
 * @return 输出句子的字节长度，0 表示没有完整句子
 * 
 * Requirements: 2.2, 2.3, 2.4
 */
static size_t split_by_punctuation(const char *input, char *sentence_out, size_t sentence_max_len) {
    if (s_tts == NULL || sentence_out == NULL || sentence_max_len == 0) {
        return 0;
    }
    
    // 追加输入文本到内部缓冲区
    if (input != NULL && *input != '\0') {
        size_t input_len = strlen(input);
        size_t remaining = SENTENCE_BUFFER_SIZE - s_tts->buffer_pos - 1;
        
        if (input_len > remaining) {
            input_len = remaining;
            ESP_LOGW(TAG, "Sentence buffer overflow, truncating input");
        }
        
        if (input_len > 0) {
            memcpy(s_tts->sentence_buffer + s_tts->buffer_pos, input, input_len);
            s_tts->buffer_pos += input_len;
            s_tts->sentence_buffer[s_tts->buffer_pos] = '\0';
        }
    }
    
    // 在缓冲区中查找中文标点
    const char *p = s_tts->sentence_buffer;
    size_t pos = 0;
    
    while (pos < s_tts->buffer_pos) {
        size_t punct_len = 0;
        if (is_chinese_punctuation(p, &punct_len)) {
            // 找到标点，计算句子长度（包含标点）
            size_t sentence_len = pos + punct_len;
            
            // 检查句子字符数是否小于 2（Requirements 2.4）
            char temp[SENTENCE_BUFFER_SIZE];
            if (sentence_len < sentence_max_len && sentence_len < SENTENCE_BUFFER_SIZE) {
                memcpy(temp, s_tts->sentence_buffer, sentence_len);
                temp[sentence_len] = '\0';
                
                size_t char_count = utf8_char_count(temp);
                
                if (char_count < 2) {
                    // 句子太短，跳过这个标点，继续查找下一个
                    ESP_LOGD(TAG, "Skipping short sentence (%zu chars): %s", char_count, temp);
                    p += punct_len;
                    pos += punct_len;
                    continue;
                }
            }
            
            // 复制句子到输出缓冲区
            if (sentence_len >= sentence_max_len) {
                sentence_len = sentence_max_len - 1;
            }
            memcpy(sentence_out, s_tts->sentence_buffer, sentence_len);
            sentence_out[sentence_len] = '\0';
            
            // 从内部缓冲区移除已处理的句子
            size_t remaining = s_tts->buffer_pos - (pos + punct_len);
            if (remaining > 0) {
                memmove(s_tts->sentence_buffer, s_tts->sentence_buffer + pos + punct_len, remaining);
            }
            s_tts->buffer_pos = remaining;
            s_tts->sentence_buffer[s_tts->buffer_pos] = '\0';
            
            ESP_LOGD(TAG, "Split sentence (%zu bytes): %s", sentence_len, sentence_out);
            return sentence_len;
        }
        
        // 移动到下一个字符
        // UTF-8 字符长度判断
        unsigned char c = (unsigned char)*p;
        size_t char_bytes = 1;
        if ((c & 0xE0) == 0xC0) {
            char_bytes = 2;
        } else if ((c & 0xF0) == 0xE0) {
            char_bytes = 3;
        } else if ((c & 0xF8) == 0xF0) {
            char_bytes = 4;
        }
        
        p += char_bytes;
        pos += char_bytes;
    }
    
    // 没有找到标点，返回 0
    return 0;
}

/**
 * 获取缓冲区中的剩余文本（流结束时调用）
 * 
 * @param sentence_out 输出句子缓冲区
 * @param sentence_max_len 输出缓冲区最大长度
 * @return 输出句子的字节长度，0 表示缓冲区为空
 * 
 * Requirements: 2.3
 */
static size_t flush_remaining_text(char *sentence_out, size_t sentence_max_len) {
    if (s_tts == NULL || sentence_out == NULL || sentence_max_len == 0) {
        return 0;
    }
    
    if (s_tts->buffer_pos == 0) {
        return 0;
    }
    
    // 检查剩余文本字符数是否小于 2（Requirements 2.4）
    size_t char_count = utf8_char_count(s_tts->sentence_buffer);
    if (char_count < 2) {
        ESP_LOGD(TAG, "Skipping short remaining text (%zu chars): %s", char_count, s_tts->sentence_buffer);
        s_tts->buffer_pos = 0;
        s_tts->sentence_buffer[0] = '\0';
        return 0;
    }
    
    // 复制剩余文本到输出缓冲区
    size_t len = s_tts->buffer_pos;
    if (len >= sentence_max_len) {
        len = sentence_max_len - 1;
    }
    
    memcpy(sentence_out, s_tts->sentence_buffer, len);
    sentence_out[len] = '\0';
    
    // 清空内部缓冲区
    s_tts->buffer_pos = 0;
    s_tts->sentence_buffer[0] = '\0';
    
    ESP_LOGD(TAG, "Flushed remaining text (%zu bytes): %s", len, sentence_out);
    return len;
}

// ============================================================================
// I2C 设备初始化
// ============================================================================

static esp_err_t init_i2c_devices(i2c_master_bus_handle_t external_i2c_bus) {
    if (external_i2c_bus != NULL) {
        s_tts->i2c_bus = external_i2c_bus;
    } else {
        return ESP_OK;
    }
    
    // 添加 PCA9557 设备
    i2c_device_config_t pca9557_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(s_tts->i2c_bus, &pca9557_cfg, &s_tts->pca9557_dev);
    if (ret != ESP_OK) {
        s_tts->pca9557_dev = NULL;
        ESP_LOGW(TAG, "PCA9557 not found, PA control disabled");
    }
    
    return ESP_OK;
}

// ============================================================================
// ES8311 编解码器初始化
// ============================================================================

static esp_err_t init_es8311_codec(void) {
    ESP_LOGI(TAG, "Initializing ES8311 codec...");
    
    // 创建 I2S 通道
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tts->i2s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel");
        return ret;
    }
    
    // 配置 I2S 标准模式
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
        },
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
        i2s_del_channel(s_tts->i2s_tx_handle);
        return ret;
    }
    
    // 注册 I2S TX 发送完成回调
    i2s_event_callbacks_t cbs = {
        .on_recv = NULL,
        .on_recv_q_ovf = NULL,
        .on_sent = i2s_tx_sent_callback,
        .on_send_q_ovf = NULL,
    };
    ret = i2s_channel_register_event_callback(s_tts->i2s_tx_handle, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register I2S callback: %s", esp_err_to_name(ret));
        // 继续执行，回调失败不影响基本功能
    }
    
    // 创建 I2S 数据接口
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = NULL,
        .tx_handle = s_tts->i2s_tx_handle,
    };
    s_tts->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_tts->data_if == NULL) {
        i2s_del_channel(s_tts->i2s_tx_handle);
        return ESP_FAIL;
    }
    
    // 创建 I2C 控制接口
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = ES8311_ADDR,
        .bus_handle = s_tts->i2c_bus,
    };
    s_tts->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_tts->ctrl_if == NULL) {
        return ESP_FAIL;
    }
    
    // 创建 GPIO 接口
    s_tts->gpio_if = audio_codec_new_gpio();
    if (s_tts->gpio_if == NULL) {
        return ESP_FAIL;
    }
    
    // 创建 ES8311 编解码器
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_tts->ctrl_if,
        .gpio_if = s_tts->gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .use_mclk = true,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    s_tts->codec_if = es8311_codec_new(&es8311_cfg);
    if (s_tts->codec_if == NULL) {
        return ESP_FAIL;
    }
    
    // 创建编解码器设备
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_tts->codec_if,
        .data_if = s_tts->data_if,
    };
    s_tts->codec_dev = esp_codec_dev_new(&dev_cfg);
    if (s_tts->codec_dev == NULL) {
        return ESP_FAIL;
    }
    
    // 打开编解码器
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ret = esp_codec_dev_open(s_tts->codec_dev, &fs);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // 设置音量
    esp_codec_dev_set_out_vol(s_tts->codec_dev, 80);
    
    ESP_LOGI(TAG, "ES8311 codec initialized");
    return ESP_OK;
}


// ============================================================================
// 分句任务
// ============================================================================

/**
 * 分句任务
 * 
 * 从原始文本队列读取文本，调用分句逻辑，将完整句子推入分句队列。
 * 
 * Requirements: 2.1, 2.2, 2.3
 */
static void splitter_task(void *arg) {
    ESP_LOGI(TAG, "Splitter task started");
    
    char raw_text[RAW_TEXT_MAX_LEN];
    char sentence[SENTENCE_MAX_LEN];
    bool stream_end_processed = false;
    
    while (!s_tts->should_stop) {
        // 从原始文本队列读取 (Requirements 2.1)
        if (xQueueReceive(s_tts->raw_text_queue, raw_text, pdMS_TO_TICKS(QUEUE_RECV_TIMEOUT_MS)) == pdTRUE) {
            ESP_LOGD(TAG, "Received raw text: %s", raw_text);
            
            // 调用分句逻辑，提取所有完整句子 (Requirements 2.2)
            size_t len = split_by_punctuation(raw_text, sentence, SENTENCE_MAX_LEN);
            while (len > 0) {
                // 将句子推入分句队列
                if (xQueueSend(s_tts->sentence_queue, sentence, pdMS_TO_TICKS(QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
                    ESP_LOGW(TAG, "Sentence queue full, timeout");
                } else {
                    ESP_LOGD(TAG, "Sentence queued: %s", sentence);
                }
                
                // 继续提取下一个句子
                len = split_by_punctuation(NULL, sentence, SENTENCE_MAX_LEN);
            }
            
            // 重置流结束处理标志（有新数据进来）
            stream_end_processed = false;
        }
        
        // 检查流是否结束 (Requirements 2.3)
        if (s_tts->stream_ended && !stream_end_processed) {
            ESP_LOGI(TAG, "Stream ended, flushing remaining text");
            
            // 处理剩余文本
            size_t len = flush_remaining_text(sentence, SENTENCE_MAX_LEN);
            if (len > 0) {
                if (xQueueSend(s_tts->sentence_queue, sentence, pdMS_TO_TICKS(QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
                    ESP_LOGW(TAG, "Sentence queue full, timeout");
                } else {
                    ESP_LOGI(TAG, "Final sentence queued: %s", sentence);
                }
            }
            
            stream_end_processed = true;
        }
    }
    
    ESP_LOGI(TAG, "Splitter task stopped");
    vTaskDelete(NULL);
}

// ============================================================================
// 百度 TTS API 实现
// ============================================================================

/**
 * Token HTTP 响应上下文
 */
typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t data_len;
} token_response_t;

/**
 * Token HTTP 事件处理器
 */
static esp_err_t token_http_event_handler(esp_http_client_event_t *evt) {
    token_response_t *ctx = (token_response_t *)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (ctx != NULL && ctx->buffer != NULL) {
                if (ctx->data_len + evt->data_len < ctx->buffer_size) {
                    memcpy(ctx->buffer + ctx->data_len, evt->data, evt->data_len);
                    ctx->data_len += evt->data_len;
                    ctx->buffer[ctx->data_len] = '\0';
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/**
 * 获取百度 access_token
 * 
 * Requirements: 3.1
 */
static esp_err_t get_baidu_access_token(void) {
    if (s_tts == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // 检查 token 是否有效
    if (s_tts->access_token != NULL && s_tts->token_expire_time > esp_timer_get_time() / 1000000) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Getting Baidu access_token...");
    
    // 准备响应缓冲区
    token_response_t response_ctx = {
        .buffer = malloc(2048),
        .buffer_size = 2048,
        .data_len = 0,
    };
    if (response_ctx.buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    response_ctx.buffer[0] = '\0';
    
    // 构建 URL
    char url[512];
    snprintf(url, sizeof(url),
             "%s?grant_type=client_credentials&client_id=%s&client_secret=%s",
             BAIDU_TOKEN_URL,
             s_tts->config.api_key,
             s_tts->config.secret_key);
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .event_handler = token_http_event_handler,
        .user_data = &response_ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(response_ctx.buffer);
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get token: %s", esp_err_to_name(err));
        free(response_ctx.buffer);
        return err;
    }
    
    ESP_LOGD(TAG, "Token response: %s", response_ctx.buffer);
    
    // 解析 JSON 获取 access_token
    char *token_start = strstr(response_ctx.buffer, "\"access_token\":\"");
    if (token_start == NULL) {
        ESP_LOGE(TAG, "access_token not found in response");
        free(response_ctx.buffer);
        return ESP_FAIL;
    }
    
    token_start += strlen("\"access_token\":\"");
    char *token_end = strchr(token_start, '"');
    if (token_end == NULL) {
        free(response_ctx.buffer);
        return ESP_FAIL;
    }
    
    // 保存 token
    size_t token_len = token_end - token_start;
    if (s_tts->access_token != NULL) {
        free(s_tts->access_token);
    }
    s_tts->access_token = malloc(token_len + 1);
    if (s_tts->access_token == NULL) {
        free(response_ctx.buffer);
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_tts->access_token, token_start, token_len);
    s_tts->access_token[token_len] = '\0';
    
    // 设置过期时间 (默认30天，这里设置为29天)
    s_tts->token_expire_time = esp_timer_get_time() / 1000000 + 29 * 24 * 3600;
    
    ESP_LOGI(TAG, "Got access_token successfully");
    free(response_ctx.buffer);
    return ESP_OK;
}

/**
 * 音频 HTTP 响应上下文
 */
typedef struct {
    uint8_t *buffer;
    size_t buffer_size;
    size_t data_len;
} http_audio_context_t;

/**
 * 音频 HTTP 事件处理器
 */
static esp_err_t http_audio_event_handler(esp_http_client_event_t *evt) {
    http_audio_context_t *ctx = (http_audio_context_t *)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (ctx != NULL && ctx->buffer != NULL) {
                if (ctx->data_len + evt->data_len <= ctx->buffer_size) {
                    memcpy(ctx->buffer + ctx->data_len, evt->data, evt->data_len);
                    ctx->data_len += evt->data_len;
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/**
 * URL 编码
 */
static char *url_encode(const char *str) {
    if (str == NULL) return NULL;
    
    size_t len = strlen(str);
    char *encoded = malloc(len * 3 + 1);
    if (encoded == NULL) return NULL;
    
    char *p = encoded;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = c;
        } else {
            sprintf(p, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return encoded;
}

/**
 * 调用百度 TTS API 获取音频
 * 
 * @param text 要合成的文本
 * @param audio_buffer 音频数据输出缓冲区
 * @param buffer_size 缓冲区大小
 * @param audio_len 输出参数，返回音频数据长度
 * @return ESP_OK 成功
 * 
 * Requirements: 3.1, 3.2
 */
static esp_err_t baidu_tts_synthesize(const char *text, uint8_t *audio_buffer, size_t buffer_size, size_t *audio_len) {
    if (s_tts == NULL || text == NULL || audio_buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 获取 access_token
    esp_err_t ret = get_baidu_access_token();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get access_token");
        return ret;
    }
    
    ESP_LOGI(TAG, "Calling Baidu TTS API: %s", text);
    
    // URL 编码文本
    char *encoded_text = url_encode(text);
    if (encoded_text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    // 构建 POST 数据
    char post_data[1024];
    snprintf(post_data, sizeof(post_data),
             "tex=%s&tok=%s&cuid=esp32_streaming_tts&ctp=1&lan=zh&spd=5&pit=5&vol=10&per=0&aue=4",
             encoded_text, s_tts->access_token);
    free(encoded_text);
    
    // 准备接收缓冲区
    http_audio_context_t ctx = {
        .buffer = audio_buffer,
        .buffer_size = buffer_size,
        .data_len = 0,
    };
    
    esp_http_client_config_t config = {
        .url = BAIDU_TTS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .event_handler = http_audio_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    
    ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TTS request failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }
    
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "TTS request failed, status: %d", status_code);
        return ESP_FAIL;
    }
    
    // 检查是否返回了错误 JSON
    if (ctx.data_len > 0 && ctx.buffer[0] == '{') {
        ESP_LOGE(TAG, "TTS returned error: %.*s", (int)(ctx.data_len > 200 ? 200 : ctx.data_len), ctx.buffer);
        return ESP_FAIL;
    }
    
    // 检查音频数据有效性
    if (ctx.data_len < 100) {
        ESP_LOGE(TAG, "TTS returned data too small: %d bytes", (int)ctx.data_len);
        return ESP_FAIL;
    }
    
    *audio_len = ctx.data_len;
    ESP_LOGI(TAG, "TTS synthesis success, audio size: %d bytes", (int)ctx.data_len);
    return ESP_OK;
}

/**
 * 播放 PCM 音频
 * 
 * @param audio_data PCM 音频数据
 * @param audio_len 音频数据长度
 * @return ESP_OK 成功
 * 
 * Requirements: 3.2
 */
static esp_err_t play_pcm_audio(const uint8_t *audio_data, size_t audio_len) {
    if (s_tts == NULL || s_tts->codec_dev == NULL || audio_data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Playing PCM audio, size: %d bytes", (int)audio_len);
    
    // 使能音频放大器
    if (!s_tts->pa_enabled && s_tts->pca9557_dev != NULL) {
        enable_audio_pa(true);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // 通知播放开始
    if (s_tts->config.on_start) {
        s_tts->config.on_start();
    }
    s_tts->is_playing = true;
    
    // 清空信号量（防止之前的残留信号）
    if (s_tts->play_done_sem != NULL) {
        xSemaphoreTake(s_tts->play_done_sem, 0);
    }
    
    // 设置待播放字节数
    s_tts->pending_bytes = audio_len;
    
    // 分块播放
    const size_t chunk_size = 1024;
    size_t offset = 0;
    
    while (offset < audio_len && !s_tts->should_stop) {
        size_t write_len = (audio_len - offset) > chunk_size ? chunk_size : (audio_len - offset);
        esp_err_t ret = esp_codec_dev_write(s_tts->codec_dev, (void *)(audio_data + offset), write_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to write audio data");
            break;
        }
        offset += write_len;
    }
    
    // 等待播放完成（通过 I2S 回调信号量）
    if (!s_tts->should_stop && offset > 0 && s_tts->play_done_sem != NULL) {
        // 计算最大等待时间：音频时长 + 500ms 余量
        uint32_t max_wait_ms = (audio_len * 1000) / (SAMPLE_RATE * 2) + 500;
        ESP_LOGD(TAG, "Waiting for playback completion (max %lu ms)", (unsigned long)max_wait_ms);
        
        if (xSemaphoreTake(s_tts->play_done_sem, pdMS_TO_TICKS(max_wait_ms)) != pdTRUE) {
            ESP_LOGW(TAG, "Playback wait timeout, pending_bytes=%d", (int)s_tts->pending_bytes);
        }
    }
    
    s_tts->is_playing = false;
    s_tts->pending_bytes = 0;
    
    // 通知播放结束
    if (s_tts->config.on_stop) {
        s_tts->config.on_stop();
    }
    
    return ESP_OK;
}

// ============================================================================
// TTS 播放任务
// ============================================================================

/**
 * TTS 播放任务
 * 
 * 从分句队列读取句子，调用百度 TTS API 获取音频并播放。
 * 
 * Requirements: 3.1, 3.2, 3.3, 3.4
 */
static void player_task(void *arg) {
    ESP_LOGI(TAG, "Player task started");
    
    char sentence[SENTENCE_MAX_LEN];
    
    // 分配音频缓冲区
    s_tts->audio_buffer = heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tts->audio_buffer == NULL) {
        s_tts->audio_buffer = malloc(AUDIO_BUFFER_SIZE);
    }
    if (s_tts->audio_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        vTaskDelete(NULL);
        return;
    }
    s_tts->audio_buffer_size = AUDIO_BUFFER_SIZE;
    
    while (!s_tts->should_stop) {
        // 从分句队列读取 (Requirements 3.1)
        if (xQueueReceive(s_tts->sentence_queue, sentence, pdMS_TO_TICKS(QUEUE_RECV_TIMEOUT_MS)) == pdTRUE) {
            ESP_LOGI(TAG, "Processing sentence: %s", sentence);
            
            // 检查是否应该停止
            if (s_tts->should_stop) {
                break;
            }
            
            // 调用百度 TTS API 获取音频 (Requirements 3.1)
            size_t audio_len = 0;
            esp_err_t ret = baidu_tts_synthesize(sentence, s_tts->audio_buffer, s_tts->audio_buffer_size, &audio_len);
            
            if (ret != ESP_OK) {
                // 记录日志，跳过当前句子，继续下一句 (Error Handling)
                ESP_LOGW(TAG, "TTS synthesis failed for: %s, skipping", sentence);
                continue;
            }
            
            // 播放音频 (Requirements 3.2)
            ret = play_pcm_audio(s_tts->audio_buffer, audio_len);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Audio playback failed, continuing");
            }
            
            // 当前句子播放完成，继续处理下一个句子 (Requirements 3.3)
            ESP_LOGD(TAG, "Sentence playback completed");
        }
        // 分句队列为空且文本流未结束时，等待新句子 (Requirements 3.4)
    }
    
    ESP_LOGI(TAG, "Player task stopped");
    vTaskDelete(NULL);
}

// ============================================================================
// 公共 API 实现
// ============================================================================

esp_err_t streaming_tts_init(const streaming_tts_config_t *config) {
    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_tts != NULL) {
        ESP_LOGW(TAG, "Streaming TTS already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing streaming TTS service...");
    
    // 分配内部状态结构体
    s_tts = calloc(1, sizeof(streaming_tts_t));
    if (s_tts == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for streaming TTS");
        return ESP_ERR_NO_MEM;
    }
    
    // 复制配置
    s_tts->config = *config;
    
    // 复制 API 密钥 (深拷贝)
    if (config->api_key != NULL) {
        s_tts->config.api_key = strdup(config->api_key);
        if (s_tts->config.api_key == NULL) {
            free(s_tts);
            s_tts = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (config->secret_key != NULL) {
        s_tts->config.secret_key = strdup(config->secret_key);
        if (s_tts->config.secret_key == NULL) {
            free((void *)s_tts->config.api_key);
            free(s_tts);
            s_tts = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    
    // 设置默认 I2S 引脚 (立创实战派 ESP32-S3 默认值)
    if (s_tts->config.i2s_mclk_pin == 0) s_tts->config.i2s_mclk_pin = 38;
    if (s_tts->config.i2s_bclk_pin == 0) s_tts->config.i2s_bclk_pin = 14;
    if (s_tts->config.i2s_ws_pin == 0) s_tts->config.i2s_ws_pin = 13;
    if (s_tts->config.i2s_dout_pin == 0) s_tts->config.i2s_dout_pin = 45;
    
    // 初始化状态
    s_tts->stream_ended = false;
    s_tts->is_playing = false;
    s_tts->should_stop = false;
    s_tts->buffer_pos = 0;
    memset(s_tts->sentence_buffer, 0, SENTENCE_BUFFER_SIZE);
    
    // 创建原始文本队列
    s_tts->raw_text_queue = xQueueCreate(RAW_TEXT_QUEUE_SIZE, RAW_TEXT_MAX_LEN);
    if (s_tts->raw_text_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create raw text queue");
        goto cleanup;
    }
    ESP_LOGI(TAG, "Raw text queue created (size: %d, item: %d bytes)", 
             RAW_TEXT_QUEUE_SIZE, RAW_TEXT_MAX_LEN);
    
    // 创建分句队列
    s_tts->sentence_queue = xQueueCreate(SENTENCE_QUEUE_SIZE, SENTENCE_MAX_LEN);
    if (s_tts->sentence_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create sentence queue");
        goto cleanup;
    }
    ESP_LOGI(TAG, "Sentence queue created (size: %d, item: %d bytes)", 
             SENTENCE_QUEUE_SIZE, SENTENCE_MAX_LEN);
    
    // 创建播放完成信号量
    s_tts->play_done_sem = xSemaphoreCreateBinary();
    if (s_tts->play_done_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create play done semaphore");
        goto cleanup;
    }
    ESP_LOGI(TAG, "Play done semaphore created");
    
    // 初始化 I2C 设备
    esp_err_t ret = init_i2c_devices((i2c_master_bus_handle_t)s_tts->config.i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C devices init failed, continuing without PA control");
    }
    
    // 初始化 ES8311 编解码器
    ret = init_es8311_codec();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ES8311 codec");
        goto cleanup;
    }
    
    // 使能音频放大器
    if (s_tts->pca9557_dev != NULL) {
        enable_audio_pa(true);
    }
    
    // 创建分句任务
    BaseType_t task_ret = xTaskCreate(
        splitter_task,
        "tts_splitter",
        4096,
        NULL,
        5,
        &s_tts->splitter_task
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create splitter task");
        goto cleanup;
    }
    
    // 创建 TTS 播放任务
    task_ret = xTaskCreate(
        player_task,
        "tts_player",
        8192,
        NULL,
        5,
        &s_tts->player_task
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create player task");
        goto cleanup;
    }
    
    s_tts->initialized = true;
    ESP_LOGI(TAG, "Streaming TTS service initialized successfully");
    return ESP_OK;

cleanup:
    // 清理已分配的资源
    if (s_tts->splitter_task != NULL) {
        s_tts->should_stop = true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (s_tts->raw_text_queue != NULL) {
        vQueueDelete(s_tts->raw_text_queue);
    }
    if (s_tts->sentence_queue != NULL) {
        vQueueDelete(s_tts->sentence_queue);
    }
    if (s_tts->codec_dev != NULL) {
        esp_codec_dev_close(s_tts->codec_dev);
        esp_codec_dev_delete(s_tts->codec_dev);
    }
    if (s_tts->i2s_tx_handle != NULL) {
        i2s_del_channel(s_tts->i2s_tx_handle);
    }
    if (s_tts->config.api_key != NULL) {
        free((void *)s_tts->config.api_key);
    }
    if (s_tts->config.secret_key != NULL) {
        free((void *)s_tts->config.secret_key);
    }
    free(s_tts);
    s_tts = NULL;
    return ESP_FAIL;
}


/**
 * 推送文本到流式 TTS 处理流程
 * 
 * 将 SSE 接收到的文本追加到原始文本队列，由分句器异步处理。
 * 如果队列已满，会阻塞等待直到队列有空间（最多 5 秒超时）。
 * 
 * Requirements: 1.1, 1.2, 5.2
 */
esp_err_t streaming_tts_push_text(const char *text) {
    // 检查服务是否已初始化
    if (s_tts == NULL || !s_tts->initialized) {
        ESP_LOGW(TAG, "Streaming TTS not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 忽略空文本 (Requirements 1.1 - 只处理有效文本)
    if (text == NULL || strlen(text) == 0) {
        return ESP_OK;
    }
    
    // 重置流结束标志（有新数据进来表示流还在继续）
    s_tts->stream_ended = false;
    
    // 准备文本缓冲区
    char text_buf[RAW_TEXT_MAX_LEN];
    size_t text_len = strlen(text);
    
    // 如果文本超过最大长度，需要分段发送
    size_t offset = 0;
    while (offset < text_len) {
        size_t chunk_len = text_len - offset;
        if (chunk_len > RAW_TEXT_MAX_LEN - 1) {
            chunk_len = RAW_TEXT_MAX_LEN - 1;
        }
        
        memcpy(text_buf, text + offset, chunk_len);
        text_buf[chunk_len] = '\0';
        
        // 发送到原始文本队列 (Requirements 1.1)
        // 如果队列已满，阻塞等待直到队列有空间 (Requirements 1.2)
        if (xQueueSend(s_tts->raw_text_queue, text_buf, pdMS_TO_TICKS(QUEUE_SEND_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Raw text queue full, timeout after %d ms", QUEUE_SEND_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        
        ESP_LOGD(TAG, "Text pushed to queue (%zu bytes): %s", chunk_len, text_buf);
        offset += chunk_len;
    }
    
    return ESP_OK;
}

/**
 * 标记文本流结束
 * 
 * 当 SSE 连接断开时调用，触发分句器处理剩余文本。
 * 分句器会将内部缓冲区中所有剩余文本作为最后一句加入分句队列。
 * 
 * Requirements: 1.3, 2.3
 */
esp_err_t streaming_tts_end_stream(void) {
    // 检查服务是否已初始化
    if (s_tts == NULL || !s_tts->initialized) {
        ESP_LOGW(TAG, "Streaming TTS not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 标记流结束 (Requirements 1.3)
    // 分句任务会检测此标志并处理剩余文本 (Requirements 2.3)
    s_tts->stream_ended = true;
    
    ESP_LOGI(TAG, "Stream ended, splitter will flush remaining text");
    return ESP_OK;
}

/**
 * 停止播放并清空所有队列
 * 
 * 立即停止当前播放，清空原始文本队列和分句队列，
 * 重置内部状态以便接收新的文本流。
 * 
 * Requirements: 4.1
 */
esp_err_t streaming_tts_stop(void) {
    // 检查服务是否已初始化
    if (s_tts == NULL || !s_tts->initialized) {
        ESP_LOGW(TAG, "Streaming TTS not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Stopping streaming TTS...");
    
    // 设置停止标志，通知播放任务停止当前播放
    // 注意：这里不设置 should_stop，因为那会导致任务退出
    // 我们只是想停止当前播放，而不是销毁服务
    
    // 清空原始文本队列 (Requirements 4.1)
    if (s_tts->raw_text_queue != NULL) {
        xQueueReset(s_tts->raw_text_queue);
        ESP_LOGD(TAG, "Raw text queue cleared");
    }
    
    // 清空分句队列 (Requirements 4.1)
    if (s_tts->sentence_queue != NULL) {
        xQueueReset(s_tts->sentence_queue);
        ESP_LOGD(TAG, "Sentence queue cleared");
    }
    
    // 重置流状态，准备接收新的文本流 (Requirements 4.2)
    s_tts->stream_ended = false;
    
    // 清空分句缓冲区
    s_tts->buffer_pos = 0;
    memset(s_tts->sentence_buffer, 0, SENTENCE_BUFFER_SIZE);
    
    // 通知播放停止回调
    if (s_tts->is_playing && s_tts->config.on_stop) {
        s_tts->config.on_stop();
    }
    s_tts->is_playing = false;
    
    ESP_LOGI(TAG, "Streaming TTS stopped, ready for new stream");
    return ESP_OK;
}

/**
 * 查询是否正在播放
 * 
 * 返回当前 TTS 播放状态。当有音频正在播放时返回 true，
 * 否则返回 false（包括服务未初始化的情况）。
 * 
 * Requirements: 4.3
 */
bool streaming_tts_is_playing(void) {
    // 服务未初始化时返回 false
    if (s_tts == NULL || !s_tts->initialized) {
        return false;
    }
    
    // 返回当前播放状态 (Requirements 4.3)
    return s_tts->is_playing;
}

/**
 * 销毁流式 TTS 服务
 * 
 * 停止所有任务，释放所有资源。调用此函数后，
 * 需要重新调用 streaming_tts_init 才能再次使用服务。
 * 
 * Requirements: 5.4
 */
void streaming_tts_destroy(void) {
    // 如果服务未初始化，直接返回
    if (s_tts == NULL) {
        return;
    }
    
    ESP_LOGI(TAG, "Destroying streaming TTS service...");
    
    // 标记服务为未初始化，防止其他函数继续使用
    s_tts->initialized = false;
    
    // 设置停止标志，通知所有任务退出 (Requirements 5.4)
    s_tts->should_stop = true;
    
    // 等待任务退出
    // 给任务足够的时间来检测停止标志并清理
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // 禁用音频放大器
    if (s_tts->pa_enabled && s_tts->pca9557_dev != NULL) {
        enable_audio_pa(false);
    }
    
    // 删除队列 (Requirements 5.4 - 释放所有资源)
    if (s_tts->raw_text_queue != NULL) {
        vQueueDelete(s_tts->raw_text_queue);
        s_tts->raw_text_queue = NULL;
        ESP_LOGD(TAG, "Raw text queue deleted");
    }
    if (s_tts->sentence_queue != NULL) {
        vQueueDelete(s_tts->sentence_queue);
        s_tts->sentence_queue = NULL;
        ESP_LOGD(TAG, "Sentence queue deleted");
    }
    
    // 删除播放完成信号量
    if (s_tts->play_done_sem != NULL) {
        vSemaphoreDelete(s_tts->play_done_sem);
        s_tts->play_done_sem = NULL;
        ESP_LOGD(TAG, "Play done semaphore deleted");
    }
    
    // 关闭并删除编解码器设备
    if (s_tts->codec_dev != NULL) {
        esp_codec_dev_close(s_tts->codec_dev);
        esp_codec_dev_delete(s_tts->codec_dev);
        s_tts->codec_dev = NULL;
        ESP_LOGD(TAG, "Codec device closed and deleted");
    }
    
    // 删除 I2S 通道
    if (s_tts->i2s_tx_handle != NULL) {
        i2s_del_channel(s_tts->i2s_tx_handle);
        s_tts->i2s_tx_handle = NULL;
        ESP_LOGD(TAG, "I2S channel deleted");
    }
    
    // 释放音频缓冲区
    if (s_tts->audio_buffer != NULL) {
        free(s_tts->audio_buffer);
        s_tts->audio_buffer = NULL;
        ESP_LOGD(TAG, "Audio buffer freed");
    }
    
    // 释放 access_token
    if (s_tts->access_token != NULL) {
        free(s_tts->access_token);
        s_tts->access_token = NULL;
    }
    
    // 释放配置中的字符串（深拷贝的副本）
    if (s_tts->config.api_key != NULL) {
        free((void *)s_tts->config.api_key);
    }
    if (s_tts->config.secret_key != NULL) {
        free((void *)s_tts->config.secret_key);
    }
    
    // 释放主结构体
    free(s_tts);
    s_tts = NULL;
    
    ESP_LOGI(TAG, "Streaming TTS service destroyed successfully");
}
