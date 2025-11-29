idf_component_register(
    SRCS "streaming_tts.c" "tts_service.c"
    INCLUDE_DIRS "."
    REQUIRES driver freertos esp_codec_dev esp_http_client mbedtls esp_timer
)
