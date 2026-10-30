// TCP side of the UART bridges.
//
// UART -> TCP: BridgePoll() moves the RX ring into tcp_write() as send buffer allows.
// TCP -> UART: received pbufs are copied into the TX ring; whatever does not fit is
// held and only acknowledged (tcp_recved) once copied, so a slow UART closes the
// TCP window instead of losing data.

#include <stdio.h>
#include "Bridge.h"
#include "Uart.h"
#include "Settings.h"
#include "Config.h"
#include "pico/stdlib.h"
#include "lwip/tcp.h"

typedef struct SBridge
{
	uint8_t u8Port;
	struct tcp_pcb *psListen;
	struct tcp_pcb *psClient;
	struct pbuf *psPending;				// Received but not yet fully copied to the TX ring
	uint16_t u16PendingOffset;
	uint16_t u16BoundPort;
	ip_addr_t sClientAddr;
	uint16_t u16ClientPort;
	uint64_t u64ClientSinceUs;
	uint32_t u32Connects;
	uint32_t u32NetRxBytes;				// TCP -> TX ring
	uint32_t u32NetTxBytes;				// RX ring -> TCP
	uint32_t u32Discarded;				// UART data thrown away with no client connected
	bool bRxWaiting;					// UART data is waiting to be coalesced
	uint64_t u64RxWaitingSinceUs;		// When the oldest waiting byte was first seen
} SBridge;

static SBridge sg_sBridges[UART_PORT_COUNT];

static void BridgeClientClose(SBridge *psBridge)
{
	struct tcp_pcb *psPcb = psBridge->psClient;

	if (psBridge->psPending)
	{
		pbuf_free(psBridge->psPending);
		psBridge->psPending = NULL;
		psBridge->u16PendingOffset = 0;
	}

	if (NULL == psPcb)
	{
		return;
	}

	psBridge->psClient = NULL;
	tcp_arg(psPcb, NULL);
	tcp_recv(psPcb, NULL);
	tcp_err(psPcb, NULL);

	if (ERR_OK != tcp_close(psPcb))
	{
		tcp_abort(psPcb);
	}
}

// Copy as much of the held pbuf chain into the TX ring as fits
static void BridgeDrainPending(SBridge *psBridge)
{
	SRing *psTx = &g_sUartPorts[psBridge->u8Port].sTx;

	while (psBridge->psPending)
	{
		uint8_t *pu8Span;
		uint32_t u32Span = RingWriteSpan(psTx, &pu8Span);
		uint32_t u32Left = psBridge->psPending->tot_len - psBridge->u16PendingOffset;
		uint32_t u32Copied;

		if (0 == u32Span)
		{
			break;
		}

		if (u32Span > u32Left)
		{
			u32Span = u32Left;
		}

		u32Copied = pbuf_copy_partial(psBridge->psPending, pu8Span, (u16_t) u32Span, psBridge->u16PendingOffset);
		RingProduce(psTx, u32Copied);
		psBridge->u16PendingOffset += (uint16_t) u32Copied;
		psBridge->u32NetRxBytes += u32Copied;

		if (psBridge->psClient)
		{
			tcp_recved(psBridge->psClient, (u16_t) u32Copied);
		}

		if (psBridge->u16PendingOffset >= psBridge->psPending->tot_len)
		{
			pbuf_free(psBridge->psPending);
			psBridge->psPending = NULL;
			psBridge->u16PendingOffset = 0;
		}
	}
}

static err_t BridgeRecv(void *pvArg,
                        struct tcp_pcb *psPcb,
                        struct pbuf *psPbuf,
                        err_t s8Err)
{
	SBridge *psBridge = (SBridge *) pvArg;

	if (NULL == psPbuf)
	{
		// Remote closed.  Anything already in the TX ring still goes out.
		BridgeDrainPending(psBridge);
		BridgeClientClose(psBridge);
		return(ERR_OK);
	}

	if (ERR_OK != s8Err)
	{
		pbuf_free(psPbuf);
		return(s8Err);
	}

	if (psBridge->psPending)
	{
		pbuf_cat(psBridge->psPending, psPbuf);
	}
	else
	{
		psBridge->psPending = psPbuf;
		psBridge->u16PendingOffset = 0;
	}

	BridgeDrainPending(psBridge);
	return(ERR_OK);
}

static void BridgeErr(void *pvArg,
                      err_t s8Err)
{
	SBridge *psBridge = (SBridge *) pvArg;

	// The pcb is already gone
	psBridge->psClient = NULL;
	if (psBridge->psPending)
	{
		pbuf_free(psBridge->psPending);
		psBridge->psPending = NULL;
		psBridge->u16PendingOffset = 0;
	}
}

