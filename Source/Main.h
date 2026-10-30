#pragma once

// Main loop services

#include "Shared/Shared.h"

// Reboot (or drop to the USB bootloader) once pending console output has had time to drain
extern void MainRequestReboot(bool bBootsel);
