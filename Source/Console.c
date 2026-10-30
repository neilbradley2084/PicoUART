// Console session plumbing: line editing, output buffering, broadcast logging

#include <stdio.h>
#include <stdarg.h>
#include "Console.h"
#include "Commands.h"
#include "Wifi.h"
#include "pico/stdlib.h"

#define CONSOLE_PROMPT		"> "
#define CONSOLE_FORMAT_SIZE	512

SConsoleSession *g_psConsoleSessions = NULL;

static const char *sg_peStatusText[] =
{
	"ok",
	"out of memory",
	"invalid parameter",
	"not found",
	"busy",
	"flash write failed",
	"network error",
	"hardware error",
};

const char *StatusText(EStatus eStatus)
{
	if ((uint32_t) eStatus < (sizeof(sg_peStatusText) / sizeof(sg_peStatusText[0])))
	{
		return(sg_peStatusText[eStatus]);
	}

	return("unknown error");
}

EStatus ConsoleSessionCreate(EConsoleType eType,
                             void *pvTransport,
                             SConsoleSession **ppsSession)
{
	EStatus eStatus = ESTATUS_OK;
	SConsoleSession *psSession = NULL;

	MEMALLOC(psSession, sizeof(*psSession));
	if (NULL == psSession)
	{
		ERR_GOTO(ESTATUS_NO_MEMORY);
	}

	MEMALLOC_NO_CLEAR(psSession->sOutput.pu8Buffer, CONFIG_CONSOLE_OUTPUT_SIZE);
	if (NULL == psSession->sOutput.pu8Buffer)
	{
		ERR_GOTO(ESTATUS_NO_MEMORY);
	}

	psSession->sOutput.u32Size = CONFIG_CONSOLE_OUTPUT_SIZE;
	psSession->eType = eType;
	psSession->pvTransport = pvTransport;
	psSession->psNext = g_psConsoleSessions;
	g_psConsoleSessions = psSession;
	*ppsSession = psSession;
	return(ESTATUS_OK);

errorExit:
	if (psSession)
	{
		MEMFREE(psSession->sOutput.pu8Buffer);
	}

	MEMFREE(psSession);
	return(eStatus);
}

void ConsoleSessionDestroy(SConsoleSession *psSession)
{
	SConsoleSession **ppsLink = &g_psConsoleSessions;

	while (*ppsLink)
	{
		if (*ppsLink == psSession)
		{
			*ppsLink = psSession->psNext;
			break;
		}

		ppsLink = &(*ppsLink)->psNext;
	}

	WifiSessionGone(psSession);
	MEMFREE(psSession->sOutput.pu8Buffer);
	MEMFREE(psSession);
}

// Queue bytes untouched (telnet negotiation).  Drops whatever does not fit.
void ConsoleWriteRaw(SConsoleSession *psSession,
                     const uint8_t *pu8Data,
                     uint32_t u32Length)
{
	while (u32Length)
	{
		if (false == RingPut(&psSession->sOutput, *pu8Data))
		{
			break;
		}

		pu8Data++;
		u32Length--;
	}
}

// Queue text, expanding \n to \r\n
void ConsoleWrite(SConsoleSession *psSession,
                  const char *peText,
                  uint32_t u32Length)
{
	while (u32Length)
	{
		if ('\n' == *peText)
		{
			if (RingFree(&psSession->sOutput) < 2)
			{
				break;
			}

			(void) RingPut(&psSession->sOutput, '\r');
		}

		if (false == RingPut(&psSession->sOutput, (uint8_t) *peText))
		{
			break;
		}

		peText++;
		u32Length--;
	}
}

void ConsolePrintf(SConsoleSession *psSession,
                   const char *peFormat,
                   ...)
{
	char eBuffer[CONSOLE_FORMAT_SIZE];
	va_list sArgs;
	int s32Length;

	va_start(sArgs, peFormat);
	s32Length = vsnprintf(eBuffer, sizeof(eBuffer), peFormat, sArgs);
	va_end(sArgs);

	if (s32Length < 0)
	{
		return;
	}

	if ((uint32_t) s32Length >= sizeof(eBuffer))
	{
		s32Length = sizeof(eBuffer) - 1;
	}

	ConsoleWrite(psSession, eBuffer, (uint32_t) s32Length);
}

