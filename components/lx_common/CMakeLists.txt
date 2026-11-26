idf_component_register(SRCS "lx_crc32.c" "lx_zip.c" "lx_storage.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES freertos log esp_hw_support)
