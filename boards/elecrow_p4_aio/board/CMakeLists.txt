idf_component_register(
    SRCS "board.c" "board_display.c" "board_touch.c" "board_audio.c" "board_buttons.c"
    INCLUDE_DIRS "."
    REQUIRES bsp fatfs vfs sdmmc esp_driver_sdmmc esp_driver_gpio esp_driver_ledc esp_lcd esp_hw_support esp_mm esp_driver_i2c esp_driver_i2s esp_adc
    WHOLE_ARCHIVE       # implements the bsp.h contract: link every file, callers sit in any component
)