void ConsolePrompt(SConsoleSession *psSession)
{
	ConsoleWrite(psSession, CONSOLE_PROMPT, strlen(CONSOLE_PROMPT));
}

void ConsoleBanner(SConsoleSession *psSession)
{
	ConsolePrintf(psSession, "\nUartBridge (built " __DATE__ " " __TIME__ ")\n"
				  "Type 'help' for commands.\n");
}

// Print a line on every session without trashing what the user is typing
static void ConsoleBroadcast(const char *pePrefix,
                             const char *peFormat,
                             va_list sArgs)
{
	char eBuffer[CONSOLE_FORMAT_SIZE];
	SConsoleSession *psSession;

	(void) vsnprintf(eBuffer, sizeof(eBuffer), peFormat, sArgs);

	for (psSession = g_psConsoleSessions; psSession; psSession = psSession->psNext)
	{
		// Web command sessions only carry their own command's output
		if (psSession->bClosing || (ECONSOLETYPE_HTTP == psSession->eType))
		{
			continue;
		}

		if (psSession->bExecuting)
		{
			// Mid-command: plain line, the command's own prompt follows
			ConsolePrintf(psSession, "%s%s\n", pePrefix, eBuffer);
			continue;
		}

		ConsolePrintf(psSession, "\r\x1b[K%s%s\n", pePrefix, eBuffer);
		ConsolePrompt(psSession);
		ConsoleWrite(psSession, psSession->eLine, psSession->u32LineLength);
	}
}

void LogMsg(const char *peFormat, ...)
{
	va_list sArgs;

	va_start(sArgs, peFormat);
	ConsoleBroadcast("", peFormat, sArgs);
	va_end(sArgs);
}

void LogErr(const char *peFormat, ...)
{
	va_list sArgs;

	va_start(sArgs, peFormat);
	ConsoleBroadcast("ERROR: ", peFormat, sArgs);
	va_end(sArgs);
}

// Split a line into arguments in place.  "quoted strings" keep spaces; \" and \\ escape.
static uint32_t ConsoleTokenize(char *peLine,
                                char **ppeArgv,
                                uint32_t u32Max)
{
	char *peRead = peLine;
	char *peWrite = peLine;
	uint32_t u32Argc = 0;

	while (u32Argc < u32Max)
	{
		while ((' ' == *peRead) || ('\t' == *peRead))
		{
			peRead++;
		}

		if ('\0' == *peRead)
		{
			break;
		}

		ppeArgv[u32Argc++] = peWrite;

		if ('"' == *peRead)
		{
			peRead++;
			while ((*peRead != '\0') && (*peRead != '"'))
			{
				if (('\\' == *peRead) && (peRead[1] != '\0'))
				{
					peRead++;
				}

				*peWrite++ = *peRead++;
			}

			if ('"' == *peRead)
			{
				peRead++;
			}
		}
		else
		{
			while ((*peRead != '\0') && (*peRead != ' ') && (*peRead != '\t'))
			{
				*peWrite++ = *peRead++;
			}
		}

		// Step over the delimiter before terminating - peWrite may equal peRead
		if ((' ' == *peRead) || ('\t' == *peRead))
		{
			peRead++;
		}

		*peWrite++ = '\0';
	}

	return(u32Argc);
}

// Tokenize and run the line in psSession->eLine
static void ConsoleExecuteLine(SConsoleSession *psSession)
{
	char *peArgv[(CONFIG_CONSOLE_LINE_SIZE / 2) + 1];
	uint32_t u32Argc;

	psSession->eLine[psSession->u32LineLength] = '\0';
	psSession->u32LineLength = 0;
	u32Argc = ConsoleTokenize(psSession->eLine, peArgv, sizeof(peArgv) / sizeof(peArgv[0]));

	if (u32Argc)
	{
		psSession->bExecuting = true;
		CommandExecute(psSession, u32Argc, peArgv);
		psSession->bExecuting = false;
	}
}

