/**
 * 百度在线 TTS 语音合成服务实现
 * 使用百度语音合成 API 将文本转换为语音
 */

#include "tts_service.h"
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
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "BAIDU_TTS";

// 百度 TTS API 配置
#define BAIDU_TTS_API_KEY    "your_api_key"      // 需要替换为你的 API Key
#define BAIDU_TTS_SECRET_KEY "your_secret_key"   // 需要替换为你的 Secret Key
#define BAIDU_TOKEN_URL      "https://aip.baidubce.com/oauth/2.0/token"
#define BAIDU_TTS_URL        "https://tsn.baidu.com/text2audio"

// ES8311 音频编解码器地址
#define ES8311_ADDR 0x30

// PCA9557 IO 扩展芯片
#define PCA9557_ADDR 0x19
#define PCA9557_REG_OUTPUT 0x01

#define TTS_TEXT_QUEUE_SIZE 20  // 增加队列大小以容纳更多文本片段
#define TTS_MAX_TEXT_LEN 512
#define SAMPLE_RATE 16000
#define AUDIO_BUFFER_SIZE (50 * 1024)  // 50KB 音频缓冲区，约 1.5 秒音频

typedef struct {
    tts_config_t config;
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

// 使能/禁用音频放大器
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

// 初始化 I2C 设备
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
    }
    
    return ESP_OK;
}

// 初始化 ES8311 编解码器
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


// Token 响应缓冲区
typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t data_len;
} token_response_t;

static esp_err_t token_http_event_handler(esp_http_client_event_t *evt) {
    token_response_t *ctx = (token_response_t *)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // 接收所有数据，包括 chunked 响应
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

// 获取百度 access_token
static esp_err_t get_baidu_access_token(void) {
    if (s_tts == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // 检查 token 是否有效
    if (s_tts->access_token != NULL && s_tts->token_expire_time > esp_timer_get_time() / 1000000) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "获取百度 access_token...");
    
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
             s_tts->config.api_key ? s_tts->config.api_key : BAIDU_TTS_API_KEY,
             s_tts->config.secret_key ? s_tts->config.secret_key : BAIDU_TTS_SECRET_KEY);
    
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
        ESP_LOGE(TAG, "获取 token 失败: %s", esp_err_to_name(err));
        free(response_ctx.buffer);
        return err;
    }
    
    ESP_LOGI(TAG, "Token 响应: %s", response_ctx.buffer);
    
    // 解析 JSON 获取 access_token
    char *token_start = strstr(response_ctx.buffer, "\"access_token\":\"");
    if (token_start == NULL) {
        ESP_LOGE(TAG, "响应中未找到 access_token");
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
    
    ESP_LOGI(TAG, "获取 access_token 成功");
    free(response_ctx.buffer);
    return ESP_OK;
}

// HTTP 事件处理器 - 边接收边播放音频数据
typedef struct {
    esp_codec_dev_handle_t codec_dev;
    size_t total_len;
    bool first_chunk;
    bool is_error;
} http_streaming_audio_context_t;

static esp_err_t http_streaming_audio_event_handler(esp_http_client_event_t *evt) {
    http_streaming_audio_context_t *ctx = (http_streaming_audio_context_t *)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (ctx != NULL && ctx->codec_dev != NULL && !ctx->is_error) {
                // 检查是否是错误响应（JSON 格式）
                if (ctx->first_chunk && evt->data_len > 0) {
                    ctx->first_chunk = false;
                    if (((char *)evt->data)[0] == '{') {
                        ESP_LOGE(TAG, "TTS 返回错误: %.*s", (int)evt->data_len, (char *)evt->data);
                        ctx->is_error = true;
                        return ESP_OK;
                    }
                }
                
                // 直接播放收到的音频数据
                esp_err_t ret = esp_codec_dev_write(ctx->codec_dev, evt->data, evt->data_len);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "写入音频数据失败");
                }
                ctx->total_len += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// URL 编码
