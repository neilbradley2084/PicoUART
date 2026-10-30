// UartBridge - dual UART <-> WiFi bridge for the Raspberry Pi Pico 2 W / Pico W
//
// Core 0: WiFi (cyw43 + lwIP, polled), TCP bridges, telnet + USB consoles, web page
// Core 1: UART FIFO <-> ring buffer pump and ATX PWR_OK guard, running entirely from SRAM

#include "Main.h"
#include "Settings.h"
#include "Uart.h"
#include "Wifi.h"
#include "Bridge.h"
#include "Console.h"
#include "Telnet.h"
#include "UsbConsole.h"
#include "Power.h"
#include "Http.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/psm.h"
#include "hardware/resets.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

// Time allowed for "Rebooting" to reach the console before it happens
#define MAIN_REBOOT_DELAY_US	250000

static bool sg_bRebootPending;
static bool sg_bRebootBootsel;
static uint64_t sg_u64RebootAtUs;

void MainRequestReboot(bool bBootsel)
{
	sg_bRebootPending = true;
	sg_bRebootBootsel = bBootsel;
	sg_u64RebootAtUs = time_us_64() + MAIN_REBOOT_DELAY_US;
}

static void MainReboot(void)
{
	if (sg_bRebootBootsel)
	{
		reset_usb_boot(0, 0);
	}

	// Watchdog reboot that leaves the RESETS block alone, so IO_BANK0 / PADS_BANK0
	// (and with them the target power, reset and LED pins) ride through the reboot.
	// See BootResets.c for the matching boot-time side.  Everything else is quiesced
	// first, so DMA, PIO, USB etc. don't keep running through boot.
	(void) save_and_disable_interrupts();
	multicore_reset_core1();
	reset_block_mask(RESETS_RESET_BITS & ~((1u << RESET_IO_BANK0) |
										   (1u << RESET_PADS_BANK0) |
										   (1u << RESET_IO_QSPI) |
										   (1u << RESET_PADS_QSPI) |
										   (1u << RESET_SYSCFG) |
										   (1u << RESET_PLL_SYS) |
										   (1u << RESET_PLL_USB)));
	watchdog_hw->scratch[4] = 0;		// Normal boot, no bootrom reboot vector
	hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
	psm_hw->wdsel = PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS | PSM_WDSEL_RESETS_BITS);
	hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
	for (;;)
	{
		tight_loop_contents();
	}
}

int main(void)
{
	EStatus eStatus = ESTATUS_OK;
	EStatus eSettings;
	uint32_t u32Loop;

	eSettings = SettingsLoad();

	// Target pins first: adopted as-is after a warm reboot, else their safe state
	// (ATX: reset asserted, PS_ON# released).  Then core 1, for the PWR_OK guard.
	ERR_GOTO(PowerInit());
	UartCore1Launch();

	// Order matters: everything that allocates goes before UartStart(),
	// which hands the rest of the heap to the ring buffers.
	ERR_GOTO(UartInit());
	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		UartConfigure((uint8_t) u32Loop, &g_sSettings.sPort[u32Loop].sLine);
	}

	// PowerPoll between steps keeps PWR_OK timeouts and reset holds on time
	ERR_GOTO(UsbConsoleInit());
	PowerPoll();
	ERR_GOTO(WifiInit());
	PowerPoll();
	ERR_GOTO(TelnetInit());
	ERR_GOTO(HttpInit());
	ERR_GOTO(BridgeInit());
	PowerPoll();
	ERR_GOTO(UartStart());

	if (ESTATUS_NOT_FOUND == eSettings)
	{
		LogMsg("settings: none stored, using defaults");
	}

	// Everything is up - apply the startup power policy (cold boot only)
	PowerStartup();

	for (;;)
	{
		cyw43_arch_poll();
		WifiPoll();
		BridgePoll();
		TelnetPoll();
		HttpPoll();
		UsbConsolePoll();
		PowerPoll();

		if (sg_bRebootPending && (time_us_64() >= sg_u64RebootAtUs))
		{
			MainReboot();
		}
	}

errorExit:
	// Target off and left alone - core 1's PWR_OK guard may not be running
	PowerFailsafe();

	// Keep the USB console alive so the failure can be seen and acted on
	for (;;)
	{
		LogErr("startup failed (%s)", StatusText(eStatus));
		for (u32Loop = 0; u32Loop < 5000; u32Loop++)
		{
			UsbConsolePoll();
			if (sg_bRebootPending && (time_us_64() >= sg_u64RebootAtUs))
			{
				MainReboot();
			}

			sleep_ms(1);
		}
	}
}
