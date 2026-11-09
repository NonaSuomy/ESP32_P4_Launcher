idf_component_register(
    SRCS "lx_app.c" "lx_services.c" "lx_svc_disp.c" "lx_svc_audio.c" "lx_svc_input.c" "lx_svc_thrd.c" "lx_svc_ui.c" "lx_svc_net.c" "lx_svc_ble.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES papp_abi esp_mm
    PRIV_REQUIRES lx_common bsp lx_comp lx_audio lx_input esp_timer heap esp_system lx_ui lx_net lwip mbedtls esp_hw_support bt espressif__esp_hosted
)

# BLE: esp_hosted 2.x starts the C6's controller on request; 1.4 (Tab5) runs it from the C6's boot
idf_component_get_property(hosted_dir espressif__esp_hosted COMPONENT_DIR)
file(STRINGS "${hosted_dir}/idf_component.yml" hosted_line REGEX "^version:")
string(REGEX MATCH "[0-9]+" hosted_major "${hosted_line}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE LX_HOSTED_MAJOR=${hosted_major})
