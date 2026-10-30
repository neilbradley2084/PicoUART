// Small web server for the configuration page.
//
//   GET  /      the page (Web/index.html, built into flash)
//   POST /cmd   body = one console command line; reply = its console output
//
// Every change made from the page is an ordinary console command, so validation,
// saving and immediate effect are exactly those of telnet / USB.  Requests are
// parsed and run from the main loop (HttpPoll), never inside lwIP callbacks, as
// commands may touch the radio.  One request per connection (Connection: close).

#include <stdio.h>
#include <strings.h>
#include "Http.h"
#include "HttpPage.h"
#include "Console.h"
#include "Config.h"
#include "lwip/tcp.h"

typedef struct SHttpConn
{
	struct SHttpConn *psNext;
	struct tcp_pcb *psPcb;				// NULL once lwIP has freed it
	struct pbuf *psPending;				// Received, not yet parsed
	char *peRequest;					// CONFIG_HTTP_REQUEST_SIZE + 1
	uint32_t u32Length;
	bool bHandled;						// Reply queued
	uint8_t *pu8Reply;					// Owned reply bytes (headers, command output)
	uint32_t u32ReplyLength;
	uint32_t u32ReplySent;
	const uint8_t *pu8Static;			// Then this, straight from flash (the page)
	uint32_t u32StaticLength;
	uint32_t u32StaticSent;
	bool bClose;						// Everything queued - close when lwIP takes it
} SHttpConn;

static struct tcp_pcb *sg_psListen;
static SHttpConn *sg_psConns;

static void HttpFree(SHttpConn *psConn)
{
	SHttpConn **ppsLink = &sg_psConns;

	while (*ppsLink)
	{
		if (*ppsLink == psConn)
		{
			*ppsLink = psConn->psNext;
			break;
		}

		ppsLink = &(*ppsLink)->psNext;
	}

	if (psConn->psPending)
	{
		pbuf_free(psConn->psPending);
	}

	MEMFREE(psConn->peRequest);
	MEMFREE(psConn->pu8Reply);
	MEMFREE(psConn);
}

static void HttpDetach(SHttpConn *psConn,
                       bool bAbort)
{
	struct tcp_pcb *psPcb = psConn->psPcb;

	psConn->psPcb = NULL;
	if (NULL == psPcb)
	{
		return;
	}

	tcp_arg(psPcb, NULL);
	tcp_recv(psPcb, NULL);
	tcp_err(psPcb, NULL);

	if (bAbort || (tcp_close(psPcb) != ERR_OK))
	{
		tcp_abort(psPcb);
	}
}

static err_t HttpRecv(void *pvArg,
                      struct tcp_pcb *psPcb,
                      struct pbuf *psPbuf,
                      err_t s8Err)
{
	SHttpConn *psConn = (SHttpConn *) pvArg;

	if (NULL == psPbuf)
	{
		// Client closed its side; finish any reply we owe, then close
		psConn->bClose = true;
		return(ERR_OK);
	}

	if (s8Err != ERR_OK)
	{
		pbuf_free(psPbuf);
		return(s8Err);
	}

	if (psConn->psPending)
	{
		pbuf_cat(psConn->psPending, psPbuf);
	}
	else
	{
		psConn->psPending = psPbuf;
	}

	return(ERR_OK);
}

static void HttpErr(void *pvArg,
                    err_t s8Err)
{
	SHttpConn *psConn = (SHttpConn *) pvArg;

	// The pcb is already gone
	psConn->psPcb = NULL;
}

static err_t HttpAccept(void *pvArg,
                        struct tcp_pcb *psPcb,
                        err_t s8Err)
{
	SHttpConn *psConn = NULL;

	if ((s8Err != ERR_OK) || (NULL == psPcb))
	{
		return(ERR_VAL);
	}

	MEMALLOC(psConn, sizeof(*psConn));
	if (psConn)
	{
		MEMALLOC_NO_CLEAR(psConn->peRequest, CONFIG_HTTP_REQUEST_SIZE + 1);
	}

	if ((NULL == psConn) || (NULL == psConn->peRequest))
	{
		MEMFREE(psConn);
		tcp_abort(psPcb);
		return(ERR_ABRT);
	}

	psConn->psPcb = psPcb;
	psConn->psNext = sg_psConns;
	sg_psConns = psConn;

	tcp_arg(psPcb, psConn);
	tcp_recv(psPcb, HttpRecv);
	tcp_err(psPcb, HttpErr);
	tcp_nagle_disable(psPcb);
	return(ERR_OK);
}

