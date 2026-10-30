#pragma once

// Small web server: serves the configuration page and runs console commands
// posted to /cmd, replying with their output.

#include "Shared/Shared.h"

extern EStatus HttpInit(void);
extern void HttpPoll(void);