static char *url_encode(const char *str) {
    if (str == NULL) return NULL;
    
    size_t len = strlen(str);
    // 最坏情况：每个字符都需要编码为 %XX (3倍)
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


// 百度 TTS API 单次请求最大文本长度（UTF-8 字节数）
// 百度官方限制是 2048 字节
#define BAIDU_TTS_MAX_TEXT_LEN 2048

// 调用百度 TTS API 并流式播放音频
static esp_err_t baidu_tts_synthesize(const char *text, uint8_t *audio_buffer, size_t buffer_size, size_t *audio_len) {
    (void)audio_buffer;  // 不再使用，保留参数兼容性
    (void)buffer_size;
    
    if (s_tts == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 获取 access_token
    esp_err_t ret = get_baidu_access_token();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取 access_token 失败");
        return ret;
    }
    
    // 限制文本长度，避免超出百度 API 限制
    size_t text_len = strlen(text);
    char *truncated_text = NULL;
    const char *tts_text = text;
    
    if (text_len > BAIDU_TTS_MAX_TEXT_LEN) {
        ESP_LOGW(TAG, "文本过长 (%d 字节)，截断到 %d 字节", (int)text_len, BAIDU_TTS_MAX_TEXT_LEN);
        truncated_text = malloc(BAIDU_TTS_MAX_TEXT_LEN + 1);
        if (truncated_text == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(truncated_text, text, BAIDU_TTS_MAX_TEXT_LEN);
        truncated_text[BAIDU_TTS_MAX_TEXT_LEN] = '\0';
        tts_text = truncated_text;
    }
    
    ESP_LOGI(TAG, "调用百度 TTS API: %s", tts_text);
    
    // URL 编码文本
    char *encoded_text = url_encode(tts_text);
    if (truncated_text != NULL) {
        free(truncated_text);
    }
    if (encoded_text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    // 动态分配 POST 数据缓冲区
    // URL 编码后最大长度 = 原文本长度 * 3 + token(~200) + 其他参数(~100)
    size_t encoded_len = strlen(encoded_text);
    size_t post_data_size = encoded_len + 512;
    char *post_data = malloc(post_data_size);
    if (post_data == NULL) {
        free(encoded_text);
        return ESP_ERR_NO_MEM;
    }
    
    // 构建 POST 数据
    // 参数说明:
    // tex: 文本
    // tok: access_token
    // cuid: 用户唯一标识
    // ctp: 客户端类型 1=web
    // lan: 语言 zh=中文
    // spd: 语速 0-15, 默认5
    // pit: 音调 0-15, 默认5
    // vol: 音量 0-15, 默认5
    // per: 发音人 0=女声, 1=男声, 3=情感男声, 4=情感女声
    // aue: 音频格式 3=mp3, 4=pcm-16k, 5=pcm-8k, 6=wav
    snprintf(post_data, post_data_size,
             "tex=%s&tok=%s&cuid=esp32_tts&ctp=1&lan=zh&spd=5&pit=5&vol=10&per=0&aue=4",
             encoded_text, s_tts->access_token);
    free(encoded_text);
    
    // 使用流式播放上下文，边下载边播放
    http_streaming_audio_context_t ctx = {
        .codec_dev = s_tts->codec_dev,
        .total_len = 0,
        .first_chunk = true,
        .is_error = false,
    };
    
    esp_http_client_config_t config = {
        .url = BAIDU_TTS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .event_handler = http_streaming_audio_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(post_data);
        return ESP_FAIL;
    }
    
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    
    ret = esp_http_client_perform(client);
    free(post_data);
    post_data = NULL;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TTS 请求失败: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }
    
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "TTS 请求失败，状态码: %d", status_code);
        return ESP_FAIL;
    }
    
    if (ctx.is_error) {
        return ESP_FAIL;
    }
    
    if (ctx.total_len < 100) {
        ESP_LOGE(TAG, "TTS 返回数据太小: %d bytes", (int)ctx.total_len);
        return ESP_FAIL;
    }
    
    *audio_len = ctx.total_len;
    ESP_LOGI(TAG, "TTS 流式播放完成，总音频大小: %d bytes", (int)ctx.total_len);
    return ESP_OK;
}

