// Settings storage in flash.
//
// Writes run with core 0's interrupts off.  Core 1 keeps running from SRAM,
// so serial traffic is unaffected by the ~50 ms sector erase.

#include "Settings.h"
#include "Config.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#define SETTINGS_MAGIC			0x47524255		// 'UBRG'
#define SETTINGS_VERSION		5

// Version 4 power layout (before panel resetaction) - read only, for migration
typedef struct SPowerSettingsV4
{
	EPowerMode eMode;
	EPowerStartup eStartup;
	bool bLastOn;
	EPinPolarity eResetPolarity;
	EPinDrive eResetDrive;
	uint32_t u32ResetPulseMs;
	EPinPolarity ePsOnPolarity;
	EPinDrive ePsOnDrive;
	uint32_t u32ResetHoldMs;
	uint32_t u32PwrOkTimeoutMs;
	EPinPull eResetPull;
	EPinPolarity ePowerPolarity;
	EPinDrive ePowerDrive;
	EPinPull ePowerPull;
	EPanelAction ePowerAction;
	uint32_t u32PowerPulseMs;
	uint32_t u32ForceOffHoldMs;
	bool bSense;
	EPinPolarity eSensePolarity;
	uint32_t u32SenseTimeoutMs;
} SPowerSettingsV4;

typedef struct SSettingsV4
{
	uint32_t u32Magic;
	uint32_t u32Version;
	uint32_t u32Sequence;
	SWifiSettings sWifi;
	SPortSettings sPort[UART_PORT_COUNT];
	SPowerSettingsV4 sPower;
	uint32_t u32Crc;
} SSettingsV4;

// Version 3 layout (before power control) - read only, for migration
typedef struct SSettingsV3
{
	uint32_t u32Magic;
	uint32_t u32Version;
	uint32_t u32Sequence;
	SWifiSettings sWifi;
	SPortSettings sPort[UART_PORT_COUNT];
	uint32_t u32Crc;
} SSettingsV3;

// Version 2 layout (before port names) - read only, for migration
typedef struct SPortSettingsV2
{
	SUartLine sLine;
	uint16_t u16TcpPort;
	bool bClearOnConnect;
} SPortSettingsV2;

typedef struct SSettingsV2
{
	uint32_t u32Magic;
	uint32_t u32Version;
	uint32_t u32Sequence;
	SWifiSettings sWifi;
	SPortSettingsV2 sPort[UART_PORT_COUNT];
	uint32_t u32Crc;
} SSettingsV2;

// Version 1 layout (before clearonconnect) - read only, for migration
typedef struct SPortSettingsV1
{
	SUartLine sLine;
	uint16_t u16TcpPort;
} SPortSettingsV1;

typedef struct SSettingsV1
{
	uint32_t u32Magic;
	uint32_t u32Version;
	uint32_t u32Sequence;
	SWifiSettings sWifi;
	SPortSettingsV1 sPort[UART_PORT_COUNT];
	uint32_t u32Crc;
} SSettingsV1;

SSettings g_sSettings;

static const uint32_t sg_u32Offsets[2] =
{
	PICO_FLASH_SIZE_BYTES - (2 * FLASH_SECTOR_SIZE),
	PICO_FLASH_SIZE_BYTES - (1 * FLASH_SECTOR_SIZE),
};

// Index into sg_u32Offsets of the copy currently in use (-1 = none)
static int32_t sg_s32Active = -1;

// Sequence of the copy in use.  Kept here so SettingsDefaults() can't rewind it.
static uint32_t sg_u32Sequence;

static uint32_t Crc32(const uint8_t *pu8Data,
                      uint32_t u32Length)
{
	uint32_t u32Crc = 0xffffffff;
	uint32_t u32Loop;
	uint32_t u32Bit;

	for (u32Loop = 0; u32Loop < u32Length; u32Loop++)
	{
		u32Crc ^= pu8Data[u32Loop];
		for (u32Bit = 0; u32Bit < 8; u32Bit++)
		{
			if (u32Crc & 1)
			{
				u32Crc = (u32Crc >> 1) ^ 0xedb88320;
			}
			else
			{
				u32Crc >>= 1;
			}
		}
	}

	return(u32Crc ^ 0xffffffff);
}

static uint32_t SettingsCrc(const SSettings *psSettings)
{
	return(Crc32((const uint8_t *) psSettings, offsetof(SSettings, u32Crc)));
}

