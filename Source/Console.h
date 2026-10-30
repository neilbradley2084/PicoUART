#pragma once

// Configuration console sessions.  Any number of telnet sessions plus the USB
// CDC session run at once; all of them share the same command set.

#include "Shared/Shared.h"
#include "Config.h"
#include "Ring.h"

typedef enum EConsoleType
{
	ECONSOLETYPE_USB = 0,
	ECONSOLETYPE_TELNET,
	ECONSOLETYPE_HTTP,						// One command, output captured for a web reply
} EConsoleType;

typedef struct SConsoleSession
{
	struct SConsoleSession *psNext;
	EConsoleType eType;
	void *pvTransport;						// struct tcp_pcb * for telnet
	void *pvPendingInput;					// struct pbuf * received, processed from the main loop
	SRing sOutput;							// Pending output, drained by the transport
	char eLine[CONFIG_CONSOLE_LINE_SIZE];
	uint32_t u32LineLength;
	uint8_t u8Escape;						// ANSI escape parser state
	uint8_t u8Telnet;						// Telnet IAC parser state
	bool bLastCr;
	bool bExecuting;						// Inside CommandExecute()
	bool bClosing;							// Close once output drains
} SConsoleSession;

extern SConsoleSession *g_psConsoleSessions;

extern EStatus ConsoleSessionCreate(EConsoleType eType,
                                    void *pvTransport,
                                    SConsoleSession **ppsSession);
extern void ConsoleSessionDestroy(SConsoleSession *psSession);
extern void ConsoleInput(SConsoleSession *psSession,
                         uint8_t u8Data);
extern void ConsoleRunLine(SConsoleSession *psSession,
                           const char *peLine,
                           uint32_t u32Length);
extern void ConsoleWrite(SConsoleSession *psSession,
                         const char *peText,
                         uint32_t u32Length);
extern void ConsoleWriteRaw(SConsoleSession *psSession,
                            const uint8_t *pu8Data,
                            uint32_t u32Length);
extern void ConsolePrintf(SConsoleSession *psSession,
                          const char *peFormat,
                          ...) __attribute__((format(printf, 2, 3)));
extern void ConsoleBanner(SConsoleSession *psSession);
extern void ConsolePrompt(SConsoleSession *psSession);
extern void FormatDuration(uint64_t u64Us,
                           char *peBuffer,
                           uint32_t u32Size);
extern void FormatIp(uint32_t u32Ip,
                     char *peBuffer,
                     uint32_t u32Size);
