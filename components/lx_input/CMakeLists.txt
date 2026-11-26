idf_component_register(
    SRCS "lx_input.c" "lx_input_usb.c" "lx_pad_decode.c" "lx_input_hidpad.c" "lx_hid_parse.c" "lx_input_xinput.c" "lx_padmap.c"
    INCLUDE_DIRS "include"
    REQUIRES papp_abi
    PRIV_REQUIRES bsp esp_timer usb nvs_flash hal
)
# USB host BNA workaround (lx_input_usb.c): IDF asserts on a channel error without a halt
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=usb_dwc_hal_chan_decode_intr")
# USB hubs (lx_input_usb.c): the board's port is limited to full speed each time IDF powers it
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=hcd_port_command")
