if(ESP_PLATFORM)
    set(BSP_FONT_SRCS "src/fonts/nf_font_20.c" "src/fonts/nf_font_32.c")
endif()

idf_component_register(
    SRCS
        "src/bsp_audio.c"
        "src/bsp_display.c"
        "src/bsp_power_key.c"
        ${BSP_FONT_SRCS}
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        esp_lcd
        esp_adc
        esp_partition
        app_update
)