// 播放 PCM 音频
static esp_err_t play_pcm_audio(const uint8_t *audio_data, size_t audio_len) {
    if (s_tts == NULL || s_tts->codec_dev == NULL || audio_data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "播放 PCM 音频，大小: %d bytes", (int)audio_len);
    
    // 使能音频放大器
    if (!s_tts->pa_enabled && s_tts->pca9557_dev != NULL) {
        enable_audio_pa(true);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // 通知播放开始
    if (s_tts->config.callback) {
        s_tts->config.callback(TTS_EVENT_START, s_tts->config.user_data);
    }
    s_tts->is_playing = true;
    
    // 写入音频数据到 DMA 缓冲区
    esp_err_t ret = esp_codec_dev_write(s_tts->codec_dev, (void *)audio_data, audio_len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "写入音频数据失败: %s", esp_err_to_name(ret));
    }
    
    // 注意：esp_codec_dev_write 返回后，DMA 仍在后台播放音频
    // 这里不做任何等待，让音频自然播放完成
    
    s_tts->is_playing = false;
    
    // 通知播放结束（实际上音频可能还在播放）
    if (s_tts->config.callback) {
        s_tts->config.callback(TTS_EVENT_STOP, s_tts->config.user_data);
    }
    
    return ret;
}

// 播放文本（合成并播放）
static esp_err_t tts_play_text(const char *text) {
    if (s_tts == NULL || text == NULL || strlen(text) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 过滤太短的文本（少于2个字符可能导致 TTS 问题）
    size_t text_len = strlen(text);
    if (text_len < 2) {
        ESP_LOGW(TAG, "文本太短，跳过 TTS: %s", text);
        return ESP_OK;
    }
    
    // 使能音频放大器
    if (!s_tts->pa_enabled && s_tts->pca9557_dev != NULL) {
        enable_audio_pa(true);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // 通知播放开始
    if (s_tts->config.callback) {
        s_tts->config.callback(TTS_EVENT_START, s_tts->config.user_data);
    }
    s_tts->is_playing = true;
    
    // 调用百度 TTS（边下载边播放）
    size_t audio_len = 0;
    esp_err_t ret = baidu_tts_synthesize(text, NULL, 0, &audio_len);
    
    s_tts->is_playing = false;
    
    // 通知播放结束
    if (s_tts->config.callback) {
        s_tts->config.callback(TTS_EVENT_STOP, s_tts->config.user_data);
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TTS 合成/播放失败");
    }
    
    return ret;
}

// TTS 任务
static void tts_task(void *arg) {
    char text[TTS_MAX_TEXT_LEN];
    
    while (!s_tts->should_stop) {
        if (xQueueReceive(s_tts->text_queue, text, pdMS_TO_TICKS(100)) == pdTRUE) {
            tts_play_text(text);
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
    
    // 复制 API 密钥
    if (config->api_key != NULL) {
        s_tts->config.api_key = strdup(config->api_key);
    }
    if (config->secret_key != NULL) {
        s_tts->config.secret_key = strdup(config->secret_key);
    }
    
    // 设置默认值
    if (s_tts->config.sample_rate == 0) s_tts->config.sample_rate = SAMPLE_RATE;
    if (s_tts->config.speed == 0) s_tts->config.speed = 5;
    
    // 默认 I2S 引脚
    if (s_tts->config.i2s_mclk_pin == 0) s_tts->config.i2s_mclk_pin = 38;
    if (s_tts->config.i2s_bclk_pin == 0) s_tts->config.i2s_bclk_pin = 14;
    if (s_tts->config.i2s_ws_pin == 0) s_tts->config.i2s_ws_pin = 13;
    if (s_tts->config.i2s_dout_pin == 0) s_tts->config.i2s_dout_pin = 45;
    
    ESP_LOGI(TAG, "Initializing Baidu TTS service...");
    
    // 初始化 I2C 设备
    esp_err_t ret = init_i2c_devices((i2c_master_bus_handle_t)s_tts->config.i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C devices init failed");
    }
    
    // 初始化 ES8311 编解码器
    ret = init_es8311_codec();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ES8311 codec");
        free(s_tts);
        s_tts = NULL;
        return ret;
    }
    
    // 使能音频放大器
    if (s_tts->pca9557_dev != NULL) {
        enable_audio_pa(true);
    }
    
    // 注意：不再需要预分配音频缓冲区，因为现在是边下载边播放
    
    // 创建文本队列
    s_tts->text_queue = xQueueCreate(TTS_TEXT_QUEUE_SIZE, TTS_MAX_TEXT_LEN);
    if (s_tts->text_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create text queue");
        free(s_tts);
        s_tts = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    // 创建 TTS 任务
    BaseType_t task_ret = xTaskCreate(tts_task, "baidu_tts", 8192, NULL, 5, &s_tts->task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TTS task");
        vQueueDelete(s_tts->text_queue);
        free(s_tts);
        s_tts = NULL;
        return ESP_FAIL;
    }
    
    s_tts->initialized = true;
    ESP_LOGI(TAG, "Baidu TTS service initialized");
    return ESP_OK;
}

// 文本转语音并播放 (同步)
esp_err_t tts_speak(const char *text) {
    if (s_tts == NULL || text == NULL || !s_tts->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return tts_play_text(text);
}

// 将文本添加到播放队列 (异步)
esp_err_t tts_speak_async(const char *text) {
    if (s_tts == NULL || text == NULL || !s_tts->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // 过滤空文本或太短的文本
    size_t text_len = strlen(text);
    if (text_len < 2) {
        ESP_LOGD(TAG, "文本太短，跳过: %s", text);
        return ESP_OK;
    }
    
    char text_buf[TTS_MAX_TEXT_LEN];
    strncpy(text_buf, text, TTS_MAX_TEXT_LEN - 1);
    text_buf[TTS_MAX_TEXT_LEN - 1] = '\0';
    
    // 检查队列剩余空间
    UBaseType_t spaces = uxQueueSpacesAvailable(s_tts->text_queue);
    if (spaces == 0) {
        ESP_LOGW(TAG, "TTS 队列已满，等待空间...");
        // 等待更长时间让队列有空间
        if (xQueueSend(s_tts->text_queue, text_buf, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "TTS 队列超时，丢弃文本: %s", text_buf);
            return ESP_ERR_TIMEOUT;
        }
    } else {
        if (xQueueSend(s_tts->text_queue, text_buf, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "TTS 队列发送失败");
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "TTS 文本已加入队列 (剩余空间: %d): %s", (int)spaces, text_buf);
    return ESP_OK;
}

// 停止当前播放并清空队列
esp_err_t tts_stop(void) {
    if (s_tts == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // 只清空队列，不设置 should_stop 标志
    // should_stop 标志仅用于销毁服务时停止任务
    xQueueReset(s_tts->text_queue);
    ESP_LOGI(TAG, "TTS 队列已清空");
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
    
    if (s_tts->codec_dev != NULL) {
        esp_codec_dev_close(s_tts->codec_dev);
        esp_codec_dev_delete(s_tts->codec_dev);
    }
    
    if (s_tts->i2s_tx_handle != NULL) {
        i2s_del_channel(s_tts->i2s_tx_handle);
    }
    
    if (s_tts->audio_buffer != NULL) {
        free(s_tts->audio_buffer);
    }
    
    if (s_tts->access_token != NULL) {
        free(s_tts->access_token);
    }
    
    if (s_tts->config.api_key != NULL) {
        free((void *)s_tts->config.api_key);
    }
    if (s_tts->config.secret_key != NULL) {
        free((void *)s_tts->config.secret_key);
    }
    
    free(s_tts);
    s_tts = NULL;
    
    ESP_LOGI(TAG, "Baidu TTS service destroyed");
}
