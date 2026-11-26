idf_component_register(
    SRCS "lx_comp.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp
    PRIV_REQUIRES lx_common esp_driver_ppa esp_mm esp_timer heap
)