// Queue a reply: status line + headers + owned body, optionally followed by static bytes
static void HttpReply(SHttpConn *psConn,
                      const char *peStatus,
                      const char *peType,
                      const uint8_t *pu8Body,
                      uint32_t u32BodyLength,
                      const uint8_t *pu8Static,
                      uint32_t u32StaticLength)
{
	char eHeader[256];
	int s32Header;

	s32Header = snprintf(eHeader, sizeof(eHeader),
						 "HTTP/1.1 %s\r\n"
						 "Content-Type: %s\r\n"
						 "Content-Length: %lu\r\n"
						 "Cache-Control: no-store\r\n"
						 "Connection: close\r\n"
						 "\r\n",
						 peStatus,
						 peType,
						 (unsigned long) (u32BodyLength + u32StaticLength));

	if ((s32Header < 0) || ((uint32_t) s32Header >= sizeof(eHeader)))
	{
		HttpDetach(psConn, true);
		return;
	}

	MEMALLOC_NO_CLEAR(psConn->pu8Reply, (uint32_t) s32Header + u32BodyLength);
	if (NULL == psConn->pu8Reply)
	{
		HttpDetach(psConn, true);
		return;
	}

	memcpy(psConn->pu8Reply, eHeader, (uint32_t) s32Header);
	if (u32BodyLength)
	{
		memcpy(psConn->pu8Reply + s32Header, pu8Body, u32BodyLength);
	}

	psConn->u32ReplyLength = (uint32_t) s32Header + u32BodyLength;
	psConn->pu8Static = pu8Static;
	psConn->u32StaticLength = u32StaticLength;
	psConn->bHandled = true;
}

static void HttpReplyText(SHttpConn *psConn,
                          const char *peStatus,
                          const char *peText)
{
	HttpReply(psConn, peStatus, "text/plain; charset=utf-8", (const uint8_t *) peText, strlen(peText), NULL, 0);
}

// Run one console command and reply with what it printed
static void HttpCommand(SHttpConn *psConn,
                        const char *peBody,
                        uint32_t u32BodyLength)
{
	SConsoleSession *psSession = NULL;
	uint8_t *pu8Output = NULL;
	uint32_t u32Output;
	uint32_t u32Loop;

	// The command is the first line of the body
	for (u32Loop = 0; u32Loop < u32BodyLength; u32Loop++)
	{
		if (('\r' == peBody[u32Loop]) || ('\n' == peBody[u32Loop]))
		{
			u32BodyLength = u32Loop;
			break;
		}
	}

	if (ConsoleSessionCreate(ECONSOLETYPE_HTTP, NULL, &psSession) != ESTATUS_OK)
	{
		HttpReplyText(psConn, "503 Service Unavailable", "out of memory\n");
		return;
	}

	ConsoleRunLine(psSession, peBody, u32BodyLength);

	u32Output = RingUsed(&psSession->sOutput);
	MEMALLOC_NO_CLEAR(pu8Output, u32Output + 1);
	if (NULL == pu8Output)
	{
		ConsoleSessionDestroy(psSession);
		HttpReplyText(psConn, "503 Service Unavailable", "out of memory\n");
		return;
	}

	for (u32Loop = 0; u32Loop < u32Output; u32Loop++)
	{
		(void) RingGet(&psSession->sOutput, &pu8Output[u32Loop]);
	}

	ConsoleSessionDestroy(psSession);
	HttpReply(psConn, "200 OK", "text/plain; charset=utf-8", pu8Output, u32Output, NULL, 0);
	MEMFREE(pu8Output);
}

// Find a header value (case-insensitive name) in the header block, or NULL
static const char *HttpHeader(const char *peHeaders,
                              const char *peName)
{
	uint32_t u32NameLength = strlen(peName);
	const char *peLine = peHeaders;

	while (peLine && *peLine)
	{
		if ((0 == strncasecmp(peLine, peName, u32NameLength)) && (':' == peLine[u32NameLength]))
		{
			peLine += u32NameLength + 1;
			while (' ' == *peLine)
			{
				peLine++;
			}

			return(peLine);
		}

		peLine = strstr(peLine, "\r\n");
		if (peLine)
		{
			peLine += 2;
		}
	}

	return(NULL);
}

// Parse once the whole request is in; returns false while more is needed
static bool HttpParse(SHttpConn *psConn)
{
	char *peEnd;
	char *peBody;
	const char *peValue;
	uint32_t u32HeaderLength;
	uint32_t u32BodyLength = 0;

	psConn->peRequest[psConn->u32Length] = '\0';
	peEnd = strstr(psConn->peRequest, "\r\n\r\n");
	if (NULL == peEnd)
	{
		if (psConn->u32Length >= CONFIG_HTTP_REQUEST_SIZE)
		{
			HttpReplyText(psConn, "431 Request Header Fields Too Large", "request too large\n");
			return(true);
		}

		return(false);
	}

	u32HeaderLength = (uint32_t) (peEnd - psConn->peRequest) + 4;
	peBody = psConn->peRequest + u32HeaderLength;

	peValue = HttpHeader(psConn->peRequest, "Content-Length");
	if (peValue)
	{
		u32BodyLength = (uint32_t) strtoul(peValue, NULL, 10);
	}

	if ((u32HeaderLength + u32BodyLength) > CONFIG_HTTP_REQUEST_SIZE)
	{
		HttpReplyText(psConn, "413 Content Too Large", "request too large\n");
		return(true);
	}

	if (psConn->u32Length < (u32HeaderLength + u32BodyLength))
	{
		return(false);
	}

	if ((0 == strncmp(psConn->peRequest, "GET / ", 6)) ||
		(0 == strncmp(psConn->peRequest, "GET /index.html ", 16)))
	{
		HttpReply(psConn, "200 OK", "text/html; charset=utf-8", NULL, 0, g_u8HttpPage, g_u32HttpPageSize);
	}
	else
	if (0 == strncmp(psConn->peRequest, "POST /cmd ", 10))
	{
		HttpCommand(psConn, peBody, u32BodyLength);
	}
	else
	if (0 == strncmp(psConn->peRequest, "GET /favicon.ico ", 17))
	{
		HttpReply(psConn, "204 No Content", "text/plain", NULL, 0, NULL, 0);
	}
	else
	{
		HttpReplyText(psConn, "404 Not Found", "not found\n");
	}

	return(true);
}

