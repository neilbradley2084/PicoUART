#pragma once

// USB CDC (virtual COM port) transport for the console

#include "Shared/Shared.h"

extern EStatus UsbConsoleInit(void);
extern void UsbConsolePoll(void);
extern bool UsbConsoleConnected(void);
