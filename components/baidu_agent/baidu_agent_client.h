/**
 * 百度智能体 Conversation API 客户端
 * 基于 SSE (Server-Sent Events) 协议实现
 */

#ifndef BAIDU_AGENT_CLIENT_H
#define BAIDU_AGENT_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 百度智能体 API 配置
#define BAIDU_AGENT_API_HOST "agentapi.baidu.com"
#define BAIDU_AGENT_API_PATH "/assistant/conversation"
#define BAIDU_AGENT_API_PORT 443

// 最大重试次数
#define BAIDU_AGENT_MAX_RETRIES 3

// 超时设置 (毫秒)
#define BAIDU_AGENT_CONNECT_TIMEOUT 10000
#define BAIDU_AGENT_READ_TIMEOUT 30000

/**
 * SSE 事件类型
 */
typedef enum {
    BAIDU_AGENT_EVENT_CONNECTING,    // 正在连接
    BAIDU_AGENT_EVENT_CONNECTED,     // 已连接
    BAIDU_AGENT_EVENT_MESSAGE,       // 收到消息
    BAIDU_AGENT_EVENT_ERROR,         // 错误
    BAIDU_AGENT_EVENT_DISCONNECTED   // 断开连接
} baidu_agent_event_type_t;

/**
 * 回调函数类型
 * @param event_type 事件类型
 * @param data 事件数据 (对于 MESSAGE 事件是消息内容,对于 ERROR 事件是错误信息)
 * @param data_len 数据长度
 * @param user_data 用户自定义数据
 */
typedef void (*baidu_agent_callback_t)(
    baidu_agent_event_type_t event_type,
    const char *data,
    size_t data_len,
    void *user_data
);

/**
 * 百度智能体客户端配置
 */
typedef struct {
    const char *app_id;           // 应用 ID (必填)
    const char *secret_key;       // 密钥 (必填)
    const char *open_id;          // 外部用户ID (必填，需要保证唯一性)
    const char *thread_id;        // 会话ID (可选，续接对话时需要)
    baidu_agent_callback_t callback; // 回调函数 (必填)
    void *user_data;              // 用户自定义数据 (可选)
    bool auto_reconnect;          // 是否自动重连 (默认 true)
    uint32_t reconnect_interval;  // 重连间隔 (毫秒, 默认 5000)
} baidu_agent_config_t;

/**
 * 百度智能体客户端句柄
 */
typedef void* baidu_agent_handle_t;

/**
 * 初始化百度智能体客户端
 * @param config 客户端配置
 * @return 客户端句柄,失败返回 NULL
 */
baidu_agent_handle_t baidu_agent_init(const baidu_agent_config_t *config);

/**
 * 发送消息到百度智能体
 * @param handle 客户端句柄
 * @param message 消息内容
 * @param message_len 消息长度 (0 表示自动计算)
 * @return ESP_OK 成功, 其他值表示错误
 */
esp_err_t baidu_agent_send_message(
    baidu_agent_handle_t handle,
    const char *message,
    size_t message_len
);

/**
 * 启动会话 (连接到 API)
 * @param handle 客户端句柄
 * @return ESP_OK 成功, 其他值表示错误
 */
esp_err_t baidu_agent_start(baidu_agent_handle_t handle);

/**
 * 停止会话 (断开连接)
 * @param handle 客户端句柄
 * @return ESP_OK 成功, 其他值表示错误
 */
esp_err_t baidu_agent_stop(baidu_agent_handle_t handle);

/**
 * 销毁客户端并释放资源
 * @param handle 客户端句柄
 */
void baidu_agent_destroy(baidu_agent_handle_t handle);

/**
 * 检查客户端是否已连接
 * @param handle 客户端句柄
 * @return true 已连接, false 未连接
 */
bool baidu_agent_is_connected(baidu_agent_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // BAIDU_AGENT_CLIENT_H