static void ConsoleExecute(SConsoleSession *psSession)
{
	ConsoleExecuteLine(psSession);

	if (false == psSession->bClosing)
	{
		ConsolePrompt(psSession);
	}
}

// Run one complete command line (no echo, no prompt) - web requests
void ConsoleRunLine(SConsoleSession *psSession,
                    const char *peLine,
                    uint32_t u32Length)
{
	if (u32Length > (CONFIG_CONSOLE_LINE_SIZE - 1))
	{
		u32Length = CONFIG_CONSOLE_LINE_SIZE - 1;
	}

	memcpy(psSession->eLine, peLine, u32Length);
	psSession->u32LineLength = u32Length;
	ConsoleExecuteLine(psSession);
}

// One byte of user input
void ConsoleInput(SConsoleSession *psSession,
                  uint8_t u8Data)
{
	// Swallow ANSI escape sequences (arrow keys etc.)
	if (psSession->u8Escape)
	{
		if ((1 == psSession->u8Escape) && (('[' == u8Data) || ('O' == u8Data)))
		{
			psSession->u8Escape = 2;
		}
		else
		if ((2 == psSession->u8Escape) && ((u8Data < 0x40) || (u8Data > 0x7e)))
		{
			// Parameter bytes - keep going
		}
		else
		{
			psSession->u8Escape = 0;
		}

		return;
	}

	if ('\n' == u8Data)
	{
		if (psSession->bLastCr)
		{
			psSession->bLastCr = false;
			return;
		}

		u8Data = '\r';
	}
	else
	{
		psSession->bLastCr = ('\r' == u8Data);
	}

	if ('\r' == u8Data)
	{
		ConsoleWrite(psSession, "\n", 1);
		ConsoleExecute(psSession);
		return;
	}

	if ((0x08 == u8Data) || (0x7f == u8Data))
	{
		if (psSession->u32LineLength)
		{
			psSession->u32LineLength--;
			ConsoleWrite(psSession, "\b \b", 3);
		}

		return;
	}

	if ((0x03 == u8Data) || (0x15 == u8Data))
	{
		// Ctrl-C / Ctrl-U: abandon the line
		psSession->u32LineLength = 0;
		ConsoleWrite(psSession, "^C\n", 3);
		ConsolePrompt(psSession);
		return;
	}

	if (0x1b == u8Data)
	{
		psSession->u8Escape = 1;
		return;
	}

	if ((u8Data < 0x20) || (u8Data > 0x7e))
	{
		return;
	}

	if (psSession->u32LineLength < (CONFIG_CONSOLE_LINE_SIZE - 1))
	{
		psSession->eLine[psSession->u32LineLength++] = (char) u8Data;
		ConsoleWrite(psSession, (const char *) &u8Data, 1);
	}
}

void FormatDuration(uint64_t u64Us,
                    char *peBuffer,
                    uint32_t u32Size)
{
	uint32_t u32Seconds = (uint32_t) (u64Us / 1000000);
	uint32_t u32Days = u32Seconds / 86400;

	if (u32Days)
	{
		snprintf(peBuffer, u32Size, "%lud %02lu:%02lu:%02lu",
				 (unsigned long) u32Days,
				 (unsigned long) ((u32Seconds / 3600) % 24),
				 (unsigned long) ((u32Seconds / 60) % 60),
				 (unsigned long) (u32Seconds % 60));
	}
	else
	{
		snprintf(peBuffer, u32Size, "%02lu:%02lu:%02lu",
				 (unsigned long) (u32Seconds / 3600),
				 (unsigned long) ((u32Seconds / 60) % 60),
				 (unsigned long) (u32Seconds % 60));
	}
}

void FormatIp(uint32_t u32Ip,
              char *peBuffer,
              uint32_t u32Size)
{
	snprintf(peBuffer, u32Size, "%lu.%lu.%lu.%lu",
			 (unsigned long) ((u32Ip >> 24) & 0xff),
			 (unsigned long) ((u32Ip >> 16) & 0xff),
			 (unsigned long) ((u32Ip >> 8) & 0xff),
			 (unsigned long) (u32Ip & 0xff));
}
