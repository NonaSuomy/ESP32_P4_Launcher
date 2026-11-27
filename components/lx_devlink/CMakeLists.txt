idf_component_register(
    SRCS "lx_devlink.c"
    INCLUDE_DIRS "include"
    REQUIRES lx_loader
    PRIV_REQUIRES lx_common bsp lx_input lx_net esp_driver_usb_serial_jtag esp_driver_uart vfs fatfs esp_timer
                  espcoredump esp_system esp_app_format heap hal soc nvs_flash lwip
)
