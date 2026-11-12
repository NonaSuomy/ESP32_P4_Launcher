// SPDX-License-Identifier: MIT
#include "board_internal.h"
#include "board_pins.h"
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_netif.h"
#include <stdio.h>
static esp_eth_handle_t eth;
static char address[16];
static void event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
    ip_event_got_ip_t *e = data;
    snprintf(address, sizeof(address), IPSTR, IP2STR(&e->ip_info.ip));
  }
  if ((base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) ||
      (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP))
    address[0] = 0;
}
const char *bsp_ethernet_ip(void) { return address; }
esp_err_t bsp_ethernet_init(void) {
  if (eth)
    return ESP_OK;
  eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
  eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
  emac.smi_gpio.mdc_num = BOARD_ETH_MDC;
  emac.smi_gpio.mdio_num = BOARD_ETH_MDIO;
  emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
  emac.clock_config.rmii.clock_gpio = BOARD_ETH_CLOCK;
  eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
  phy_cfg.phy_addr = BOARD_ETH_PHY_ADDR;
  phy_cfg.reset_gpio_num = BOARD_ETH_RESET;
  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_cfg);
  esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
  if (!mac || !phy) {
    if (mac)
      mac->del(mac);
    if (phy)
      phy->del(phy);
    return ESP_ERR_NO_MEM;
  }
  esp_eth_handle_t candidate = NULL;
  esp_netif_t *net = NULL;
  esp_eth_netif_glue_handle_t glue = NULL;
  bool registered = false;
  esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
  esp_err_t err = esp_eth_driver_install(&config, &candidate);
  if (err != ESP_OK)
    goto fail;
  esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
  inherent.route_prio = 150; // Wi-Fi STA defaults to 100.
  esp_netif_config_t net_cfg = {.base = &inherent,
                                .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
  net = esp_netif_new(&net_cfg);
  if (!net) {
    err = ESP_ERR_NO_MEM;
    goto fail;
  }
  glue = esp_eth_new_netif_glue(candidate);
  if (!glue) {
    err = ESP_ERR_NO_MEM;
    goto fail;
  }
  err = esp_netif_attach(net, glue);
  if (err != ESP_OK)
    goto fail;
  err = esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED,
                                   event, NULL);
  if (err != ESP_OK)
    goto fail;
  registered = true;
  err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, event, NULL);
  if (err != ESP_OK)
    goto fail;
  err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, event, NULL);
  if (err != ESP_OK)
    goto fail;
  err = esp_eth_start(candidate);
  if (err != ESP_OK)
    goto fail;
  eth = candidate;
  return ESP_OK;
fail:
  if (registered) {
    esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, event);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, event);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, event);
  }
  if (glue)
    esp_eth_del_netif_glue(glue);
  if (net)
    esp_netif_destroy(net);
  if (candidate)
    esp_eth_driver_uninstall(candidate);
  mac->del(mac);
  phy->del(phy);
  return err;
}
