// UART engine.
//
// Core 1 runs UartCore1Main() from SRAM forever, polling both PL011s:
//   RX FIFO -> sRx ring, sTx ring -> TX FIFO, and loading new register images.
// Nothing on core 1 touches flash, so core 0 can erase/program the settings
// sector without stopping the UARTs.
//
// With RTS/CTS enabled, a full sRx ring leaves bytes in the RX FIFO; the PL011
// then deasserts RTS on its own.

#include <unistd.h>
#include "Uart.h"
#include "Config.h"
#include "Power.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

SUartPort g_sUartPorts[UART_PORT_COUNT];

const SUartPins g_sUartPins[UART_PORT_COUNT] =
{
	{ CONFIG_UART0_PIN_TX, CONFIG_UART0_PIN_RX, CONFIG_UART0_PIN_CTS, CONFIG_UART0_PIN_RTS },
	{ CONFIG_UART1_PIN_TX, CONFIG_UART1_PIN_RX, CONFIG_UART1_PIN_CTS, CONFIG_UART1_PIN_RTS },
};

const uint32_t g_u32UartBaudRates[] =
{
	110, 300, 600, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 56000, 57600,
	115200, 128000, 230400, 256000, 460800, 921600, 1000000, 1152000,
	0
};

// Data register error flags
#define UART_DR_ERROR_BITS	(UART_UARTDR_OE_BITS | UART_UARTDR_BE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_FE_BITS)

// Service one port.  RAM resident - called only from core 1.
static void __not_in_flash_func(UartService)(SUartPort *psPort)
{
	uart_hw_t *psHw = psPort->psHw;
	uint32_t u32Seq = psPort->u32ConfigSeq;

	// New register image from core 0
	if (u32Seq != psPort->u32AppliedSeq)
	{
		__dmb();
		psHw->cr = 0;
		psHw->ibrd = psPort->u32Ibrd;
		psHw->fbrd = psPort->u32Fbrd;
		psHw->lcr_h = psPort->u32Lcrh;		// Latches ibrd/fbrd
		psHw->cr = psPort->u32Cr;
		psPort->u32AppliedSeq = u32Seq;
	}

	// RX FIFO -> ring
	while (0 == (psHw->fr & UART_UARTFR_RXFE_BITS))
	{
		uint32_t u32Data;

		if (0 == RingFree(&psPort->sRx))
		{
			if (psHw->cr & UART_UARTCR_RTSEN_BITS)
			{
				// Leave it in the FIFO - hardware drops RTS when it fills
				break;
			}

			u32Data = psHw->dr;
			psPort->sStats.u32RxDropped++;
			continue;
		}

		u32Data = psHw->dr;

		if (u32Data & UART_DR_ERROR_BITS)
		{
			if (u32Data & UART_UARTDR_OE_BITS)
			{
				psPort->sStats.u32Overrun++;
			}

			if (u32Data & UART_UARTDR_FE_BITS)
			{
				psPort->sStats.u32Framing++;
			}

			if (u32Data & UART_UARTDR_PE_BITS)
			{
				psPort->sStats.u32Parity++;
			}

			if (u32Data & UART_UARTDR_BE_BITS)
			{
				// A break carries no data byte
				psPort->sStats.u32Break++;
				continue;
			}
		}

		(void) RingPut(&psPort->sRx, (uint8_t) u32Data);
		psPort->sStats.u32RxBytes++;
	}

	// Ring -> TX FIFO
	while (0 == (psHw->fr & UART_UARTFR_TXFF_BITS))
	{
		uint8_t u8Data;

		if (false == RingGet(&psPort->sTx, &u8Data))
		{
			break;
		}

		psHw->dr = u8Data;
		psPort->sStats.u32TxBytes++;
	}
}

// Set by UartStart() once the ring buffers exist
static volatile bool sg_bUartsReady;

// Core 1 entry point.  Never returns.  Starts early (UartCore1Launch) so the
// ATX PWR_OK guard covers our whole boot; the UARTs join once UartStart() runs.
static void __not_in_flash_func(UartCore1Main)(void)
{
	for (;;)
	{
		if (sg_bUartsReady)
		{
			UartService(&g_sUartPorts[0]);
			UartService(&g_sUartPorts[1]);
		}

		PowerCore1Service();
	}
}

// Start core 1.  Call straight after PowerInit().
void UartCore1Launch(void)
{
	multicore_launch_core1(UartCore1Main);
}

