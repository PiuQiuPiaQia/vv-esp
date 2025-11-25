/**
 * 百度智能体 Conversation API 客户端实现
 * 基于 SSE (Server-Sent Events) 协议
 */

#include "baidu_agent_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "BAIDU_AGENT";

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
} baidu_agent_client_t;

/**
 * SSE 事件回调 - 处理 HTTP 响应数据
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    baidu_agent_client_t *client = (baidu_agent_client_t *)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "已连接到服务器");
            if (client->config.callback) {
                client->config.callback(
                    BAIDU_AGENT_EVENT_CONNECTED,
                    NULL, 0,
                    client->config.user_data
                );
            }
            break;
            
        case HTTP_EVENT_ON_DATA:
            // 接收 SSE 数据
            if (evt->data_len > 0) {
                // 将数据追加到缓冲区
                size_t remaining = SSE_BUFFER_SIZE - client->sse_buffer_pos - 1;
                size_t copy_len = (evt->data_len < remaining) ? evt->data_len : remaining;
                
                if (copy_len > 0) {
                    memcpy(client->sse_buffer + client->sse_buffer_pos, evt->data, copy_len);
                    client->sse_buffer_pos += copy_len;
                    client->sse_buffer[client->sse_buffer_pos] = '\0';
                    
                    // 处理缓冲区中的完整行
                    char *line_start = client->sse_buffer;
                    char *line_end;
                    
                    while ((line_end = strstr(line_start, "\n")) != NULL) {
                        *line_end = '\0';
                        
                        // 解析 SSE 行
                        if (strncmp(line_start, "data: ", 6) == 0) {
                            const char *data = line_start + 6;
                            
                            // 跳过 [DONE] 消息
                            if (strcmp(data, "[DONE]") != 0) {
                                // 解析 JSON 数据
                                cJSON *json = cJSON_Parse(data);
                                if (json != NULL) {
                                    cJSON *answer = cJSON_GetObjectItem(json, "answer");
                                    if (answer && cJSON_IsString(answer)) {
                                        const char *answer_text = answer->valuestring;
                                        ESP_LOGI(TAG, "收到消息: %s", answer_text);
                                        
                                        if (client->config.callback) {
                                            client->config.callback(
                                                BAIDU_AGENT_EVENT_MESSAGE,
                                                answer_text,
                                                strlen(answer_text),
                                                client->config.user_data
                                            );
                                        }
                                    }
                                    cJSON_Delete(json);
                                }
                            }
                        }
                        
                        line_start = line_end + 1;
                    }
                    
                    // 将未处理的数据移到缓冲区开头
                    size_t remaining_len = strlen(line_start);
                    if (remaining_len > 0 && line_start != client->sse_buffer) {
                        memmove(client->sse_buffer, line_start, remaining_len + 1);
                        client->sse_buffer_pos = remaining_len;
                    } else if (remaining_len == 0) {
                        client->sse_buffer_pos = 0;
                        client->sse_buffer[0] = '\0';
                    }
                }
            }
            break;
            
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "已断开连接");
            if (client->config.callback) {
                client->config.callback(
                    BAIDU_AGENT_EVENT_DISCONNECTED,
                    NULL, 0,
                    client->config.user_data
                );
            }
            break;
            
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP 错误");
            if (client->config.callback) {
                client->config.callback(
                    BAIDU_AGENT_EVENT_ERROR,
                    "HTTP 连接错误", 0,
                    client->config.user_data
                );
            }
            break;
            
        default:
            break;
    }
    
    return ESP_OK;
}

/**
 * 构建 HTTP 请求 URL
 */
