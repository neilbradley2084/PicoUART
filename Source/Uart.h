#pragma once

// Hardware UART ports.  Core 1 owns the UART registers and moves bytes between
// the FIFOs and the ring buffers; core 0 stages configuration and statistics.

#include "Shared/Shared.h"
#include "Ring.h"
#include "hardware/structs/uart.h"

#define UART_PORT_COUNT		2

typedef enum EParity
{
	EPARITY_NONE = 0,
	EPARITY_EVEN,
	EPARITY_ODD,
	EPARITY_MARK,
	EPARITY_SPACE,
} EParity;

typedef enum EFlow
{
	EFLOW_NONE = 0,
	EFLOW_RTSCTS,
} EFlow;

// Line settings for one port
typedef struct SUartLine
{
	uint32_t u32Baud;
	uint8_t u8DataBits;			// 5-8
	uint8_t u8StopBits;			// 1 or 2
	EParity eParity;
	EFlow eFlow;
} SUartLine;

typedef struct SUartStats
{
	uint32_t u32RxBytes;		// UART -> ring
	uint32_t u32TxBytes;		// Ring -> UART
	uint32_t u32RxDropped;		// Arrived with the ring full and no flow control
	uint32_t u32Overrun;
	uint32_t u32Framing;
	uint32_t u32Parity;
	uint32_t u32Break;
} SUartStats;

typedef struct SUartPort
{
	uart_hw_t *psHw;
	SRing sRx;							// UART -> network.  Core 1 produces, core 0 consumes.
	SRing sTx;							// Network -> UART.  Core 0 produces, core 1 consumes.

	// Register image staged by core 0, applied by core 1
	volatile uint32_t u32Ibrd;
	volatile uint32_t u32Fbrd;
	volatile uint32_t u32Lcrh;
	volatile uint32_t u32Cr;
	volatile uint32_t u32ConfigSeq;		// Bumped by core 0 after the image is written
	volatile uint32_t u32AppliedSeq;	// Last image core 1 loaded

	volatile SUartStats sStats;			// Written by core 1 only
	SUartStats sBaseline;				// Core 0 snapshot taken by UartStatsClear()
	uint32_t u32ActualBaud;
} SUartPort;

// Pins for a port
typedef struct SUartPins
{
	uint8_t u8Tx;
	uint8_t u8Rx;
	uint8_t u8Cts;
	uint8_t u8Rts;
} SUartPins;

extern SUartPort g_sUartPorts[UART_PORT_COUNT];
extern const SUartPins g_sUartPins[UART_PORT_COUNT];

// Supported baud rates, 0 terminated
extern const uint32_t g_u32UartBaudRates[];

extern EStatus UartInit(void);
extern EStatus UartStart(void);
extern void UartCore1Launch(void);
extern void UartConfigure(uint8_t u8Port,
                          const SUartLine *psLine);
extern bool UartBaudValid(uint32_t u32Baud);
extern void UartStatsGet(uint8_t u8Port,
                         SUartStats *psStats);
extern void UartStatsClear(uint8_t u8Port);
extern bool UartCtsAsserted(uint8_t u8Port);
extern uint32_t UartActivityCount(void);
extern uint32_t UartHeapRemaining(void);