// Layout version of a valid stored copy, 0 if invalid
static uint32_t SettingsCopyVersion(const SSettings *psSettings)
{
	const SSettingsV1 *psV1 = (const SSettingsV1 *) psSettings;
	const SSettingsV2 *psV2 = (const SSettingsV2 *) psSettings;
	const SSettingsV3 *psV3 = (const SSettingsV3 *) psSettings;
	const SSettingsV4 *psV4 = (const SSettingsV4 *) psSettings;

	if (psSettings->u32Magic != SETTINGS_MAGIC)
	{
		return(0);
	}

	if ((SETTINGS_VERSION == psSettings->u32Version) &&
		(SettingsCrc(psSettings) == psSettings->u32Crc))
	{
		return(SETTINGS_VERSION);
	}

	if ((4 == psV4->u32Version) &&
		(Crc32((const uint8_t *) psV4, offsetof(SSettingsV4, u32Crc)) == psV4->u32Crc))
	{
		return(4);
	}

	if ((3 == psV3->u32Version) &&
		(Crc32((const uint8_t *) psV3, offsetof(SSettingsV3, u32Crc)) == psV3->u32Crc))
	{
		return(3);
	}

	if ((2 == psV2->u32Version) &&
		(Crc32((const uint8_t *) psV2, offsetof(SSettingsV2, u32Crc)) == psV2->u32Crc))
	{
		return(2);
	}

	if ((1 == psV1->u32Version) &&
		(Crc32((const uint8_t *) psV1, offsetof(SSettingsV1, u32Crc)) == psV1->u32Crc))
	{
		return(1);
	}

	return(0);
}

// Bring a version 1 copy forward; new fields get their defaults
static void SettingsMigrateV1(const SSettingsV1 *psV1)
{
	uint32_t u32Loop;

	SettingsDefaults(&g_sSettings);
	g_sSettings.u32Sequence = psV1->u32Sequence;
	g_sSettings.sWifi = psV1->sWifi;

	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		g_sSettings.sPort[u32Loop].sLine = psV1->sPort[u32Loop].sLine;
		g_sSettings.sPort[u32Loop].u16TcpPort = psV1->sPort[u32Loop].u16TcpPort;
	}
}

// Bring a version 2 copy forward; ports start unnamed
static void SettingsMigrateV2(const SSettingsV2 *psV2)
{
	uint32_t u32Loop;

	SettingsDefaults(&g_sSettings);
	g_sSettings.u32Sequence = psV2->u32Sequence;
	g_sSettings.sWifi = psV2->sWifi;

	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		g_sSettings.sPort[u32Loop].sLine = psV2->sPort[u32Loop].sLine;
		g_sSettings.sPort[u32Loop].u16TcpPort = psV2->sPort[u32Loop].u16TcpPort;
		g_sSettings.sPort[u32Loop].bClearOnConnect = psV2->sPort[u32Loop].bClearOnConnect;
	}
}

// Bring a version 4 copy forward; panel reset action defaults to pulse
static void SettingsMigrateV4(const SSettingsV4 *psV4)
{
	const SPowerSettingsV4 *psOld = &psV4->sPower;
	SPowerSettings *psPower = &g_sSettings.sPower;

	SettingsDefaults(&g_sSettings);
	g_sSettings.u32Sequence = psV4->u32Sequence;
	g_sSettings.sWifi = psV4->sWifi;
	memcpy(g_sSettings.sPort, psV4->sPort, sizeof(g_sSettings.sPort));

	psPower->eMode = psOld->eMode;
	psPower->eStartup = psOld->eStartup;
	psPower->bLastOn = psOld->bLastOn;
	psPower->eResetPolarity = psOld->eResetPolarity;
	psPower->eResetDrive = psOld->eResetDrive;
	psPower->u32ResetPulseMs = psOld->u32ResetPulseMs;
	psPower->ePsOnPolarity = psOld->ePsOnPolarity;
	psPower->ePsOnDrive = psOld->ePsOnDrive;
	psPower->u32ResetHoldMs = psOld->u32ResetHoldMs;
	psPower->u32PwrOkTimeoutMs = psOld->u32PwrOkTimeoutMs;
	psPower->eResetPull = psOld->eResetPull;
	psPower->ePowerPolarity = psOld->ePowerPolarity;
	psPower->ePowerDrive = psOld->ePowerDrive;
	psPower->ePowerPull = psOld->ePowerPull;
	psPower->ePowerAction = psOld->ePowerAction;
	psPower->u32PowerPulseMs = psOld->u32PowerPulseMs;
	psPower->u32ForceOffHoldMs = psOld->u32ForceOffHoldMs;
	psPower->bSense = psOld->bSense;
	psPower->eSensePolarity = psOld->eSensePolarity;
	psPower->u32SenseTimeoutMs = psOld->u32SenseTimeoutMs;
}

// Bring a version 3 copy forward; power control gets defaults
static void SettingsMigrateV3(const SSettingsV3 *psV3)
{
	SettingsDefaults(&g_sSettings);
	g_sSettings.u32Sequence = psV3->u32Sequence;
	g_sSettings.sWifi = psV3->sWifi;
	memcpy(g_sSettings.sPort, psV3->sPort, sizeof(g_sSettings.sPort));
}