static char* build_request_url(const baidu_agent_client_t *client, const char *message) {
    // URL 格式: https://agents.baidu.com/api/conversation?appId=xxx&secretKey=xxx&query=xxx&stream=true
    
    // 计算 URL 长度
    size_t url_len = strlen("https://") + strlen(BAIDU_AGENT_API_HOST) + 
                     strlen(BAIDU_AGENT_API_PATH) + 
                     strlen("?appId=") + strlen(client->config.app_id) +
                     strlen("&secretKey=") + strlen(client->config.secret_key) +
                     strlen("&query=") + strlen(message) * 3 + // URL 编码最多3倍
                     strlen("&stream=true") + 1;
    
    char *url = malloc(url_len);
    if (url == NULL) {
        ESP_LOGE(TAG, "分配 URL 内存失败");
        return NULL;
    }
    
    // URL 编码消息
    char *encoded_message = malloc(strlen(message) * 3 + 1);
    if (encoded_message == NULL) {
        ESP_LOGE(TAG, "分配编码缓冲区失败");
        free(url);
        return NULL;
    }
    
    // 简单的 URL 编码
    const char *src = message;
    char *dst = encoded_message;
    while (*src) {
        if ((*src >= 'A' && *src <= 'Z') || 
            (*src >= 'a' && *src <= 'z') || 
            (*src >= '0' && *src <= '9') ||
            *src == '-' || *src == '_' || *src == '.' || *src == '~') {
            *dst++ = *src;
        } else {
            sprintf(dst, "%%%02X", (unsigned char)*src);
            dst += 3;
        }
        src++;
    }
    *dst = '\0';
    
    // 构建完整 URL
    snprintf(url, url_len,
             "https://%s%s?appId=%s&secretKey=%s&query=%s&stream=true",
             BAIDU_AGENT_API_HOST,
             BAIDU_AGENT_API_PATH,
             client->config.app_id,
             client->config.secret_key,
             encoded_message);
    
    free(encoded_message);
    return url;
}

/**
 * HTTP 客户端任务
 */
static void http_client_task(void *arg) {
    baidu_agent_client_t *client = (baidu_agent_client_t *)arg;
    
    while (!client->should_stop) {
        if (client->http_client != NULL) {
            // 执行 HTTP 请求
            esp_err_t err = esp_http_client_perform(client->http_client);
            
            if (err == ESP_OK) {
                int status_code = esp_http_client_get_status_code(client->http_client);
                ESP_LOGI(TAG, "HTTP GET 状态码 = %d", status_code);
            } else {
                ESP_LOGE(TAG, "HTTP GET 请求失败: %s", esp_err_to_name(err));
                
                // 错误回调
                if (client->config.callback) {
                    client->config.callback(
                        BAIDU_AGENT_EVENT_ERROR,
                        esp_err_to_name(err),
                        strlen(esp_err_to_name(err)),
                        client->config.user_data
                    );
                }
                
                // 自动重连逻辑
                if (client->config.auto_reconnect && 
                    client->retry_count < BAIDU_AGENT_MAX_RETRIES) {
                    client->retry_count++;
                    ESP_LOGI(TAG, "等待 %d 毫秒后重试 (%d/%d)...",
                             client->config.reconnect_interval,
                             client->retry_count,
                             BAIDU_AGENT_MAX_RETRIES);
                    vTaskDelay(pdMS_TO_TICKS(client->config.reconnect_interval));
                    continue;
                }
            }
            
            // 清理 HTTP 客户端
            esp_http_client_cleanup(client->http_client);
            client->http_client = NULL;
        }
        
        // 任务延迟
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    ESP_LOGI(TAG, "HTTP 客户端任务退出");
    client->task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * 初始化客户端
 */
baidu_agent_handle_t baidu_agent_init(const baidu_agent_config_t *config) {
    if (config == NULL || config->app_id == NULL || 
        config->secret_key == NULL || config->callback == NULL) {
        ESP_LOGE(TAG, "无效的配置参数");
        return NULL;
    }
    
    baidu_agent_client_t *client = calloc(1, sizeof(baidu_agent_client_t));
    if (client == NULL) {
        ESP_LOGE(TAG, "分配客户端内存失败");
        return NULL;
    }
    
    // 复制配置
    client->config = *config;
    client->config.app_id = strdup(config->app_id);
    client->config.secret_key = strdup(config->secret_key);
    
    if (client->config.app_id == NULL || client->config.secret_key == NULL) {
        ESP_LOGE(TAG, "复制配置字符串失败");
        free((void*)client->config.app_id);
        free((void*)client->config.secret_key);
        free(client);
        return NULL;
    }
    
    // 设置默认值
    if (client->config.reconnect_interval == 0) {
        client->config.reconnect_interval = 5000;
    }
    
    // 分配 SSE 缓冲区
    client->sse_buffer = malloc(SSE_BUFFER_SIZE);
    if (client->sse_buffer == NULL) {
        ESP_LOGE(TAG, "分配 SSE 缓冲区失败");
        free((void*)client->config.app_id);
        free((void*)client->config.secret_key);
        free(client);
        return NULL;
    }
    client->sse_buffer[0] = '\0';
    client->sse_buffer_pos = 0;
    
    // 创建互斥锁
    client->mutex = xSemaphoreCreateMutex();
    if (client->mutex == NULL) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        free(client->sse_buffer);
        free((void*)client->config.app_id);
        free((void*)client->config.secret_key);
        free(client);
        return NULL;
    }
    
    client->is_connected = false;
    client->should_stop = false;
    client->retry_count = 0;
    
    ESP_LOGI(TAG, "客户端初始化成功");
    return (baidu_agent_handle_t)client;
}

/**
 * 发送消息
 */
esp_err_t baidu_agent_send_message(
    baidu_agent_handle_t handle,
    const char *message,
    size_t message_len) {
    
    if (handle == NULL || message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    baidu_agent_client_t *client = (baidu_agent_client_t *)handle;
    
    if (message_len == 0) {
        message_len = strlen(message);
    }
    
    // 构建请求 URL
    char *url = build_request_url(client, message);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "发送消息到百度智能体: %s", message);
    ESP_LOGD(TAG, "请求 URL: %s", url);
    
    // 配置 HTTP 客户端
    esp_http_client_config_t http_config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = client,
        .timeout_ms = BAIDU_AGENT_READ_TIMEOUT,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .method = HTTP_METHOD_GET,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    // 清理旧的 HTTP 客户端
    if (client->http_client != NULL) {
        esp_http_client_cleanup(client->http_client);
    }
    
    // 创建新的 HTTP 客户端
    client->http_client = esp_http_client_init(&http_config);
    free(url);
    
    if (client->http_client == NULL) {
        ESP_LOGE(TAG, "创建 HTTP 客户端失败");
        return ESP_FAIL;
    }
    
    // 重置缓冲区
    client->sse_buffer_pos = 0;
    client->sse_buffer[0] = '\0';
    client->retry_count = 0;
    
    // 如果任务未运行,启动任务
    if (client->task_handle == NULL) {
        BaseType_t ret = xTaskCreate(
            http_client_task,
            "baidu_agent_http",
            8192,
            client,
            5,
            &client->task_handle
        );
        
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "创建 HTTP 客户端任务失败");
            esp_http_client_cleanup(client->http_client);
            client->http_client = NULL;
            return ESP_FAIL;
        }
    }
    
    return ESP_OK;
}

