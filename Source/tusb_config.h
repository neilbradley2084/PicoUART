#pragma once

// TinyUSB configuration - single CDC interface for the configuration console

#define CFG_TUD_ENABLED					1
#define CFG_TUSB_RHPORT0_MODE			OPT_MODE_DEVICE
#define CFG_TUD_ENDPOINT0_SIZE			64

#define CFG_TUD_CDC						1
#define CFG_TUD_MSC						0
#define CFG_TUD_HID						0
#define CFG_TUD_MIDI					0
#define CFG_TUD_VENDOR					0

#define CFG_TUD_CDC_RX_BUFSIZE			256
#define CFG_TUD_CDC_TX_BUFSIZE			1024
#define CFG_TUD_CDC_EP_BUFSIZE			64
