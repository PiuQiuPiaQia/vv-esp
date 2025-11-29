/**
 * 流式 TTS 语音合成服务
 * 
 * 从 SSE 流接收文本，按标点符号分句，通过百度 TTS API 进行流式语音合成和播放。
 * 采用生产者-消费者模式，通过两级队列实现低延迟的流式播报：
 * 
 * SSE 文本 → [原始文本队列] → 分句器 → [分句队列] → TTS 播放器 → 音频输出
 */

#ifndef STREAMING_TTS_H
#define STREAMING_TTS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 流式 TTS 事件回调类型
 */
typedef void (*streaming_tts_callback_t)(void);

/**
 * 流式 TTS 配置结构体
 * 
 * Requirements: 5.1 - 初始化流式 TTS 时接受百度 API 密钥和音频硬件配置
 */
typedef struct {
    // 百度 TTS API 配置
    const char *api_key;        ///< 百度 API Key
    const char *secret_key;     ///< 百度 Secret Key
    
    // I2S 音频输出引脚配置
    int i2s_mclk_pin;           ///< I2S MCLK 引脚
    int i2s_bclk_pin;           ///< I2S BCLK 引脚
    int i2s_ws_pin;             ///< I2S WS/LRCK 引脚
    int i2s_dout_pin;           ///< I2S DOUT 引脚
    
    // I2C 总线句柄 (用于控制音频编解码器和放大器)
    void *i2c_bus_handle;       ///< i2c_master_bus_handle_t
    
    // 事件回调 (可选)
    streaming_tts_callback_t on_start;  ///< 开始播放回调
    streaming_tts_callback_t on_stop;   ///< 停止播放回调
} streaming_tts_config_t;

/**
 * 初始化流式 TTS 服务
 * 
 * 创建原始文本队列、分句队列，启动分句任务和 TTS 播放任务。
 * 
 * @param config 流式 TTS 配置
 * @return ESP_OK 成功，其他值表示错误
 * 
 * Requirements: 5.1
 */
esp_err_t streaming_tts_init(const streaming_tts_config_t *config);

/**
 * 推送文本到流式 TTS 处理流程
 * 
 * 将 SSE 接收到的文本追加到原始文本队列，由分句器异步处理。
 * 如果队列已满，会阻塞等待直到队列有空间。
 * 
 * @param text 要推送的文本
 * @return ESP_OK 成功，ESP_ERR_TIMEOUT 队列满超时
 * 
 * Requirements: 1.1, 1.2, 5.2
 */
esp_err_t streaming_tts_push_text(const char *text);

/**
 * 标记文本流结束
 * 
 * 当 SSE 连接断开时调用，触发分句器处理剩余文本。
 * 
 * @return ESP_OK 成功
 * 
 * Requirements: 1.3, 2.3
 */
esp_err_t streaming_tts_end_stream(void);

/**
 * 停止播放并清空所有队列
 * 
 * 立即停止当前播放，清空原始文本队列和分句队列。
 * 
 * @return ESP_OK 成功
 * 
 * Requirements: 4.1, 5.3
 */
esp_err_t streaming_tts_stop(void);

/**
 * 查询是否正在播放
 * 
 * @return true 正在播放，false 未播放
 * 
 * Requirements: 4.3
 */
bool streaming_tts_is_playing(void);

/**
 * 销毁流式 TTS 服务
 * 
 * 停止所有任务，释放所有资源。
 * 
 * Requirements: 5.4
 */
void streaming_tts_destroy(void);

#ifdef __cplusplus
}
#endif

#endif // STREAMING_TTS_H
