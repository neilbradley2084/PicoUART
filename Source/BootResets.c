// Boot-time peripheral resets.
//
// Replaces the SDK's weak runtime_init_early_resets() with one that also leaves
// IO_BANK0 and PADS_BANK0 alone.  Together with the RESETS-preserving watchdog
// reboot in Main.c this keeps the target power / reset / LED pins exactly as they
// were across a software reboot of this device.  After power-on or a RUN pin reset
// the hardware has already reset these blocks, so nothing changes there.

#include "Shared/Shared.h"
#include "pico/runtime_init.h"
#include "hardware/resets.h"

void runtime_init_early_resets(void)
{
	// As the SDK, plus IO_BANK0 and PADS_BANK0
	reset_block_mask(~((1u << RESET_IO_QSPI) |
					   (1u << RESET_PADS_QSPI) |
					   (1u << RESET_PLL_USB) |
					   (1u << RESET_USBCTRL) |
					   (1u << RESET_SYSCFG) |
					   (1u << RESET_PLL_SYS) |
					   (1u << RESET_IO_BANK0) |
					   (1u << RESET_PADS_BANK0)));

	// Release peripherals clocked only by clk_sys / clk_ref (same list as the SDK)
	unreset_block_mask_wait_blocking(RESETS_RESET_BITS & ~(
#if PICO_RP2040
									 (1u << RESET_RTC) |
#else
									 (1u << RESET_HSTX) |
#endif
									 (1u << RESET_ADC) |
									 (1u << RESET_SPI0) |
									 (1u << RESET_SPI1) |
									 (1u << RESET_UART0) |
									 (1u << RESET_UART1) |
									 (1u << RESET_USBCTRL)));
}
