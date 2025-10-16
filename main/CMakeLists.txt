idf_component_register(SRCS "hello_world_main.c"
                       REQUIRES 
                           esp_lcd
                       PRIV_REQUIRES 
                           spi_flash 
                           driver
                       INCLUDE_DIRS "")