// Stage a new line configuration.  Takes effect on core 1's next pass.
void UartConfigure(uint8_t u8Port,
                   const SUartLine *psLine)
{
	SUartPort *psPort = &g_sUartPorts[u8Port];
	uint32_t u32Clock = clock_get_hz(clk_peri);
	uint32_t u32Div;
	uint32_t u32Ibrd;
	uint32_t u32Fbrd;
	uint32_t u32Lcrh;
	uint32_t u32Cr;

	// Same rounding as the SDK's uart_set_baudrate(): 16.6 fixed point divisor
	u32Div = ((8 * u32Clock) / psLine->u32Baud) + 1;
	u32Ibrd = u32Div >> 7;
	if (0 == u32Ibrd)
	{
		u32Ibrd = 1;
		u32Fbrd = 0;
	}
	else
	if (u32Ibrd >= 65535)
	{
		u32Ibrd = 65535;
		u32Fbrd = 0;
	}
	else
	{
		u32Fbrd = (u32Div & 0x7f) >> 1;
	}

	psPort->u32ActualBaud = (4 * u32Clock) / ((64 * u32Ibrd) + u32Fbrd);

	u32Lcrh = UART_UARTLCR_H_FEN_BITS |
			  (((uint32_t) psLine->u8DataBits - 5) << UART_UARTLCR_H_WLEN_LSB);

	if (2 == psLine->u8StopBits)
	{
		u32Lcrh |= UART_UARTLCR_H_STP2_BITS;
	}

	if (EPARITY_EVEN == psLine->eParity)
	{
		u32Lcrh |= UART_UARTLCR_H_PEN_BITS | UART_UARTLCR_H_EPS_BITS;
	}
	else
	if (EPARITY_ODD == psLine->eParity)
	{
		u32Lcrh |= UART_UARTLCR_H_PEN_BITS;
	}
	else
	if (EPARITY_MARK == psLine->eParity)
	{
		// Stick parity, EPS clear -> parity bit always 1
		u32Lcrh |= UART_UARTLCR_H_PEN_BITS | UART_UARTLCR_H_SPS_BITS;
	}
	else
	if (EPARITY_SPACE == psLine->eParity)
	{
		// Stick parity, EPS set -> parity bit always 0
		u32Lcrh |= UART_UARTLCR_H_PEN_BITS | UART_UARTLCR_H_SPS_BITS | UART_UARTLCR_H_EPS_BITS;
	}

	u32Cr = UART_UARTCR_UARTEN_BITS | UART_UARTCR_TXE_BITS | UART_UARTCR_RXE_BITS;
	if (EFLOW_RTSCTS == psLine->eFlow)
	{
		u32Cr |= UART_UARTCR_RTSEN_BITS | UART_UARTCR_CTSEN_BITS;
	}
	else
	{
		// No flow control - hold RTS asserted
		u32Cr |= UART_UARTCR_RTS_BITS;
	}

	psPort->u32Ibrd = u32Ibrd;
	psPort->u32Fbrd = u32Fbrd;
	psPort->u32Lcrh = u32Lcrh;
	psPort->u32Cr = u32Cr;
	__dmb();
	psPort->u32ConfigSeq++;
}

bool UartBaudValid(uint32_t u32Baud)
{
	uint32_t u32Loop;

	for (u32Loop = 0; g_u32UartBaudRates[u32Loop]; u32Loop++)
	{
		if (u32Baud == g_u32UartBaudRates[u32Loop])
		{
			return(true);
		}
	}

	return(false);
}

// Statistics since the last UartStatsClear()
void UartStatsGet(uint8_t u8Port,
                  SUartStats *psStats)
{
	SUartPort *psPort = &g_sUartPorts[u8Port];

	psStats->u32RxBytes = psPort->sStats.u32RxBytes - psPort->sBaseline.u32RxBytes;
	psStats->u32TxBytes = psPort->sStats.u32TxBytes - psPort->sBaseline.u32TxBytes;
	psStats->u32RxDropped = psPort->sStats.u32RxDropped - psPort->sBaseline.u32RxDropped;
	psStats->u32Overrun = psPort->sStats.u32Overrun - psPort->sBaseline.u32Overrun;
	psStats->u32Framing = psPort->sStats.u32Framing - psPort->sBaseline.u32Framing;
	psStats->u32Parity = psPort->sStats.u32Parity - psPort->sBaseline.u32Parity;
	psStats->u32Break = psPort->sStats.u32Break - psPort->sBaseline.u32Break;
}

