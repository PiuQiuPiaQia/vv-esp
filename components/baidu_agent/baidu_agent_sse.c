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
            // 重置 SSE 事件类型
            strncpy(client->current_sse_event, "message", sizeof(client->current_sse_event) - 1);
            client->current_sse_event[sizeof(client->current_sse_event) - 1] = '\0';
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
                ESP_LOGD(TAG, "原始数据 (%d bytes): %.*s", evt->data_len, evt->data_len, (char*)evt->data);

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
                        // 处理可能的 \r\n
                        char *actual_end = line_end;
                        if (actual_end > line_start && *(actual_end - 1) == '\r') {
                            *(actual_end - 1) = '\0';
                        }
                        *line_end = '\0';

                        // 跳过空行（SSE 事件分隔符）
                        if (strlen(line_start) == 0) {
                            // 空行表示一个 SSE 事件结束，重置事件类型为默认值
                            strncpy(client->current_sse_event, "message", sizeof(client->current_sse_event) - 1);
                            line_start = line_end + 1;
                            continue;
                        }

                        // 解析 SSE event 行
                        if (strncmp(line_start, "event:", 6) == 0) {
                            const char *event_type = line_start + 6;
                            // 跳过可能的空格
                            while (*event_type == ' ') event_type++;
                            strncpy(client->current_sse_event, event_type, sizeof(client->current_sse_event) - 1);
                            client->current_sse_event[sizeof(client->current_sse_event) - 1] = '\0';
                            ESP_LOGI(TAG, "SSE事件类型: %s", client->current_sse_event);
                        }
                        // 解析 SSE data 行
                        else if (strncmp(line_start, "data:", 5) == 0) {
                            const char *data = line_start + 5;
                            // 跳过可能的空格
                            while (*data == ' ') data++;
                            
                            ESP_LOGI(TAG, "SSE数据 (事件=%s): %s", client->current_sse_event, data);

                            // 只处理 message 事件，跳过 ping 等其他事件
                            if (strcmp(client->current_sse_event, "message") == 0) {
                                if (strcmp(data, "[DONE]") == 0) {
                                    ESP_LOGI(TAG, "收到 [DONE] 标记，SSE 流结束");
                                } else {
                                    baidu_agent_process_json(client, data);
                                }
                            } else {
                                ESP_LOGD(TAG, "跳过非 message 事件: %s", client->current_sse_event);
                            }
                        }

                        line_start = line_end + 1;
                    }

                    // 将未处理的数据移到缓冲区开头
                    size_t remaining_len = client->sse_buffer + client->sse_buffer_pos - line_start;
                    if (remaining_len > 0 && line_start != client->sse_buffer) {
                        memmove(client->sse_buffer, line_start, remaining_len);
                        client->sse_buffer_pos = remaining_len;
                        client->sse_buffer[client->sse_buffer_pos] = '\0';
                    } else if (line_start >= client->sse_buffer + client->sse_buffer_pos) {
                        client->sse_buffer_pos = 0;
                        client->sse_buffer[0] = '\0';
                    }
                } else {
                    ESP_LOGW(TAG, "SSE 缓冲区已满，丢弃数据");
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
