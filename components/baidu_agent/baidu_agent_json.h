/**
 * 百度智能体 JSON 数据解析模块
 */

#ifndef BAIDU_AGENT_JSON_H
#define BAIDU_AGENT_JSON_H

#include "baidu_agent_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 处理 SSE 消息的 JSON 数据
 * @param client 客户端实例
 * @param json_str JSON 字符串
 */
void baidu_agent_process_json(baidu_agent_client_t *client, const char *json_str);

/**
 * 构建 POST 请求体 (JSON格式)
 * @param client 客户端实例
 * @param message 消息内容
 * @return JSON 字符串，需要调用者释放
 */
char* baidu_agent_build_request_body(const baidu_agent_client_t *client, const char *message);

#ifdef __cplusplus
}
#endif

#endif // BAIDU_AGENT_JSON_H
