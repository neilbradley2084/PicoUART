cmake_minimum_required(VERSION 3.13)

# Dual UART <-> WiFi bridge for the Raspberry Pi Pico 2 W (RP2350) or Pico W (RP2040)

# pico2_w (default) or pico_w: cmake -G Ninja -DPICO_BOARD=pico_w ..
set(PICO_BOARD pico2_w CACHE STRING "Board type: pico2_w or pico_w")

if ((NOT PICO_BOARD STREQUAL "pico2_w") AND (NOT PICO_BOARD STREQUAL "pico_w"))
	message(FATAL_ERROR "PICO_BOARD must be pico2_w or pico_w (got '${PICO_BOARD}')")
endif()

# The Pico SDK is pulled into the build tree.  A PICO_SDK_PATH environment
# variable is ignored; to use another copy pass -DPICO_SDK_PATH=... explicitly.
if (NOT DEFINED PICO_SDK_PATH)
	unset(ENV{PICO_SDK_PATH})
	set(PICO_SDK_FETCH_FROM_GIT ON)
	set(PICO_SDK_FETCH_FROM_GIT_TAG 2.2.0)
	set(PICO_SDK_FETCH_FROM_GIT_PATH ${CMAKE_BINARY_DIR}/External)
endif()

include(pico_sdk_import.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "2.1.0")
	message(FATAL_ERROR "Pico SDK ${PICO_SDK_VERSION_STRING} at ${PICO_SDK_PATH} is too old for the RP2350 - need 2.1.0 or later")
endif()

project(UartBridge C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(UartBridge
	Source/Main.c
	Source/Settings.c
	Source/Uart.c
	Source/Wifi.c
	Source/Bridge.c
	Source/Console.c
	Source/Commands.c
	Source/Telnet.c
	Source/Power.c
	Source/BootResets.c
	Source/Http.c
	${CMAKE_CURRENT_BINARY_DIR}/HttpPage.c
	Source/UsbConsole.c
	Source/UsbDescriptors.c
	)

# Web page built into flash; regenerated whenever index.html changes
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/HttpPage.c
	COMMAND ${CMAKE_COMMAND}
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/Source/Web/index.html
			-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/HttpPage.c
			-P ${CMAKE_CURRENT_SOURCE_DIR}/Source/Web/Embed.cmake
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Source/Web/index.html
			${CMAKE_CURRENT_SOURCE_DIR}/Source/Web/Embed.cmake
	COMMENT "Embedding web page"
	)

target_include_directories(UartBridge PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/Source
	)

target_compile_definitions(UartBridge PRIVATE
	PICO_STACK_SIZE=0x1000
	CYW43_HOST_NAME=\"UartBridge\"
	)

target_compile_options(UartBridge PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(UartBridge
	pico_stdlib
	pico_multicore
	pico_unique_id
	pico_cyw43_arch_lwip_poll
	hardware_uart
	hardware_clocks
	hardware_flash
	hardware_watchdog
	tinyusb_device
	)

# Console is ours (USB CDC + telnet), not SDK stdio
pico_enable_stdio_usb(UartBridge 0)
pico_enable_stdio_uart(UartBridge 0)

pico_add_extra_outputs(UartBridge)
