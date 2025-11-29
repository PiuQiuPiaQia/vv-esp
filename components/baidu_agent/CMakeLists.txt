idf_component_register(SRCS "baidu_agent_client.c" "baidu_agent_sse.c" "baidu_agent_json.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client json esp-tls mbedtls tts_service)