static void TcpKeepalive(struct tcp_pcb *psPcb)
{
	ip_set_option(psPcb, SOF_KEEPALIVE);
	psPcb->keep_idle = CONFIG_KEEPALIVE_IDLE_MS;
	psPcb->keep_intvl = CONFIG_KEEPALIVE_INTERVAL_MS;
	psPcb->keep_cnt = CONFIG_KEEPALIVE_COUNT;
}

static err_t BridgeAccept(void *pvArg,
                          struct tcp_pcb *psPcb,
                          err_t s8Err)
{
	SBridge *psBridge = (SBridge *) pvArg;
	char eAddr[IPADDR_STRLEN_MAX];

	if ((ERR_OK != s8Err) || (NULL == psPcb))
	{
		return(ERR_VAL);
	}

	if (psBridge->psClient)
	{
		LogMsg("port %u: replacing client %s", psBridge->u8Port, ipaddr_ntoa(&psBridge->sClientAddr));
		BridgeClientClose(psBridge);
	}

	// Stale UART data from before this client is not theirs - unless the port keeps it
	if (g_sSettings.sPort[psBridge->u8Port].bClearOnConnect)
	{
		RingDiscard(&g_sUartPorts[psBridge->u8Port].sRx);
	}

	psBridge->psClient = psPcb;
	ip_addr_copy(psBridge->sClientAddr, psPcb->remote_ip);
	psBridge->u16ClientPort = psPcb->remote_port;
	psBridge->u64ClientSinceUs = time_us_64();
	psBridge->u32Connects++;

	tcp_arg(psPcb, psBridge);
	tcp_recv(psPcb, BridgeRecv);
	tcp_err(psPcb, BridgeErr);
	tcp_nagle_disable(psPcb);
	TcpKeepalive(psPcb);

	ipaddr_ntoa_r(&psBridge->sClientAddr, eAddr, sizeof(eAddr));
	LogMsg("port %u: client %s:%u connected", psBridge->u8Port, eAddr, psBridge->u16ClientPort);
	return(ERR_OK);
}

static void BridgeListenClose(SBridge *psBridge)
{
	if (psBridge->psListen)
	{
		tcp_arg(psBridge->psListen, NULL);
		tcp_accept(psBridge->psListen, NULL);
		(void) tcp_close(psBridge->psListen);
		psBridge->psListen = NULL;
	}

	psBridge->u16BoundPort = 0;
}

static EStatus BridgeListen(SBridge *psBridge,
                            uint16_t u16Port)
{
	struct tcp_pcb *psPcb;
	struct tcp_pcb *psListen;

	psPcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
	if (NULL == psPcb)
	{
		return(ESTATUS_NO_MEMORY);
	}

	ip_set_option(psPcb, SOF_REUSEADDR);
	if (ERR_OK != tcp_bind(psPcb, IP_ANY_TYPE, u16Port))
	{
		tcp_abort(psPcb);
		return(ESTATUS_BUSY);
	}

	psListen = tcp_listen_with_backlog(psPcb, 1);
	if (NULL == psListen)
	{
		tcp_abort(psPcb);
		return(ESTATUS_NO_MEMORY);
	}

	psBridge->psListen = psListen;
	psBridge->u16BoundPort = u16Port;
	tcp_arg(psListen, psBridge);
	tcp_accept(psListen, BridgeAccept);
	return(ESTATUS_OK);
}

// Match the listener to the port's current tcpport setting
void BridgeApplyPort(uint8_t u8Port)
{
	SBridge *psBridge = &sg_sBridges[u8Port];
	uint16_t u16Port = g_sSettings.sPort[u8Port].u16TcpPort;
	EStatus eStatus;

	if ((u16Port == psBridge->u16BoundPort) && (psBridge->psListen))
	{
		return;
	}

	if (psBridge->psClient)
	{
		LogMsg("port %u: TCP port changed, dropping client", u8Port);
	}

	BridgeClientClose(psBridge);
	BridgeListenClose(psBridge);

	if (0 == u16Port)
	{
		return;
	}

	eStatus = BridgeListen(psBridge, u16Port);
	if (ESTATUS_OK != eStatus)
	{
		LogErr("port %u: cannot listen on TCP %u (%s)", u8Port, u16Port, StatusText(eStatus));
	}
}

void BridgeDisconnect(uint8_t u8Port)
{
	BridgeClientClose(&sg_sBridges[u8Port]);
}

void BridgeStatsClear(uint8_t u8Port)
{
	SBridge *psBridge = &sg_sBridges[u8Port];

	psBridge->u32Connects = 0;
	psBridge->u32NetRxBytes = 0;
	psBridge->u32NetTxBytes = 0;
	psBridge->u32Discarded = 0;
}

uint32_t BridgeClientCount(void)
{
	uint32_t u32Count = 0;
	uint32_t u32Loop;

	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		if (sg_sBridges[u32Loop].psClient)
		{
			u32Count++;
		}
	}

	return(u32Count);
}