void SettingsWifiDefaults(SWifiSettings *psWifi)
{
	memset(psWifi, 0, sizeof(*psWifi));
	psWifi->bEnabled = true;
	psWifi->bDhcp = true;
	psWifi->u32Channel = 0;
}

void SettingsPortDefaults(uint8_t u8Port,
                          SPortSettings *psPort)
{
	memset(psPort, 0, sizeof(*psPort));
	psPort->sLine.u32Baud = 115200;
	psPort->sLine.u8DataBits = 8;
	psPort->sLine.u8StopBits = 1;
	psPort->sLine.eParity = EPARITY_NONE;
	psPort->sLine.eFlow = EFLOW_NONE;
	psPort->u16TcpPort = CONFIG_TCP_PORT_BASE + u8Port;
	psPort->bClearOnConnect = true;
}

void SettingsPowerDefaults(SPowerSettings *psPower)
{
	memset(psPower, 0, sizeof(*psPower));

	// Global
	psPower->eMode = EPOWERMODE_PANEL;
	psPower->eStartup = EPOWERSTARTUP_OFF;
	psPower->bLastOn = false;
	psPower->eResetPolarity = EPINPOLARITY_LOW;
	psPower->eResetDrive = EPINDRIVE_OPENCOLLECTOR;
	psPower->u32ResetPulseMs = 200;

	// ATX
	psPower->ePsOnPolarity = EPINPOLARITY_LOW;
	psPower->ePsOnDrive = EPINDRIVE_PUSHPULL;
	psPower->u32ResetHoldMs = 150;
	psPower->u32PwrOkTimeoutMs = 2000;

	// Panel
	psPower->eResetAction = EPANELACTION_PULSE;
	psPower->eResetPull = EPINPULL_UP;
	psPower->ePowerPolarity = EPINPOLARITY_LOW;
	psPower->ePowerDrive = EPINDRIVE_OPENCOLLECTOR;
	psPower->ePowerPull = EPINPULL_UP;
	psPower->ePowerAction = EPANELACTION_PULSE;
	psPower->u32PowerPulseMs = 500;
	psPower->u32ForceOffHoldMs = 4500;
	psPower->bSense = false;
	psPower->eSensePolarity = EPINPOLARITY_HIGH;
	psPower->u32SenseTimeoutMs = 2000;
}

void SettingsDefaults(SSettings *psSettings)
{
	uint32_t u32Loop;

	memset(psSettings, 0, sizeof(*psSettings));
	psSettings->u32Magic = SETTINGS_MAGIC;
	psSettings->u32Version = SETTINGS_VERSION;
	SettingsWifiDefaults(&psSettings->sWifi);

	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		SettingsPortDefaults((uint8_t) u32Loop, &psSettings->sPort[u32Loop]);
	}

	SettingsPowerDefaults(&psSettings->sPower);
}

// Power settings out of range -> defaults
static void SettingsSanitizePower(SPowerSettings *psPower)
{
	if ((psPower->eMode > EPOWERMODE_ATX) ||
		(psPower->eStartup > EPOWERSTARTUP_LAST) ||
		(psPower->eResetPolarity > EPINPOLARITY_HIGH) ||
		(psPower->eResetDrive > EPINDRIVE_OPENCOLLECTOR) ||
		(psPower->ePsOnPolarity > EPINPOLARITY_HIGH) ||
		(psPower->ePsOnDrive > EPINDRIVE_OPENCOLLECTOR) ||
		(psPower->eResetPull > EPINPULL_DOWN) ||
		(psPower->ePowerPolarity > EPINPOLARITY_HIGH) ||
		(psPower->ePowerDrive > EPINDRIVE_OPENCOLLECTOR) ||
		(psPower->ePowerPull > EPINPULL_DOWN) ||
		(psPower->ePowerAction > EPANELACTION_LEVEL) ||
		(psPower->eResetAction > EPANELACTION_LEVEL) ||
		(psPower->eSensePolarity > EPINPOLARITY_HIGH) ||
		(0 == psPower->u32ResetPulseMs) ||
		(0 == psPower->u32PwrOkTimeoutMs) ||
		(0 == psPower->u32PowerPulseMs) ||
		(0 == psPower->u32ForceOffHoldMs) ||
		(0 == psPower->u32SenseTimeoutMs))
	{
		SettingsPowerDefaults(psPower);
	}
}

