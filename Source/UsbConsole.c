// USB CDC console.  TinyUSB runs from the main loop (tud_task), not SDK stdio.

#include "UsbConsole.h"
#include "Console.h"
#include "tusb.h"

static SConsoleSession *sg_psSession;
static bool sg_bConnected;

bool UsbConsoleConnected(void)
{
	return(sg_bConnected);
}

void UsbConsolePoll(void)
{
	bool bConnected;

	if (NULL == sg_psSession)
	{
		return;
	}

	tud_task();

	bConnected = tud_cdc_connected();
	if (bConnected != sg_bConnected)
	{
		sg_bConnected = bConnected;
		if (bConnected)
		{
			// Terminal just opened (DTR) - start clean
			RingDiscard(&sg_psSession->sOutput);
			sg_psSession->u32LineLength = 0;
			ConsoleBanner(sg_psSession);
			ConsolePrompt(sg_psSession);
		}
	}

	if (false == bConnected)
	{
		// Nobody listening - don't let output pile up
		RingDiscard(&sg_psSession->sOutput);
		if (tud_cdc_available())
		{
			tud_cdc_read_flush();
		}

		return;
	}

	while (tud_cdc_available())
	{
		uint8_t u8Buffer[64];
		uint32_t u32Count = tud_cdc_read(u8Buffer, sizeof(u8Buffer));
		uint32_t u32Loop;

		for (u32Loop = 0; u32Loop < u32Count; u32Loop++)
		{
			ConsoleInput(sg_psSession, u8Buffer[u32Loop]);
		}
	}

	for (;;)
	{
		uint8_t *pu8Span;
		uint32_t u32Span = RingReadSpan(&sg_psSession->sOutput, &pu8Span);
		uint32_t u32Room = tud_cdc_write_available();

		if ((0 == u32Span) || (0 == u32Room))
		{
			break;
		}

		if (u32Span > u32Room)
		{
			u32Span = u32Room;
		}

		u32Span = tud_cdc_write(pu8Span, u32Span);
		RingConsume(&sg_psSession->sOutput, u32Span);
	}

	(void) tud_cdc_write_flush();
}

EStatus UsbConsoleInit(void)
{
	EStatus eStatus;

	eStatus = ConsoleSessionCreate(ECONSOLETYPE_USB, NULL, &sg_psSession);
	if (ESTATUS_OK != eStatus)
	{
		return(eStatus);
	}

	if (false == tusb_init())
	{
		return(ESTATUS_HARDWARE);
	}

	return(ESTATUS_OK);
}