/**
 * 启动会话
 */
esp_err_t baidu_agent_start(baidu_agent_handle_t handle) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    baidu_agent_client_t *client = (baidu_agent_client_t *)handle;
    client->should_stop = false;
    
    ESP_LOGI(TAG, "启动百度智能体会话");
    return ESP_OK;
}

/**
 * 停止会话
 */
esp_err_t baidu_agent_stop(baidu_agent_handle_t handle) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    baidu_agent_client_t *client = (baidu_agent_client_t *)handle;
    client->should_stop = true;
    
    // 等待任务退出
    if (client->task_handle != NULL) {
        int timeout = 100;
        while (client->task_handle != NULL && timeout-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    // 清理 HTTP 客户端
    if (client->http_client != NULL) {
        esp_http_client_cleanup(client->http_client);
        client->http_client = NULL;
    }
    
    client->is_connected = false;
    
    ESP_LOGI(TAG, "停止百度智能体会话");
    return ESP_OK;
}

/**
 * 销毁客户端
 */
void baidu_agent_destroy(baidu_agent_handle_t handle) {
    if (handle == NULL) {
        return;
    }
    
    baidu_agent_client_t *client = (baidu_agent_client_t *)handle;
    
    // 停止会话
    baidu_agent_stop(handle);
    
    // 释放资源
    if (client->mutex != NULL) {
        vSemaphoreDelete(client->mutex);
    }
    
    free(client->sse_buffer);
    free((void*)client->config.app_id);
    free((void*)client->config.secret_key);
    free(client);
    
    ESP_LOGI(TAG, "客户端已销毁");
}

/**
 * 检查连接状态
 */
bool baidu_agent_is_connected(baidu_agent_handle_t handle) {
    if (handle == NULL) {
        return false;
    }
    
    baidu_agent_client_t *client = (baidu_agent_client_t *)handle;
    return client->is_connected;
}