# Guition JC1060P470C 7" with a rev v3.x chip: the same board as guition_p4_7, built for v3 silicon.
# One image can't run on both chip generations (the ROM and memory map differ), so this is its own
# board id: its own image (launcher_guition_p4_7_v3) and its own entry in latest.json.
set(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../guition_p4_7/board")
idf_component_register(
    SRCS "${SRC_DIR}/board.c" "${SRC_DIR}/board_display.c" "${SRC_DIR}/board_touch.c" "${SRC_DIR}/board_audio.c"
         "${SRC_DIR}/board_buttons.c" "${SRC_DIR}/board_rtc.c"
    INCLUDE_DIRS "${SRC_DIR}"
    REQUIRES bsp fatfs vfs sdmmc esp_driver_sdmmc esp_driver_gpio esp_driver_ledc esp_lcd esp_hw_support esp_mm esp_driver_i2c esp_driver_i2s
    WHOLE_ARCHIVE       # implements the bsp.h contract: link every file, callers sit in any component
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_CHIP_V3=1)
