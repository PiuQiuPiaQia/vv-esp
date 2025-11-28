/**
 * 本地 TTS 语音合成服务
 * 使用 ESP-IDF 的 esp_tts_chinese 组件实现离线中文语音合成
 */

#ifndef TTS_SERVICE_H
#define TTS_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * TTS 事件类型
 */
typedef enum {
    TTS_EVENT_START,        // 开始播放
    TTS_EVENT_PLAYING,      // 正在播放
    TTS_EVENT_STOP,         // 停止播放
    TTS_EVENT_ERROR,        // 错误
} tts_event_type_t;

/**
 * TTS 事件回调
 */
typedef void (*tts_event_callback_t)(tts_event_type_t event, void *user_data);

/**
 * TTS 配置 (本地 TTS)
 */
typedef struct {
    int sample_rate;            // 采样率 (默认 16000)
    int speed;                  // 语速 0-5, 默认 1 (正常)
    tts_event_callback_t callback;
    void *user_data;
    
    // I2S 音频输出配置 (立创实战派 ESP32-S3 默认值)
    int i2s_mclk_pin;           // I2S MCLK 引脚 (默认 38)
    int i2s_bclk_pin;           // I2S BCLK 引脚 (默认 14)
    int i2s_ws_pin;             // I2S WS/LRCK 引脚 (默认 13)
    int i2s_dout_pin;           // I2S DOUT 引脚 (默认 45)
    
    // I2C 总线句柄 (可选，用于控制 PCA9557 音频放大器和 ES8311 编解码器)
    void *i2c_bus_handle;       // i2c_master_bus_handle_t, 如果为 NULL 则尝试自己创建
} tts_config_t;

/**
 * 初始化本地 TTS 服务
 * @param config TTS 配置
 * @return ESP_OK 成功
 */
esp_err_t tts_service_init(const tts_config_t *config);

/**
 * 文本转语音并播放 (同步)
 * @param text 要播放的文本 (支持中文)
 * @return ESP_OK 成功
 */
esp_err_t tts_speak(const char *text);

/**
 * 将文本添加到播放队列 (异步)
 * @param text 要播放的文本
 * @return ESP_OK 成功
 */
esp_err_t tts_speak_async(const char *text);

/**
 * 停止当前播放
 * @return ESP_OK 成功
 */
esp_err_t tts_stop(void);

/**
 * 检查是否正在播放
 * @return true 正在播放
 */
bool tts_is_playing(void);

/**
 * 销毁 TTS 服务
 */
void tts_service_destroy(void);

#ifdef __cplusplus
}
#endif

#endif // TTS_SERVICE_H
