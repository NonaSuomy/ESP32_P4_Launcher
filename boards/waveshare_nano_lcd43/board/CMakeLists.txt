idf_component_register(
 SRCS "../../pax_p4_common/board.c" "../../pax_p4_common/board_display.c"
      "../../pax_p4_common/board_touch.c" "../../pax_p4_common/board_audio.c"
      "../../pax_p4_common/board_ethernet.c" "../../pax_p4_common/board_optional.c"
 INCLUDE_DIRS "." "../../pax_p4_common"
 REQUIRES bsp fatfs vfs sdmmc esp_driver_sdmmc esp_driver_gpio esp_driver_ledc esp_lcd esp_hw_support esp_mm esp_driver_i2c esp_driver_i2s esp_adc esp_eth esp_netif esp_event
 WHOLE_ARCHIVE
)
