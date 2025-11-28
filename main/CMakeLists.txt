idf_component_register(SRCS "main.c"
                       REQUIRES
                           esp_lcd
                           wifi_manager
                           baidu_agent
                           font_manager
                           tts_service
                       PRIV_REQUIRES
                           spi_flash
                           driver
                       INCLUDE_DIRS ".")