void BridgeStatsPrint(uint8_t u8Port,
                      SConsoleSession *psSession)
{
	SBridge *psBridge = &sg_sBridges[u8Port];
	char eAddr[IPADDR_STRLEN_MAX];
	char eSince[24];

	if (0 == psBridge->u16BoundPort)
	{
		ConsolePrintf(psSession, "TCP          not listening\n");
	}
	else
	if (psBridge->psClient)
	{
		ipaddr_ntoa_r(&psBridge->sClientAddr, eAddr, sizeof(eAddr));
		FormatDuration(time_us_64() - psBridge->u64ClientSinceUs, eSince, sizeof(eSince));
		ConsolePrintf(psSession, "TCP          port %u, client %s:%u for %s\n",
					  psBridge->u16BoundPort, eAddr, psBridge->u16ClientPort, eSince);
	}
	else
	{
		ConsolePrintf(psSession, "TCP          port %u, no client\n", psBridge->u16BoundPort);
	}

	ConsolePrintf(psSession, "Connects     %lu\n", (unsigned long) psBridge->u32Connects);
	ConsolePrintf(psSession, "Net rx       %lu bytes\n", (unsigned long) psBridge->u32NetRxBytes);
	ConsolePrintf(psSession, "Net tx       %lu bytes\n", (unsigned long) psBridge->u32NetTxBytes);
	ConsolePrintf(psSession, "Discarded    %lu bytes (no client)\n", (unsigned long) psBridge->u32Discarded);
}

// Pump both directions for every port
void BridgePoll(void)
{
	uint32_t u32Loop;

	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		SBridge *psBridge = &sg_sBridges[u32Loop];
		SRing *psRx = &g_sUartPorts[u32Loop].sRx;
		bool bWrote = false;
		uint32_t u32Used;
		uint32_t u32Limit;
		uint64_t u64Now;

		// Held network data waiting for TX ring space
		BridgeDrainPending(psBridge);

		if (NULL == psBridge->psClient)
		{
			uint32_t u32Used = RingUsed(psRx);

			// clearonconnect off: leave it queued for the next client.  Once the
			// ring fills, RTS/CTS holds the device off; otherwise new bytes drop.
			if ((u32Used) && (g_sSettings.sPort[u32Loop].bClearOnConnect))
			{
				psBridge->u32Discarded += u32Used;
				RingConsume(psRx, u32Used);
			}

			continue;
		}

		// UART -> TCP, coalesced:
		//   nothing in flight            -> send now (interactive latency)
		//   data in flight               -> send whole segments only, until the
		//                                   oldest waiting byte is CONFIG_BRIDGE_COALESCE_US old
		// Tiny segments would otherwise exhaust TCP_SND_QUEUELEN and stall on the
		// peer's delayed ACK.
		u32Used = RingUsed(psRx);
		if (0 == u32Used)
		{
			psBridge->bRxWaiting = false;
			continue;
		}

		u64Now = time_us_64();
		if (false == psBridge->bRxWaiting)
		{
			psBridge->bRxWaiting = true;
			psBridge->u64RxWaitingSinceUs = u64Now;
		}

		u32Limit = u32Used;
		if (((psBridge->psClient->unacked) || (psBridge->psClient->unsent)) &&
			((u64Now - psBridge->u64RxWaitingSinceUs) < CONFIG_BRIDGE_COALESCE_US))
		{
			u32Limit = (u32Used / TCP_MSS) * TCP_MSS;
		}

		while (u32Limit)
		{
			uint8_t *pu8Span;
			uint32_t u32Span = RingReadSpan(psRx, &pu8Span);
			uint32_t u32Room = tcp_sndbuf(psBridge->psClient);

			if ((0 == u32Span) || (0 == u32Room))
			{
				break;
			}

			if (u32Span > u32Room)
			{
				u32Span = u32Room;
			}

			if (u32Span > u32Limit)
			{
				u32Span = u32Limit;
			}

			if (ERR_OK != tcp_write(psBridge->psClient, pu8Span, (u16_t) u32Span, TCP_WRITE_FLAG_COPY))
			{
				// Out of segments/heap - try again next pass
				break;
			}

			RingConsume(psRx, u32Span);
			psBridge->u32NetTxBytes += u32Span;
			u32Limit -= u32Span;
			bWrote = true;
		}

		if (bWrote)
		{
			// Anything left over starts a fresh coalescing window
			psBridge->bRxWaiting = false;
			(void) tcp_output(psBridge->psClient);
		}
	}
}

EStatus BridgeInit(void)
{
	uint32_t u32Loop;

	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		sg_sBridges[u32Loop].u8Port = (uint8_t) u32Loop;
		BridgeApplyPort((uint8_t) u32Loop);
	}

	return(ESTATUS_OK);
}
