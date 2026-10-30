// USB descriptors: one CDC ACM interface ("UartBridge Console")

#include "tusb.h"
#include "pico/unique_id.h"

#define USB_VID					0x2e8a		// Raspberry Pi
#define USB_PID					0x0009		// Pico SDK CDC
#define USB_BCD					0x0200

#define USB_ITF_CDC				0
#define USB_ITF_CDC_DATA		1
#define USB_ITF_COUNT			2

#define USB_EP_CDC_NOTIFY		0x81
#define USB_EP_CDC_OUT			0x02
#define USB_EP_CDC_IN			0x82
#define USB_CDC_NOTIFY_SIZE		8
#define USB_CDC_DATA_SIZE		64

#define USB_CONFIG_LENGTH		(TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

#define USB_STR_LANGUAGE		0
#define USB_STR_MANUFACTURER	1
#define USB_STR_PRODUCT			2
#define USB_STR_SERIAL			3
#define USB_STR_CDC				4

static const tusb_desc_device_t sg_sDeviceDescriptor =
{
	.bLength			= sizeof(tusb_desc_device_t),
	.bDescriptorType	= TUSB_DESC_DEVICE,
	.bcdUSB				= USB_BCD,
	.bDeviceClass		= TUSB_CLASS_MISC,
	.bDeviceSubClass	= MISC_SUBCLASS_COMMON,
	.bDeviceProtocol	= MISC_PROTOCOL_IAD,
	.bMaxPacketSize0	= CFG_TUD_ENDPOINT0_SIZE,
	.idVendor			= USB_VID,
	.idProduct			= USB_PID,
	.bcdDevice			= 0x0100,
	.iManufacturer		= USB_STR_MANUFACTURER,
	.iProduct			= USB_STR_PRODUCT,
	.iSerialNumber		= USB_STR_SERIAL,
	.bNumConfigurations	= 1,
};

static const uint8_t sg_u8ConfigDescriptor[] =
{
	TUD_CONFIG_DESCRIPTOR(1, USB_ITF_COUNT, 0, USB_CONFIG_LENGTH, 0, 250),
	TUD_CDC_DESCRIPTOR(USB_ITF_CDC, USB_STR_CDC, USB_EP_CDC_NOTIFY, USB_CDC_NOTIFY_SIZE,
					   USB_EP_CDC_OUT, USB_EP_CDC_IN, USB_CDC_DATA_SIZE),
};

static const char *sg_peStrings[] =
{
	NULL,							// Language - handled separately
	"Raspberry Pi",
	"UartBridge",
	NULL,							// Serial - board unique ID
	"UartBridge Console",
};

static uint16_t sg_u16StringBuffer[32];

const uint8_t *tud_descriptor_device_cb(void)
{
	return((const uint8_t *) &sg_sDeviceDescriptor);
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t u8Index)
{
	(void) u8Index;
	return(sg_u8ConfigDescriptor);
}

const uint16_t *tud_descriptor_string_cb(uint8_t u8Index,
                                         uint16_t u16LangId)
{
	char eSerial[(2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES) + 1];
	const char *peText;
	uint32_t u32Length;
	uint32_t u32Loop;

	(void) u16LangId;

	if (USB_STR_LANGUAGE == u8Index)
	{
		sg_u16StringBuffer[1] = 0x0409;
		u32Length = 1;
	}
	else
	{
		if (u8Index >= (sizeof(sg_peStrings) / sizeof(sg_peStrings[0])))
		{
			return(NULL);
		}

		if (USB_STR_SERIAL == u8Index)
		{
			pico_get_unique_board_id_string(eSerial, sizeof(eSerial));
			peText = eSerial;
		}
		else
		{
			peText = sg_peStrings[u8Index];
		}

		u32Length = strlen(peText);
		if (u32Length > ((sizeof(sg_u16StringBuffer) / sizeof(sg_u16StringBuffer[0])) - 1))
		{
			u32Length = (sizeof(sg_u16StringBuffer) / sizeof(sg_u16StringBuffer[0])) - 1;
		}

		for (u32Loop = 0; u32Loop < u32Length; u32Loop++)
		{
			sg_u16StringBuffer[1 + u32Loop] = (uint16_t) peText[u32Loop];
		}
	}

	sg_u16StringBuffer[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | ((2 * u32Length) + 2));
	return(sg_u16StringBuffer);
}
