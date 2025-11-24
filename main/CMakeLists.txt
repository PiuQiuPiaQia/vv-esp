idf_component_register(SRCS "wifi_manager.c" "baidu_agent_client.c" "hello_world_main.c"
                       REQUIRES
                           esp_lcd
                           esp_http_client
                           json
                           esp_wifi
                           nvs_flash
                       PRIV_REQUIRES
                           spi_flash
                           driver
                           esp-tls
                       INCLUDE_DIRS ".")