// Repair anything out of range rather than trusting flash blindly
static void SettingsSanitize(SSettings *psSettings)
{
	uint32_t u32Loop;

	psSettings->sWifi.eSsid[SETTINGS_SSID_MAX] = '\0';
	psSettings->sWifi.ePassword[SETTINGS_PASSWORD_MAX] = '\0';

	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		SUartLine *psLine = &psSettings->sPort[u32Loop].sLine;

		psSettings->sPort[u32Loop].eName[SETTINGS_PORT_NAME_MAX] = '\0';

		if ((false == UartBaudValid(psLine->u32Baud)) ||
			(psLine->u8DataBits < 5) || (psLine->u8DataBits > 8) ||
			(psLine->u8StopBits < 1) || (psLine->u8StopBits > 2) ||
			(psLine->eParity > EPARITY_SPACE) ||
			(psLine->eFlow > EFLOW_RTSCTS))
		{
			SettingsPortDefaults((uint8_t) u32Loop, &psSettings->sPort[u32Loop]);
		}
	}

	SettingsSanitizePower(&psSettings->sPower);
}

// Load the newest valid copy, or defaults if there is none
EStatus SettingsLoad(void)
{
	const SSettings *psCopy[2];
	uint32_t u32Version[2];
	int32_t s32Pick = -1;

	psCopy[0] = (const SSettings *) (XIP_BASE + sg_u32Offsets[0]);
	psCopy[1] = (const SSettings *) (XIP_BASE + sg_u32Offsets[1]);
	u32Version[0] = SettingsCopyVersion(psCopy[0]);
	u32Version[1] = SettingsCopyVersion(psCopy[1]);

	if (u32Version[0])
	{
		s32Pick = 0;
	}

	if (u32Version[1])
	{
		if ((-1 == s32Pick) ||
			((int32_t) (psCopy[1]->u32Sequence - psCopy[0]->u32Sequence) > 0))
		{
			s32Pick = 1;
		}
	}

	if (-1 == s32Pick)
	{
		SettingsDefaults(&g_sSettings);
		sg_s32Active = -1;
		return(ESTATUS_NOT_FOUND);
	}

	if (1 == u32Version[s32Pick])
	{
		SettingsMigrateV1((const SSettingsV1 *) psCopy[s32Pick]);
	}
	else
	if (2 == u32Version[s32Pick])
	{
		SettingsMigrateV2((const SSettingsV2 *) psCopy[s32Pick]);
	}
	else
	if (3 == u32Version[s32Pick])
	{
		SettingsMigrateV3((const SSettingsV3 *) psCopy[s32Pick]);
	}
	else
	if (4 == u32Version[s32Pick])
	{
		SettingsMigrateV4((const SSettingsV4 *) psCopy[s32Pick]);
	}
	else
	{
		memcpy(&g_sSettings, psCopy[s32Pick], sizeof(g_sSettings));
	}

	sg_s32Active = s32Pick;
	sg_u32Sequence = g_sSettings.u32Sequence;
	SettingsSanitize(&g_sSettings);
	return(ESTATUS_OK);
}

// Write g_sSettings to whichever sector is not the current copy
EStatus SettingsSave(void)
{
	EStatus eStatus = ESTATUS_OK;
	uint32_t u32Length = (sizeof(SSettings) + FLASH_PAGE_SIZE - 1) & ~((uint32_t) FLASH_PAGE_SIZE - 1);
	uint8_t *pu8Image = NULL;
	int32_t s32Target;
	uint32_t u32Interrupts;

	MEMALLOC(pu8Image, u32Length);
	if (NULL == pu8Image)
	{
		ERR_GOTO(ESTATUS_NO_MEMORY);
	}

	g_sSettings.u32Magic = SETTINGS_MAGIC;
	g_sSettings.u32Version = SETTINGS_VERSION;
	g_sSettings.u32Sequence = sg_u32Sequence + 1;
	memcpy(pu8Image, &g_sSettings, sizeof(g_sSettings));
	((SSettings *) pu8Image)->u32Crc = SettingsCrc((const SSettings *) pu8Image);
	g_sSettings.u32Crc = ((SSettings *) pu8Image)->u32Crc;

	s32Target = (0 == sg_s32Active) ? 1 : 0;

	u32Interrupts = save_and_disable_interrupts();
	flash_range_erase(sg_u32Offsets[s32Target], FLASH_SECTOR_SIZE);
	flash_range_program(sg_u32Offsets[s32Target], pu8Image, u32Length);
	restore_interrupts(u32Interrupts);

	if (memcmp((const void *) (XIP_BASE + sg_u32Offsets[s32Target]), pu8Image, u32Length))
	{
		ERR_GOTO(ESTATUS_FLASH);
	}

	sg_s32Active = s32Target;
	sg_u32Sequence = g_sSettings.u32Sequence;

errorExit:
	MEMFREE(pu8Image);
	if (eStatus != ESTATUS_OK)
	{
		LogErr("settings: save failed (%s)", StatusText(eStatus));
	}

	return(eStatus);
}
