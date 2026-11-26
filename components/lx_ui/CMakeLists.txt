idf_component_register(
    SRCS "lx_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES papp_abi
    PRIV_REQUIRES lx_common lx_net lx_gfx lx_comp lx_input lx_audio bsp esp_timer heap vfs nvs_flash
)
