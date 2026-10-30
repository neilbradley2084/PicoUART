#pragma once

// Target power / reset control: ATX supply (PS_ON# / PWR_OK) or front panel
// button replacement, plus status LEDs and front panel button pass-through.

#include "Shared/Shared.h"
#include "Console.h"

// Pins: adopted unchanged after a warm reboot, else their safe state.
// Call right after SettingsLoad(), before anything slow, then start core 1.
extern EStatus PowerInit(void);

// Apply the startup policy (off / on / last; cold boot only).  Call just before the main loop.
extern void PowerStartup(void);

// Core 0 main loop
extern void PowerPoll(void);

// Core 1 loop: asserts reset the moment PWR_OK drops (ATX).  RAM resident.
extern void PowerCore1Service(void);

// Commands.  psSession (may be NULL) receives notes such as "already on".
extern EStatus PowerOn(SConsoleSession *psSession);
extern EStatus PowerOff(SConsoleSession *psSession);
extern EStatus PowerForceOff(SConsoleSession *psSession);
extern EStatus PowerReset(SConsoleSession *psSession);
extern EStatus PowerResetHold(SConsoleSession *psSession,
                              bool bHold);

// Startup failed: target off, outputs idle, no sequencing
extern void PowerFailsafe(void);

// Pin settings changed - re-drive outputs with the new polarity/drive/pull.
// bForgetState when mode, poweraction or sense changed (target is off).
extern void PowerApplySettings(bool bForgetState);

// True when the target is off and no sequence is running
extern bool PowerIsIdleOff(void);

extern void PowerStatsPrint(SConsoleSession *psSession);
