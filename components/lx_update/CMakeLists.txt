idf_component_register(SRCS "lx_update.c" "lx_update_net.c" "lx_http.c" "lx_appstore.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES bsp lx_common app_update esp_app_format bootloader_support log nvs_flash
                                 esp_http_client esp-tls mbedtls json)
