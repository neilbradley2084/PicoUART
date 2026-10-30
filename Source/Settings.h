#pragma once

// Persistent settings.  Kept in two flash sectors at the top of flash, written
// alternately; the valid copy with the highest sequence number wins at boot.

#include "Shared/Shared.h"
#include "Uart.h"

#define SETTINGS_SSID_MAX		32
#define SETTINGS_PASSWORD_MIN	8
#define SETTINGS_PASSWORD_MAX	63
#define SETTINGS_PORT_NAME_MAX	32

typedef struct SWifiSettings
{
	bool bEnabled;
	bool bDhcp;
	char eSsid[SETTINGS_SSID_MAX + 1];
	char ePassword[SETTINGS_PASSWORD_MAX + 1];
	uint32_t u32Channel;			// 0 = auto
	uint32_t u32Ip;					// Host order, a.b.c.d = 0xaabbccdd
	uint32_t u32Netmask;
	uint32_t u32Gateway;
} SWifiSettings;

typedef struct SPortSettings
{
	SUartLine sLine;
	uint16_t u16TcpPort;			// 0 = no listener
	bool bClearOnConnect;			// Discard UART data received while no client is connected
	char eName[SETTINGS_PORT_NAME_MAX + 1];	// Human readable label, "" = none
} SPortSettings;

// Target power / reset control

typedef enum EPowerMode
{
	EPOWERMODE_PANEL = 0,			// Front panel button replacement
	EPOWERMODE_ATX,					// Direct ATX supply control (PS_ON# / PWR_OK)
} EPowerMode;

typedef enum EPowerStartup
{
	EPOWERSTARTUP_OFF = 0,
	EPOWERSTARTUP_ON,
	EPOWERSTARTUP_LAST,
} EPowerStartup;

typedef enum EPinPolarity
{
	EPINPOLARITY_LOW = 0,			// Asserted = low
	EPINPOLARITY_HIGH,				// Asserted = high
} EPinPolarity;

typedef enum EPinDrive
{
	EPINDRIVE_PUSHPULL = 0,
	EPINDRIVE_OPENCOLLECTOR,		// Driven low or released
} EPinDrive;

typedef enum EPinPull
{
	EPINPULL_NONE = 0,
	EPINPULL_UP,
	EPINPULL_DOWN,
} EPinPull;

typedef enum EPanelAction
{
	EPANELACTION_PULSE = 0,			// Button press of u32PowerPulseMs
	EPANELACTION_LEVEL,				// Held asserted while on
} EPanelAction;

typedef struct SPowerSettings
{
	// Global
	EPowerMode eMode;
	EPowerStartup eStartup;
	bool bLastOn;					// Last commanded state, for startup "last"
	EPinPolarity eResetPolarity;
	EPinDrive eResetDrive;			// ATX and panel level reset - panel pulse reset is always open collector
	uint32_t u32ResetPulseMs;		// "power reset" pulse, both modes

	// ATX
	EPinPolarity ePsOnPolarity;
	EPinDrive ePsOnDrive;
	uint32_t u32ResetHoldMs;		// Reset held this long after PWR_OK / power sensed
	uint32_t u32PwrOkTimeoutMs;		// PS_ON# dropped if PWR_OK not seen by then

	// Panel
	EPanelAction eResetAction;		// Pulse = button press, level = held while not on
	EPinPull eResetPull;
	EPinPolarity ePowerPolarity;
	EPinDrive ePowerDrive;
	EPinPull ePowerPull;
	EPanelAction ePowerAction;
	uint32_t u32PowerPulseMs;
	uint32_t u32ForceOffHoldMs;		// Press-and-hold override
	bool bSense;					// Power sense input in use
	EPinPolarity eSensePolarity;
	uint32_t u32SenseTimeoutMs;
} SPowerSettings;

typedef struct SSettings
{
	uint32_t u32Magic;
	uint32_t u32Version;
	uint32_t u32Sequence;
	SWifiSettings sWifi;
	SPortSettings sPort[UART_PORT_COUNT];
	SPowerSettings sPower;
	uint32_t u32Crc;				// Must be last
} SSettings;

extern SSettings g_sSettings;

extern void SettingsWifiDefaults(SWifiSettings *psWifi);
extern void SettingsPortDefaults(uint8_t u8Port,
                                 SPortSettings *psPort);
extern void SettingsPowerDefaults(SPowerSettings *psPower);
extern void SettingsDefaults(SSettings *psSettings);
extern EStatus SettingsLoad(void);
extern EStatus SettingsSave(void);
