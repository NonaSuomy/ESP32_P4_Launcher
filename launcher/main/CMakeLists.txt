idf_component_register(
    SRCS "main.c" "home.c" "store.c"
    INCLUDE_DIRS "."
    EMBED_FILES "../../apps/common/icons/folder_community.png" "../../apps/common/icons/store.png"
    REQUIRES board bsp lx_loader lx_devlink lx_comp lx_audio lx_input lx_common lx_update lx_net lx_web lvgl
             nvs_flash heap esp_mm esp_timer esp_driver_tsens fatfs vfs
)
