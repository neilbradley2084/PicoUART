#pragma once

// WiFi station management: join, keep retrying forever, rejoin on loss

#include "Shared/Shared.h"
#include "Console.h"
#include "Settings.h"

extern EStatus WifiInit(void);
extern void WifiPoll(void);
extern void WifiApply(const SWifiSettings *psOld,
                      SConsoleSession *psSession);
extern void WifiEnable(bool bEnable);
extern void WifiReconnect(bool bResetStats);
extern EStatus WifiScanStart(SConsoleSession *psSession);
extern void WifiSessionGone(SConsoleSession *psSession);
extern void WifiStatsPrint(SConsoleSession *psSession);
extern bool WifiConnected(void);
