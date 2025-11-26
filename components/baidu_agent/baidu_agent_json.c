/**
 * 百度智能体 JSON 数据解析模块实现
 */

#include "baidu_agent_json.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "BAIDU_AGENT_JSON";

/**
 * 处理 content 数组中的单个 content item
 * 根据 dataType 提取不同格式的内容
 */
static void process_content_item(baidu_agent_client_t *client, cJSON *content_item) {
    if (!content_item) return;

    // 获取 dataType
    cJSON *data_type = cJSON_GetObjectItem(content_item, "dataType");
    const char *type_str = (data_type && cJSON_IsString(data_type)) ? 
                           data_type->valuestring : "unknown";

    // 获取 data 字段
    cJSON *data_field = cJSON_GetObjectItem(content_item, "data");
    if (!data_field) {
        ESP_LOGW(TAG, "content item 缺少 data 字段");
        return;
    }

    // 根据 dataType 处理不同格式
    if (strcmp(type_str, "markdown") == 0) {
        // 处理 markdown 类型
        cJSON *text_field = cJSON_GetObjectItem(data_field, "text");
        if (text_field && cJSON_IsString(text_field)) {
            const char *text = text_field->valuestring;
            ESP_LOGI(TAG, "AI回复 [markdown]: %s", text);

            if (client->config.callback) {
                client->config.callback(
                    BAIDU_AGENT_EVENT_MESSAGE,
                    text,
                    strlen(text),
                    client->config.user_data
                );
            }
        }
    } else if (strcmp(type_str, "uiData") == 0) {
        // 处理 uiData 类型（基于一言 ui.json 规范）
        char *ui_json = cJSON_PrintUnformatted(data_field);
        if (ui_json) {
            ESP_LOGI(TAG, "AI回复 [uiData]: %s", ui_json);
            
            if (client->config.callback) {
                client->config.callback(
                    BAIDU_AGENT_EVENT_MESSAGE,
                    ui_json,
                    strlen(ui_json),
                    client->config.user_data
                );
            }
            free(ui_json);
        }
    } else {
        ESP_LOGW(TAG, "未知的 dataType: %s", type_str);
    }

    // 检查 isFinished 标志
    cJSON *is_finished = cJSON_GetObjectItem(content_item, "isFinished");
    if (is_finished && cJSON_IsBool(is_finished) && cJSON_IsTrue(is_finished)) {
        ESP_LOGI(TAG, "内容传输完成");
    }

    // 处理 progress 信息（如果有）
    cJSON *progress = cJSON_GetObjectItem(content_item, "progress");
    if (progress) {
        cJSON *tools_status = cJSON_GetObjectItem(progress, "toolsStatus");
        if (tools_status && cJSON_IsArray(tools_status)) {
            cJSON *tool = NULL;
            cJSON_ArrayForEach(tool, tools_status) {
                cJSON *tool_name = cJSON_GetObjectItem(tool, "toolName");
                cJSON *status = cJSON_GetObjectItem(tool, "status");
                if (tool_name && status) {
                    ESP_LOGI(TAG, "工具调用: %s, 状态: %s", 
                            tool_name->valuestring, status->valuestring);
                }
            }
        }
    }
}

/**
 * 处理 message 对象中的 content 数组
 */
static void process_message_content(baidu_agent_client_t *client, cJSON *message_obj) {
    if (!message_obj) return;

    // 提取 threadId
    cJSON *thread_id = cJSON_GetObjectItem(message_obj, "threadId");
    if (thread_id && cJSON_IsString(thread_id)) {
        if (client->thread_id != NULL) {
            free(client->thread_id);
        }
        client->thread_id = strdup(thread_id->valuestring);
        ESP_LOGI(TAG, "会话ID: %s", client->thread_id);
    }

    // 提取 endTurn 标志
    cJSON *end_turn = cJSON_GetObjectItem(message_obj, "endTurn");
    if (end_turn && cJSON_IsBool(end_turn) && cJSON_IsTrue(end_turn)) {
        ESP_LOGI(TAG, "对话轮次结束");
    }

    // 提取 msgId
    cJSON *msg_id = cJSON_GetObjectItem(message_obj, "msgId");
    if (msg_id && cJSON_IsString(msg_id)) {
        ESP_LOGD(TAG, "消息ID: %s", msg_id->valuestring);
    }

    // 处理 content 数组
    cJSON *content_array = cJSON_GetObjectItem(message_obj, "content");
    if (content_array && cJSON_IsArray(content_array)) {
        int content_count = cJSON_GetArraySize(content_array);
        ESP_LOGI(TAG, "收到 %d 个 content 项", content_count);

        cJSON *content_item = NULL;
        cJSON_ArrayForEach(content_item, content_array) {
            process_content_item(client, content_item);
        }
    }
}

