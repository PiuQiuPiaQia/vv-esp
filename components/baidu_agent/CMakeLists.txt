idf_component_register(SRCS "baidu_agent_client.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client json esp-tls mbedtls)
