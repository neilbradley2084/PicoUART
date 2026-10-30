#pragma once

// Console command set: <functional area> <action> [arguments]

#include "Shared/Shared.h"
#include "Console.h"

extern void CommandExecute(SConsoleSession *psSession,
                           uint32_t u32Argc,
                           char **ppeArgv);
