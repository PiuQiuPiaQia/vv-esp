/**
 * 百度智能体 Conversation API 客户端实现
 * 基于 SSE (Server-Sent Events) 协议
 */

#include "baidu_agent_client.h"
#include "baidu_agent_types.h"
#include "baidu_agent_sse.h"
#include "baidu_agent_json.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "BAIDU_AGENT";

/**
 * HTTP 客户端任务
 */
static void http_client_task(void *arg) {
    baidu_agent_client_t *client = (baidu_agent_client_t *)arg;

    ESP_LOGI(TAG, "HTTP 客户端任务已启动");

    while (!client->should_stop) {
        if (client->http_client != NULL) {
            ESP_LOGI(TAG, "开始执行 HTTP 请求...");
            // 执行 HTTP 请求
            esp_err_t err = esp_http_client_perform(client->http_client);
            ESP_LOGI(TAG, "HTTP 请求完成，结果: %s", esp_err_to_name(err));

            if (err == ESP_OK) {
                int status_code = esp_http_client_get_status_code(client->http_client);
                int content_length = esp_http_client_get_content_length(client->http_client);
                ESP_LOGI(TAG, "HTTP POST 状态码 = %d, Content-Length = %d", status_code, content_length);

                if (status_code != 200) {
                    ESP_LOGE(TAG, "服务器返回错误状态码: %d", status_code);
                }
            } else {
                ESP_LOGE(TAG, "HTTP POST 请求失败: %s (%d)", esp_err_to_name(err), err);

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

            // 释放POST数据
            if (client->post_data != NULL) {
                free(client->post_data);
                client->post_data = NULL;
            }
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
        config->secret_key == NULL || config->open_id == NULL ||
        config->callback == NULL) {
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
    client->config.open_id = strdup(config->open_id);

    if (client->config.app_id == NULL || client->config.secret_key == NULL ||
        client->config.open_id == NULL) {
        ESP_LOGE(TAG, "复制配置字符串失败");
        free((void*)client->config.app_id);
        free((void*)client->config.secret_key);
        free((void*)client->config.open_id);
        free(client);
        return NULL;
    }

    // 复制thread_id（如果有）
    if (config->thread_id != NULL) {
        client->config.thread_id = strdup(config->thread_id);
        if (client->config.thread_id == NULL) {
            ESP_LOGE(TAG, "复制thread_id失败");
            free((void*)client->config.app_id);
            free((void*)client->config.secret_key);
            free((void*)client->config.open_id);
            free(client);
            return NULL;
        }
    } else {
        client->config.thread_id = NULL;
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
        free((void*)client->config.open_id);
        free((void*)client->config.thread_id);
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
        free((void*)client->config.open_id);
        free((void*)client->config.thread_id);
        free(client);
        return NULL;
    }

    client->is_connected = false;
    client->should_stop = false;
    client->retry_count = 0;
    client->thread_id = NULL;
    client->post_data = NULL;

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
    char *url = baidu_agent_build_request_url(client);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // 构建请求体
    char *post_data = baidu_agent_build_request_body(client, message);
    if (post_data == NULL) {
        free(url);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "========== 发送消息到百度智能体 ==========");
    ESP_LOGI(TAG, "消息内容: %s", message);
    ESP_LOGI(TAG, "请求 URL: %s", url);
    ESP_LOGI(TAG, "请求体: %s", post_data);
    ESP_LOGI(TAG, "==========================================");

    // 配置 HTTP 客户端
    esp_http_client_config_t http_config = {
        .url = url,
        .event_handler = baidu_agent_http_event_handler,
        .user_data = client,
        .timeout_ms = BAIDU_AGENT_READ_TIMEOUT,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
        .method = HTTP_METHOD_POST,
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
        free(post_data);
        return ESP_FAIL;
    }

    // 设置 HTTP 请求头
    esp_http_client_set_header(client->http_client, "Content-Type", "application/json");

    // 设置 POST 数据
    esp_http_client_set_post_field(client->http_client, post_data, strlen(post_data));

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
            free(post_data);
            return ESP_FAIL;
        }
    }

    // 存储post_data指针，在请求完成后释放
    if (client->post_data != NULL) {
        free(client->post_data);
    }
    client->post_data = post_data;

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

    // 释放缓冲区
    if (client->sse_buffer != NULL) {
        free(client->sse_buffer);
    }

    // 释放POST数据
    if (client->post_data != NULL) {
        free(client->post_data);
    }

    // 释放动态thread_id
    if (client->thread_id != NULL) {
        free(client->thread_id);
    }

    // 释放配置字符串
    free((void*)client->config.app_id);
    free((void*)client->config.secret_key);
    free((void*)client->config.open_id);
    if (client->config.thread_id != NULL) {
        free((void*)client->config.thread_id);
    }

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