// Push queued reply bytes into lwIP as send space allows; true when all are queued
static bool HttpSend(SHttpConn *psConn)
{
	struct tcp_pcb *psPcb = psConn->psPcb;
	bool bWrote = false;

	while (psConn->u32ReplySent < psConn->u32ReplyLength)
	{
		uint32_t u32Chunk = psConn->u32ReplyLength - psConn->u32ReplySent;
		uint32_t u32Room = tcp_sndbuf(psPcb);

		if (0 == u32Room)
		{
			break;
		}

		if (u32Chunk > u32Room)
		{
			u32Chunk = u32Room;
		}

		if (tcp_write(psPcb, psConn->pu8Reply + psConn->u32ReplySent, (u16_t) u32Chunk, TCP_WRITE_FLAG_COPY) != ERR_OK)
		{
			break;
		}

		psConn->u32ReplySent += u32Chunk;
		bWrote = true;
	}

	// The page lives in flash for ever, so lwIP can reference it without a copy
	while ((psConn->u32ReplySent >= psConn->u32ReplyLength) &&
		   (psConn->u32StaticSent < psConn->u32StaticLength))
	{
		uint32_t u32Chunk = psConn->u32StaticLength - psConn->u32StaticSent;
		uint32_t u32Room = tcp_sndbuf(psPcb);

		if (0 == u32Room)
		{
			break;
		}

		if (u32Chunk > u32Room)
		{
			u32Chunk = u32Room;
		}

		if (tcp_write(psPcb, psConn->pu8Static + psConn->u32StaticSent, (u16_t) u32Chunk, 0) != ERR_OK)
		{
			break;
		}

		psConn->u32StaticSent += u32Chunk;
		bWrote = true;
	}

	if (bWrote)
	{
		(void) tcp_output(psPcb);
	}

	return(((psConn->u32ReplySent >= psConn->u32ReplyLength) &&
			(psConn->u32StaticSent >= psConn->u32StaticLength)) ? true : false);
}

void HttpPoll(void)
{
	SHttpConn *psConn = sg_psConns;

	while (psConn)
	{
		SHttpConn *psNext = psConn->psNext;

		if (NULL == psConn->psPcb)
		{
			HttpFree(psConn);
			psConn = psNext;
			continue;
		}

		// Collect request bytes
		if (psConn->psPending)
		{
			struct pbuf *psPbuf = psConn->psPending;
			uint32_t u32Room = CONFIG_HTTP_REQUEST_SIZE - psConn->u32Length;
			uint32_t u32Copy = psPbuf->tot_len;

			psConn->psPending = NULL;
			if (u32Copy > u32Room)
			{
				u32Copy = u32Room;
			}

			if (false == psConn->bHandled)
			{
				psConn->u32Length += pbuf_copy_partial(psPbuf, psConn->peRequest + psConn->u32Length, (u16_t) u32Copy, 0);
			}

			tcp_recved(psConn->psPcb, psPbuf->tot_len);
			pbuf_free(psPbuf);
		}

		if ((false == psConn->bHandled) && psConn->u32Length)
		{
			(void) HttpParse(psConn);
		}

		if (psConn->bHandled && psConn->psPcb)
		{
			if (HttpSend(psConn))
			{
				// tcp_close sends FIN after the queued data
				HttpDetach(psConn, false);
			}
		}
		else
		if (psConn->bClose && psConn->psPcb)
		{
			// Client went away without a complete request
			HttpDetach(psConn, false);
		}

		psConn = psNext;
	}
}

EStatus HttpInit(void)
{
	struct tcp_pcb *psPcb = tcp_new_ip_type(IPADDR_TYPE_ANY);

	if (NULL == psPcb)
	{
		return(ESTATUS_NO_MEMORY);
	}

	ip_set_option(psPcb, SOF_REUSEADDR);
	if (tcp_bind(psPcb, IP_ANY_TYPE, CONFIG_HTTP_PORT) != ERR_OK)
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

	tcp_accept(sg_psListen, HttpAccept);
	return(ESTATUS_OK);
}
