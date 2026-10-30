#pragma once

// TCP <-> UART data bridges, one TCP listener per port.  The newest client wins.

#include "Shared/Shared.h"
#include "Console.h"

extern EStatus BridgeInit(void);
extern void BridgePoll(void);
extern void BridgeApplyPort(uint8_t u8Port);
extern void BridgeDisconnect(uint8_t u8Port);
extern void BridgeStatsClear(uint8_t u8Port);
extern void BridgeStatsPrint(uint8_t u8Port,
                             SConsoleSession *psSession);
extern uint32_t BridgeClientCount(void);
