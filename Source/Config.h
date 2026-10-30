#pragma once

// Board-level configuration for the Pico 2 W / Pico W UART bridge

// GPIO assignments - hardware UART function pins, same on RP2040 and RP2350
#define CONFIG_UART0_PIN_TX				0
#define CONFIG_UART0_PIN_RX				1
#define CONFIG_UART0_PIN_CTS			2
#define CONFIG_UART0_PIN_RTS			3

#define CONFIG_UART1_PIN_TX				4
#define CONFIG_UART1_PIN_RX				5
#define CONFIG_UART1_PIN_CTS			6
#define CONFIG_UART1_PIN_RTS			7

// Target power control
#define CONFIG_POWER_PIN_OUT			8		// ATX: PS_ON#        Panel: power button
#define CONFIG_POWER_PIN_RESET			9		// Target reset       Panel: reset button
#define CONFIG_POWER_PIN_SENSE			10		// ATX: PWR_OK        Panel: power sense (e.g. power LED)

// Status LEDs - sink, active low (low = LED on)
#define CONFIG_LED_PIN_RESET			11		// Reset asserted
#define CONFIG_LED_PIN_POWER			12		// Power output asserted (1 Hz blink = power-up failed)
#define CONFIG_LED_PIN_SENSED			13		// Power sensed from the supply

// Front panel button pass-through inputs - switch to ground, internal pull-up
#define CONFIG_BUTTON_PIN_POWER			14
#define CONFIG_BUTTON_PIN_RESET			15
#define CONFIG_BUTTON_DEBOUNCE_MS		10

// Default per-port TCP data port is this + port number
#define CONFIG_TCP_PORT_BASE			9000

#define CONFIG_TELNET_PORT				23

// Web configuration page
#define CONFIG_HTTP_PORT				80
#define CONFIG_HTTP_REQUEST_SIZE		2048	// Request line + headers + body, per connection

// Heap left free after the UART ring buffers take everything else
// (console sessions, scan results, newlib)
#define CONFIG_HEAP_RESERVE				(32 * 1024)

// Per console session
#define CONFIG_CONSOLE_OUTPUT_SIZE		(4 * 1024)
#define CONFIG_CONSOLE_LINE_SIZE		256

// LED: data counts as flowing for this long after the last UART byte in or out
#define CONFIG_LED_ACTIVITY_HOLD_MS		500

#define CONFIG_WIFI_JOIN_TIMEOUT_MS		30000
#define CONFIG_WIFI_RETRY_DELAY_MS		2000

// Longest UART -> TCP data is held to fill a segment while earlier data is unacknowledged
#define CONFIG_BRIDGE_COALESCE_US		2000

// TCP keepalive for bridge and telnet clients
#define CONFIG_KEEPALIVE_IDLE_MS		10000
#define CONFIG_KEEPALIVE_INTERVAL_MS	2000
#define CONFIG_KEEPALIVE_COUNT			3
