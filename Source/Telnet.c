// Telnet server.  One console session per connection, no limit beyond memory.

#include "Telnet.h"
#include "Console.h"
#include "Config.h"
#include "lwip/tcp.h"

#define TELNET_IAC			255
#define TELNET_DONT			254
#define TELNET_DO			253
#define TELNET_WONT			252
#define TELNET_WILL			251
#define TELNET_SB			250
#define TELNET_SE			240
#define TELNET_OPT_ECHO		1
#define TELNET_OPT_SGA		3

// IAC parser states
#define TELNET_STATE_DATA	0
#define TELNET_STATE_IAC	1
#define TELNET_STATE_OPTION	2
#define TELNET_STATE_SB		3
#define TELNET_STATE_SB_IAC	4

static struct tcp_pcb *sg_psListen;

// We echo and run character at a time
static const uint8_t sg_u8Negotiate[] =
{
	TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO,
	TELNET_IAC, TELNET_WILL, TELNET_OPT_SGA,
	TELNET_IAC, TELNET_DO, TELNET_OPT_SGA,
};

static void TelnetByte(SConsoleSession *psSession,
                       uint8_t u8Data)
{
	if (TELNET_STATE_DATA == psSession->u8Telnet)
	{
		if (TELNET_IAC == u8Data)
		{
			psSession->u8Telnet = TELNET_STATE_IAC;
			return;
		}

		ConsoleInput(psSession, u8Data);
	}
	else
	if (TELNET_STATE_IAC == psSession->u8Telnet)
	{
		if ((u8Data >= TELNET_WILL) && (u8Data <= TELNET_DONT))
		{
			psSession->u8Telnet = TELNET_STATE_OPTION;
		}
		else
		if (TELNET_SB == u8Data)
		{
			psSession->u8Telnet = TELNET_STATE_SB;
		}
		else
		{
			// IAC IAC (literal 255) or a two byte command - nothing we use
			psSession->u8Telnet = TELNET_STATE_DATA;
		}
	}
	else
	if (TELNET_STATE_OPTION == psSession->u8Telnet)
	{
		psSession->u8Telnet = TELNET_STATE_DATA;
	}
	else
	if (TELNET_STATE_SB == psSession->u8Telnet)
	{
		if (TELNET_IAC == u8Data)
		{
			psSession->u8Telnet = TELNET_STATE_SB_IAC;
		}
	}
	else
	{
		psSession->u8Telnet = (TELNET_SE == u8Data) ? TELNET_STATE_DATA : TELNET_STATE_SB;
	}
}

static void TelnetDetach(SConsoleSession *psSession)
{
	struct tcp_pcb *psPcb = (struct tcp_pcb *) psSession->pvTransport;

	if (psSession->pvPendingInput)
	{
		pbuf_free((struct pbuf *) psSession->pvPendingInput);
		psSession->pvPendingInput = NULL;
	}

	psSession->pvTransport = NULL;
	if (psPcb)
	{
		tcp_arg(psPcb, NULL);
		tcp_recv(psPcb, NULL);
		tcp_err(psPcb, NULL);
		if (ERR_OK != tcp_close(psPcb))
		{
			tcp_abort(psPcb);
		}
	}
}

static err_t TelnetRecv(void *pvArg,
                        struct tcp_pcb *psPcb,
                        struct pbuf *psPbuf,
                        err_t s8Err)
{
	SConsoleSession *psSession = (SConsoleSession *) pvArg;

	if (NULL == psPbuf)
	{
		// Remote closed - TelnetPoll() frees the session
		TelnetDetach(psSession);
		psSession->bClosing = true;
		return(ERR_OK);
	}

	if (ERR_OK != s8Err)
	{
		pbuf_free(psPbuf);
		return(s8Err);
	}

	// Commands can touch the radio, so they don't run inside the cyw43 poll.
	// Queue the data; TelnetPoll() feeds it to the console.
	if (psSession->pvPendingInput)
	{
		pbuf_cat((struct pbuf *) psSession->pvPendingInput, psPbuf);
	}
	else
	{
		psSession->pvPendingInput = psPbuf;
	}

	return(ERR_OK);
}

// Feed queued input to the console - main loop context
static void TelnetProcessInput(SConsoleSession *psSession)
{
	struct pbuf *psPbuf = (struct pbuf *) psSession->pvPendingInput;
	struct pbuf *psScan;
	uint16_t u16Length;

	if (NULL == psPbuf)
	{
		return;
	}

	psSession->pvPendingInput = NULL;
	u16Length = psPbuf->tot_len;

	for (psScan = psPbuf; psScan; psScan = psScan->next)
	{
		const uint8_t *pu8Data = (const uint8_t *) psScan->payload;
		uint32_t u32Loop;

		for (u32Loop = 0; u32Loop < psScan->len; u32Loop++)
		{
			TelnetByte(psSession, pu8Data[u32Loop]);
		}
	}

	pbuf_free(psPbuf);

	if (psSession->pvTransport)
	{
		tcp_recved((struct tcp_pcb *) psSession->pvTransport, u16Length);
	}
}

