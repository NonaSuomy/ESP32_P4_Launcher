// SPDX-License-Identifier: MIT
#include "bsp.h"
__attribute__((weak)) esp_err_t bsp_ethernet_init(void) {
  return ESP_ERR_NOT_SUPPORTED;
}
__attribute__((weak)) const char *bsp_ethernet_ip(void) { return ""; }
