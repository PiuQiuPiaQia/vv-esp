/**
 * 百度智能体 SSE (Server-Sent Events) 协议处理模块
 */

#ifndef BAIDU_AGENT_SSE_H
#define BAIDU_AGENT_SSE_H

#include "baidu_agent_types.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HTTP 事件处理回调
 * @param evt HTTP 客户端事件
 * @return ESP_OK 成功
 */
esp_err_t baidu_agent_http_event_handler(esp_http_client_event_t *evt);

/**
 * 构建 HTTP 请求 URL (包含query参数: appId和secretKey)
 * @param client 客户端实例
 * @return URL 字符串，需要调用者释放
 */
char* baidu_agent_build_request_url(const baidu_agent_client_t *client);

#ifdef __cplusplus
}
#endif

#endif // BAIDU_AGENT_SSE_H