static void TelnetErr(void *pvArg,
                      err_t s8Err)
{
	SConsoleSession *psSession = (SConsoleSession *) pvArg;

	// The pcb is already freed
	psSession->pvTransport = NULL;
	psSession->bClosing = true;
	if (psSession->pvPendingInput)
	{
		pbuf_free((struct pbuf *) psSession->pvPendingInput);
		psSession->pvPendingInput = NULL;
	}
}

static err_t TelnetAccept(void *pvArg,
                          struct tcp_pcb *psPcb,
                          err_t s8Err)
{
	SConsoleSession *psSession;

	if ((ERR_OK != s8Err) || (NULL == psPcb))
	{
		return(ERR_VAL);
	}

	if (ESTATUS_OK != ConsoleSessionCreate(ECONSOLETYPE_TELNET, psPcb, &psSession))
	{
		tcp_abort(psPcb);
		return(ERR_ABRT);
	}

	tcp_arg(psPcb, psSession);
	tcp_recv(psPcb, TelnetRecv);
	tcp_err(psPcb, TelnetErr);
	tcp_nagle_disable(psPcb);
	ip_set_option(psPcb, SOF_KEEPALIVE);
	psPcb->keep_idle = CONFIG_KEEPALIVE_IDLE_MS;
	psPcb->keep_intvl = CONFIG_KEEPALIVE_INTERVAL_MS;
	psPcb->keep_cnt = CONFIG_KEEPALIVE_COUNT;

	ConsoleWriteRaw(psSession, sg_u8Negotiate, sizeof(sg_u8Negotiate));
	ConsoleBanner(psSession);
	ConsolePrompt(psSession);
	return(ERR_OK);
}

// Flush output and reap closed sessions
void TelnetPoll(void)
{
	SConsoleSession *psSession = g_psConsoleSessions;

	while (psSession)
	{
		SConsoleSession *psNext = psSession->psNext;
		struct tcp_pcb *psPcb;
		bool bWrote = false;

		if (ECONSOLETYPE_TELNET != psSession->eType)
		{
			psSession = psNext;
			continue;
		}

		TelnetProcessInput(psSession);
		psPcb = (struct tcp_pcb *) psSession->pvTransport;

		while (psPcb)
		{
			uint8_t *pu8Span;
			uint32_t u32Span = RingReadSpan(&psSession->sOutput, &pu8Span);
			uint32_t u32Room = tcp_sndbuf(psPcb);

			if ((0 == u32Span) || (0 == u32Room))
			{
				break;
			}

			if (u32Span > u32Room)
			{
				u32Span = u32Room;
			}

			if (ERR_OK != tcp_write(psPcb, pu8Span, (u16_t) u32Span, TCP_WRITE_FLAG_COPY))
			{
				break;
			}

			RingConsume(&psSession->sOutput, u32Span);
			bWrote = true;
		}

		if (bWrote)
		{
			(void) tcp_output(psPcb);
		}

		if (psSession->bClosing)
		{
			if ((NULL == psPcb) || (0 == RingUsed(&psSession->sOutput)))
			{
				TelnetDetach(psSession);
				ConsoleSessionDestroy(psSession);
			}
		}

		psSession = psNext;
	}
}

uint32_t TelnetSessionCount(void)
{
	SConsoleSession *psSession;
	uint32_t u32Count = 0;

	for (psSession = g_psConsoleSessions; psSession; psSession = psSession->psNext)
	{
		if (ECONSOLETYPE_TELNET == psSession->eType)
		{
			u32Count++;
		}
	}

	return(u32Count);
}

EStatus TelnetInit(void)
{
	struct tcp_pcb *psPcb = tcp_new_ip_type(IPADDR_TYPE_ANY);

	if (NULL == psPcb)
	{
		return(ESTATUS_NO_MEMORY);
	}

	ip_set_option(psPcb, SOF_REUSEADDR);
	if (ERR_OK != tcp_bind(psPcb, IP_ANY_TYPE, CONFIG_TELNET_PORT))
	{
		tcp_abort(psPcb);
		return(ESTATUS_NETWORK);
	}

	sg_psListen = tcp_listen_with_backlog(psPcb, 4);
	if (NULL == sg_psListen)
	{
		tcp_abort(psPcb);
		return(ESTATUS_NO_MEMORY);
	}

	tcp_accept(sg_psListen, TelnetAccept);
	return(ESTATUS_OK);
}
