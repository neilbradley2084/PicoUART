#pragma once

// Telnet transport for console sessions

#include "Shared/Shared.h"

extern EStatus TelnetInit(void);
extern void TelnetPoll(void);
extern uint32_t TelnetSessionCount(void);