/**
 * 处理 SSE 消息的 JSON 数据
 */
void baidu_agent_process_json(baidu_agent_client_t *client, const char *json_str) {
    cJSON *json = cJSON_Parse(json_str);
    if (json == NULL) {
        ESP_LOGE(TAG, "JSON解析失败: %s", json_str);
        return;
    }

    // 打印完整的 JSON 用于调试
    char *formatted_json = cJSON_PrintUnformatted(json);
    if (formatted_json) {
        ESP_LOGI(TAG, "收到SSE数据: %s", formatted_json);
        free(formatted_json);
    }

    // 检查状态码
    cJSON *status = cJSON_GetObjectItem(json, "status");
    if (status && cJSON_IsNumber(status) && status->valueint != 0) {
        cJSON *message = cJSON_GetObjectItem(json, "message");
        const char *err_msg = (message && cJSON_IsString(message)) ? 
                              message->valuestring : "未知错误";
        ESP_LOGE(TAG, "API返回错误: status=%d, message=%s", 
                status->valueint, err_msg);
        
        // 通过回调通知错误
        if (client->config.callback) {
            char error_info[128];
            snprintf(error_info, sizeof(error_info), "状态码%d: %s", 
                    status->valueint, err_msg);
            client->config.callback(
                BAIDU_AGENT_EVENT_ERROR,
                error_info,
                strlen(error_info),
                client->config.user_data
            );
        }
        
        cJSON_Delete(json);
        return;
    }

    // 提取 data.message
    cJSON *data_obj = cJSON_GetObjectItem(json, "data");
    if (data_obj) {
        cJSON *message_obj = cJSON_GetObjectItem(data_obj, "message");
        if (message_obj) {
            process_message_content(client, message_obj);
        }
    }

    cJSON_Delete(json);
}

/**
 * 构建 POST 请求体 (JSON格式)
 */
char* baidu_agent_build_request_body(const baidu_agent_client_t *client, const char *message) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "创建JSON对象失败");
        return NULL;
    }

    // 添加 threadId (如果有)
    const char *thread_id_to_use = client->thread_id ? client->thread_id : client->config.thread_id;
    if (thread_id_to_use != NULL) {
        cJSON_AddStringToObject(root, "threadId", thread_id_to_use);
    }

    // 添加 message 对象
    cJSON *message_obj = cJSON_CreateObject();
    if (message_obj == NULL) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "创建message对象失败");
        return NULL;
    }

    // message.content 是 Object 类型，包含 type 和 value
    cJSON *content_obj = cJSON_CreateObject();
    if (content_obj == NULL) {
        cJSON_Delete(message_obj);
        cJSON_Delete(root);
        ESP_LOGE(TAG, "创建content对象失败");
        return NULL;
    }
    
    // 添加 type 字段
    cJSON_AddStringToObject(content_obj, "type", "text");
    
    // 添加 value 对象，包含 showText
    cJSON *value_obj = cJSON_CreateObject();
    if (value_obj == NULL) {
        cJSON_Delete(content_obj);
        cJSON_Delete(message_obj);
        cJSON_Delete(root);
        ESP_LOGE(TAG, "创建value对象失败");
        return NULL;
    }
    cJSON_AddStringToObject(value_obj, "showText", message);
    cJSON_AddItemToObject(content_obj, "value", value_obj);
    
    cJSON_AddItemToObject(message_obj, "content", content_obj);
    cJSON_AddItemToObject(root, "message", message_obj);

    // 添加 source (智能体ID)
    cJSON_AddStringToObject(root, "source", client->config.app_id);

    // 添加 from (固定值 openapi)
    cJSON_AddStringToObject(root, "from", "openapi");

    // 添加 openId (外部用户ID)
    cJSON_AddStringToObject(root, "openId", client->config.open_id);

    // 转换为字符串
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "生成JSON字符串失败");
        return NULL;
    }

    ESP_LOGI(TAG, "请求体: %s", json_str);
    return json_str;
}
