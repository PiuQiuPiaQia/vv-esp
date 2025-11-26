/**
 * 百度智能体 SSE (Server-Sent Events) 协议处理模块实现
 */

#include "baidu_agent_sse.h"
#include "baidu_agent_json.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "BAIDU_AGENT_SSE";

/**
 * HTTP 事件处理回调
 */
esp_err_t baidu_agent_http_event_handler(esp_http_client_event_t *evt) {
    baidu_agent_client_t *client = (baidu_agent_client_t *)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "已连接到服务器");
            ESP_LOGI(TAG, "等待服务器响应...");
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
                ESP_LOGI(TAG, "原始数据: %.*s", evt->data_len, (char*)evt->data);

                // 将数据追加到缓冲区
                size_t remaining = SSE_BUFFER_SIZE - client->sse_buffer_pos - 1;
                size_t copy_len = (evt->data_len < remaining) ? evt->data_len : remaining;

                if (copy_len > 0) {
                    memcpy(client->sse_buffer + client->sse_buffer_pos, evt->data, copy_len);
                    client->sse_buffer_pos += copy_len;
                    client->sse_buffer[client->sse_buffer_pos] = '\0';

                    ESP_LOGI(TAG, "当前缓冲区: %s", client->sse_buffer);

                    // 处理缓冲区中的完整行
                    char *line_start = client->sse_buffer;
                    char *line_end;
                    static char current_event[32] = "message";  // 默认事件类型

                    while ((line_end = strstr(line_start, "\n")) != NULL) {
                        *line_end = '\0';

                        // 解析 SSE event 行
                        if (strncmp(line_start, "event:", 6) == 0) {
                            const char *event_type = line_start + 6;
                            strncpy(current_event, event_type, sizeof(current_event) - 1);
                            current_event[sizeof(current_event) - 1] = '\0';
                            ESP_LOGI(TAG, "SSE事件类型: %s", current_event);
                        }
                        // 解析 SSE data 行
                        else if (strncmp(line_start, "data:", 5) == 0) {
                            const char *data = line_start + 5;
                            // 跳过可能的空格
                            while (*data == ' ') data++;
                            
                            ESP_LOGI(TAG, "SSE数据 (事件=%s): %s", current_event, data);

                            // 只处理 message 事件，跳过 ping 等其他事件
                            if (strcmp(current_event, "message") == 0 && strcmp(data, "[DONE]") != 0) {
                                baidu_agent_process_json(client, data);
                            } else {
                                ESP_LOGD(TAG, "跳过非message事件或DONE消息");
                            }
                        }
                        // 空行表示一个 SSE 事件结束
                        else if (strlen(line_start) == 0) {
                            ESP_LOGD(TAG, "SSE事件分隔符（空行）");
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
            ESP_LOGE(TAG, "HTTP 连接错误");
            if (client->config.callback) {
                client->config.callback(
                    BAIDU_AGENT_EVENT_ERROR,
                    "HTTP 连接错误", 0,
                    client->config.user_data
                );
            }
            break;

        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP Header: %s: %s", evt->header_key, evt->header_value);
            break;
            
        default:
            ESP_LOGI(TAG, "未处理的HTTP事件: %d", evt->event_id);
            break;
    }
    
    return ESP_OK;
}

/**
 * 构建 HTTP 请求 URL (包含query参数: appId和secretKey)
 */
char* baidu_agent_build_request_url(const baidu_agent_client_t *client) {
    // URL 格式: https://agentapi.baidu.com/assistant/conversation?appId=xxx&secretKey=xxx

    size_t url_len = strlen("https://") + strlen(BAIDU_AGENT_API_HOST) +
                     strlen(BAIDU_AGENT_API_PATH) +
                     strlen("?appId=") + strlen(client->config.app_id) +
                     strlen("&secretKey=") + strlen(client->config.secret_key) + 1;

    char *url = malloc(url_len);
    if (url == NULL) {
        ESP_LOGE(TAG, "分配 URL 内存失败");
        return NULL;
    }

    snprintf(url, url_len,
             "https://%s%s?appId=%s&secretKey=%s",
             BAIDU_AGENT_API_HOST,
             BAIDU_AGENT_API_PATH,
             client->config.app_id,
             client->config.secret_key);

    return url;
}
