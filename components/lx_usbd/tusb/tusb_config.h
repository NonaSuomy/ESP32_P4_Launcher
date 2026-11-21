/* TinyUSB for the launcher's USB device port (lx_usbd): one HID interface on the ESP32-P4's full-speed OTG
 * controller (TinyUSB's port 0), in device mode. */
#pragma once
#define CFG_TUSB_OS              OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH     freertos/
#define CFG_TUSB_DEBUG           0
#define CFG_TUD_ENABLED          1
#define CFG_TUSB_RHPORT0_MODE    (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENDPOINT0_SIZE   64
#define CFG_TUSB_MEM_ALIGN       __attribute__((aligned(4)))
#define CFG_TUD_HID              1
#define CFG_TUD_HID_EP_BUFSIZE   16
#define CFG_TUD_CDC              0
#define CFG_TUD_MSC              0
#define CFG_TUD_MIDI             0
#define CFG_TUD_VENDOR           0
#define CFG_TUD_AUDIO            0
#define CFG_TUD_VIDEO            0
#define CFG_TUD_ECM_RNDIS        0
#define CFG_TUD_NCM              0
#define CFG_TUD_DFU              0
#define CFG_TUD_DFU_RUNTIME      0
#define CFG_TUD_BTH              0
#define CFG_TUD_USBTMC           0
#define CFG_TUD_PRINTER          0
#define CFG_TUD_MTP              0
