/**
 * 百度智能体客户端内部类型定义
 */

#ifndef BAIDU_AGENT_TYPES_H
#define BAIDU_AGENT_TYPES_H

#include "baidu_agent_client.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

// SSE 数据缓冲区大小
#define SSE_BUFFER_SIZE 4096
#define SSE_LINE_BUFFER_SIZE 2048

/**
 * 客户端内部状态
 */
typedef struct {
    baidu_agent_config_t config;
    esp_http_client_handle_t http_client;
    TaskHandle_t task_handle;
    SemaphoreHandle_t mutex;
    bool is_connected;
    bool should_stop;
    char *sse_buffer;
    size_t sse_buffer_pos;
    int retry_count;
    char *thread_id;  // 动态存储的会话ID
    char *post_data;  // POST请求数据，需要在请求完成后释放
} baidu_agent_client_t;

#ifdef __cplusplus
}
#endif

#endif // BAIDU_AGENT_TYPES_H