// Counters belong to core 1, so clearing just moves core 0's baseline
void UartStatsClear(uint8_t u8Port)
{
	SUartPort *psPort = &g_sUartPorts[u8Port];

	psPort->sBaseline.u32RxBytes = psPort->sStats.u32RxBytes;
	psPort->sBaseline.u32TxBytes = psPort->sStats.u32TxBytes;
	psPort->sBaseline.u32RxDropped = psPort->sStats.u32RxDropped;
	psPort->sBaseline.u32Overrun = psPort->sStats.u32Overrun;
	psPort->sBaseline.u32Framing = psPort->sStats.u32Framing;
	psPort->sBaseline.u32Parity = psPort->sStats.u32Parity;
	psPort->sBaseline.u32Break = psPort->sStats.u32Break;
}

// CTS input is active low; FR.CTS reads 1 when asserted
bool UartCtsAsserted(uint8_t u8Port)
{
	if (g_sUartPorts[u8Port].psHw->fr & UART_UARTFR_CTS_BITS)
	{
		return(true);
	}

	return(false);
}

// Running total of bytes moved on all ports, both directions.  Changes whenever data flows.
uint32_t UartActivityCount(void)
{
	uint32_t u32Count = 0;
	uint32_t u32Loop;

	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		u32Count += g_sUartPorts[u32Loop].sStats.u32RxBytes;
		u32Count += g_sUartPorts[u32Loop].sStats.u32TxBytes;
	}

	return(u32Count);
}

// Heap not yet claimed by sbrk
uint32_t UartHeapRemaining(void)
{
	return((uint32_t) ((uintptr_t) &__HeapLimit - (uintptr_t) sbrk(0)));
}

// Clocks, pins and initial register images.  Core 1 is not running yet.
EStatus UartInit(void)
{
	uint32_t u32Loop;

	// clk_peri from the 48 MHz USB PLL on both RP2040 and RP2350: 110 baud fits the
	// 16-bit integer divisor and every supported rate is within 0.2%
	clock_configure_undivided(clk_peri,
							  0,
							  CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
							  clock_get_hz(clk_usb));

	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		uart_inst_t *psUart = uart_get_instance(u32Loop);
		const SUartPins *psPins = &g_sUartPins[u32Loop];

		// Takes the block out of reset; the real configuration comes from core 1
		(void) uart_init(psUart, 115200);
		uart_get_hw(psUart)->cr = 0;
		g_sUartPorts[u32Loop].psHw = uart_get_hw(psUart);

		gpio_set_function(psPins->u8Tx, GPIO_FUNC_UART);
		gpio_set_function(psPins->u8Rx, GPIO_FUNC_UART);
		gpio_set_function(psPins->u8Cts, GPIO_FUNC_UART);
		gpio_set_function(psPins->u8Rts, GPIO_FUNC_UART);
		gpio_pull_up(psPins->u8Rx);
	}

	return(ESTATUS_OK);
}

// Split what is left of the heap (less CONFIG_HEAP_RESERVE) evenly across the
// four rings, then let core 1 service the UARTs.  Call last, after everything else has allocated.
EStatus UartStart(void)
{
	SRing *psRings[UART_PORT_COUNT * 2];
	uint32_t u32Each;
	uint32_t u32Loop;
	uint32_t u32Free = UartHeapRemaining();

	if (u32Free <= CONFIG_HEAP_RESERVE)
	{
		return(ESTATUS_NO_MEMORY);
	}

	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		psRings[(u32Loop * 2) + 0] = &g_sUartPorts[u32Loop].sRx;
		psRings[(u32Loop * 2) + 1] = &g_sUartPorts[u32Loop].sTx;
	}

	u32Each = ((u32Free - CONFIG_HEAP_RESERVE) / (UART_PORT_COUNT * 2)) & ~((uint32_t) 3);

	for (u32Loop = 0; u32Loop < (UART_PORT_COUNT * 2); u32Loop++)
	{
		uint8_t *pu8Buffer = NULL;

		// Allocator overhead is unknown, so back off until it fits
		while (u32Each > 1024)
		{
			MEMALLOC_NO_CLEAR(pu8Buffer, u32Each);
			if (pu8Buffer)
			{
				break;
			}

			u32Each -= 1024;
		}

		if (NULL == pu8Buffer)
		{
			return(ESTATUS_NO_MEMORY);
		}

		psRings[u32Loop]->pu8Buffer = pu8Buffer;
		psRings[u32Loop]->u32Size = u32Each;
		psRings[u32Loop]->u32Head = 0;
		psRings[u32Loop]->u32Tail = 0;
	}

	__dmb();
	sg_bUartsReady = true;
	return(ESTATUS_OK);
}
