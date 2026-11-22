idf_component_register(SRCS "lx_usbd.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "tusb"
                       REQUIRES papp_abi
                       PRIV_REQUIRES espressif__tinyusb usb esp_hw_support esp_system esp_timer hal soc lx_common lx_devlink)
# TinyUSB finds its configuration (tusb/tusb_config.h) on its own include path.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/tusb")
