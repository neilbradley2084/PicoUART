#pragma once

// Common definitions shared by all UartBridge modules

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef enum EStatus
{
	ESTATUS_OK = 0,
	ESTATUS_NO_MEMORY,
	ESTATUS_INVALID_PARAMETER,
	ESTATUS_NOT_FOUND,
	ESTATUS_BUSY,
	ESTATUS_FLASH,
	ESTATUS_NETWORK,
	ESTATUS_HARDWARE,
} EStatus;

// Assign eStatus from an expression and bail to errorExit: on failure
#define ERR_GOTO(x)					{ eStatus = (x); if (ESTATUS_OK != eStatus) { goto errorExit; } }

#define MEMALLOC(p, s)				{ (p) = calloc(1, (s)); }
#define MEMALLOC_NO_CLEAR(p, s)		{ (p) = malloc(s); }
#define MEMFREE(p)					{ free(p); (p) = NULL; }

// Linker-provided end of the heap region
extern char __HeapLimit;

// Messages delivered to every open console session
extern void LogMsg(const char *peFormat, ...) __attribute__((format(printf, 1, 2)));
extern void LogErr(const char *peFormat, ...) __attribute__((format(printf, 1, 2)));
extern const char *StatusText(EStatus eStatus);
