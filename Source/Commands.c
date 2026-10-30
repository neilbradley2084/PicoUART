// Console commands.
//
// Every change is saved to flash and applied immediately:
//   wifi ssid/password/channel -> rejoin now
//   wifi dhcp/ip/netmask/gateway -> interface readdressed now
//   port line settings -> core 1 reloads the UART registers on its next pass
//   port tcpport -> listener moved now
//
// "set" takes any number of <name> <value> pairs and applies them together, so
// e.g. "wifi set ssid Foo password Bar" changes networks in one step.

#include <stdio.h>
#include <strings.h>
#include "Commands.h"
#include "Settings.h"
#include "Uart.h"
#include "Wifi.h"
#include "Bridge.h"
#include "Telnet.h"
#include "UsbConsole.h"
#include "Power.h"
#include "Main.h"
#include "Config.h"
#include "pico/stdlib.h"
#include "pico/version.h"
#include "hardware/clocks.h"

typedef void (*FCommand)(SConsoleSession *psSession,
                         uint32_t u32Argc,
                         char **ppeArgv);

typedef struct SCommand
{
	const char *peName;			// NULL terminates
	const char *peHelp;
	FCommand pHandler;
} SCommand;

typedef struct SSettingInfo
{
	const char *peName;
	const char *peArgs;
	const char *peHelp;
	int16_t s16Id;				// ((int16_t) -1) terminates
} SSettingInfo;

#define SETTING_END		((int16_t) -1)

typedef enum EWifiSetting
{
	EWIFISETTING_CHANNEL = 0,
	EWIFISETTING_DHCP,
	EWIFISETTING_GATEWAY,
	EWIFISETTING_IP,
	EWIFISETTING_NETMASK,
	EWIFISETTING_PASSWORD,
	EWIFISETTING_SSID,
} EWifiSetting;

typedef enum EPortSetting
{
	EPORTSETTING_BAUD = 0,
	EPORTSETTING_CLEARONCONNECT,
	EPORTSETTING_DATABITS,
	EPORTSETTING_FLOW,
	EPORTSETTING_NAME,
	EPORTSETTING_PARITY,
	EPORTSETTING_STOPBITS,
	EPORTSETTING_TCPPORT,
} EPortSetting;

static const SSettingInfo sg_sWifiSettings[] =
{
	{ "channel",	"auto | <1..13>",					"auto, or join only on that channel",			EWIFISETTING_CHANNEL },
	{ "dhcp",		"off | on",							"on = DHCP, off = static ip/netmask/gateway",	EWIFISETTING_DHCP },
	{ "gateway",	"<a.b.c.d>",						"static gateway (when dhcp is off)",			EWIFISETTING_GATEWAY },
	{ "ip",			"<a.b.c.d>",						"static address (when dhcp is off)",			EWIFISETTING_IP },
	{ "netmask",	"<a.b.c.d>",						"static netmask (when dhcp is off)",			EWIFISETTING_NETMASK },
	{ "password",	"<text, 8-63 characters>",			"WPA2 passphrase, \"\" for an open network",	EWIFISETTING_PASSWORD },
	{ "ssid",		"<text, up to 32 characters>",		"network name",									EWIFISETTING_SSID },
	{ NULL,			NULL,								NULL,											SETTING_END },
};

static const SSettingInfo sg_sPortSettings[] =
{
	{ "baud",		"<rate>",							"bits per second (rates listed below)",			EPORTSETTING_BAUD },
	{ "clearonconnect",	"on | off",						"on = discard serial data received with no client connected",	EPORTSETTING_CLEARONCONNECT },
	{ "databits",	"5 | 6 | 7 | 8",					"data bits per character",						EPORTSETTING_DATABITS },
	{ "flow",		"none | rtscts",					"hardware flow control",						EPORTSETTING_FLOW },
	{ "name",		"<text, up to 32 characters>",		"label for this port, \"\" for none",			EPORTSETTING_NAME },
	{ "parity",		"none | even | odd | mark | space",	"parity",										EPORTSETTING_PARITY },
	{ "stopbits",	"1 | 2",							"stop bits",									EPORTSETTING_STOPBITS },
	{ "tcpport",	"<0..65535>",						"TCP data port, 0 = no listener",				EPORTSETTING_TCPPORT },
	{ NULL,			NULL,								NULL,											SETTING_END },
};

static const char *sg_peParityNames[] = { "none", "even", "odd", "mark", "space" };
static const char sg_eParityLetters[] = "NEOMS";
static const char *sg_peFlowNames[] = { "none", "rtscts" };

// --------------------------------------------------------------------------
// Parsing / formatting helpers
// --------------------------------------------------------------------------

static bool ParseU32(const char *peText,
                     uint32_t *pu32Value)
{
	char *peEnd;
	unsigned long u32Value;

	if (('\0' == *peText) || ('-' == *peText))
	{
		return(false);
	}

	u32Value = strtoul(peText, &peEnd, 0);
	if (*peEnd != '\0')
	{
		return(false);
	}

	*pu32Value = (uint32_t) u32Value;
	return(true);
}

static bool ParseBool(const char *peText,
                      bool *pbValue)
{
	uint32_t u32Value;

	if ((0 == strcasecmp(peText, "on")) || (0 == strcasecmp(peText, "yes")) ||
		(0 == strcasecmp(peText, "true")) || (0 == strcasecmp(peText, "enable")))
	{
		*pbValue = true;
		return(true);
	}

	if ((0 == strcasecmp(peText, "off")) || (0 == strcasecmp(peText, "no")) ||
		(0 == strcasecmp(peText, "false")) || (0 == strcasecmp(peText, "disable")))
	{
		*pbValue = false;
		return(true);
	}

	if (ParseU32(peText, &u32Value))
	{
		*pbValue = (u32Value) ? true : false;
		return(true);
	}

	return(false);
}

// Dotted quad -> host order
static bool ParseIp(const char *peText,
                    uint32_t *pu32Ip)
{
	uint32_t u32Ip = 0;
	uint32_t u32Part;

	for (u32Part = 0; u32Part < 4; u32Part++)
	{
		uint32_t u32Octet = 0;
		uint32_t u32Digits = 0;

		while ((*peText >= '0') && (*peText <= '9'))
		{
			u32Octet = (u32Octet * 10) + (uint32_t) (*peText - '0');
			u32Digits++;
			peText++;
		}

		if ((0 == u32Digits) || (u32Digits > 3) || (u32Octet > 255))
		{
			return(false);
		}

		u32Ip = (u32Ip << 8) | u32Octet;

		if (u32Part < 3)
		{
			if (*peText != '.')
			{
				return(false);
			}

			peText++;
		}
	}

	if (*peText != '\0')
	{
		return(false);
	}

	*pu32Ip = u32Ip;
	return(true);
}

// Ones then zeros only
static bool NetmaskValid(uint32_t u32Mask)
{
	uint32_t u32Inverted = ~u32Mask;

	return((0 == (u32Inverted & (u32Inverted + 1))) ? true : false);
}

static int32_t NameLookup(const char *peText,
                          const char **ppeNames,
                          uint32_t u32Count)
{
	uint32_t u32Loop;

	for (u32Loop = 0; u32Loop < u32Count; u32Loop++)
	{
		if (0 == strcasecmp(peText, ppeNames[u32Loop]))
		{
			return((int32_t) u32Loop);
		}
	}

	return(-1);
}

// --------------------------------------------------------------------------
// Usage output
// --------------------------------------------------------------------------

static void UsageCommands(SConsoleSession *psSession,
                          const char *peHeader,
                          const SCommand *psTable)
{
	uint32_t u32Width = 0;
	uint32_t u32Loop;

	for (u32Loop = 0; psTable[u32Loop].peName; u32Loop++)
	{
		if (strlen(psTable[u32Loop].peName) > u32Width)
		{
			u32Width = strlen(psTable[u32Loop].peName);
		}
	}

	ConsolePrintf(psSession, "Usage: %s\n", peHeader);
	for (u32Loop = 0; psTable[u32Loop].peName; u32Loop++)
	{
		ConsolePrintf(psSession, "  %-*s  %s\n", (int) u32Width, psTable[u32Loop].peName, psTable[u32Loop].peHelp);
	}
}

static void UsageSettings(SConsoleSession *psSession,
                          const char *peHeader,
                          const SSettingInfo *psTable)
{
	uint32_t u32NameWidth = 0;
	uint32_t u32ArgsWidth = 0;
	uint32_t u32Loop;

	for (u32Loop = 0; psTable[u32Loop].s16Id != SETTING_END; u32Loop++)
	{
		if (strlen(psTable[u32Loop].peName) > u32NameWidth)
		{
			u32NameWidth = strlen(psTable[u32Loop].peName);
		}

		if (strlen(psTable[u32Loop].peArgs) > u32ArgsWidth)
		{
			u32ArgsWidth = strlen(psTable[u32Loop].peArgs);
		}
	}

	ConsolePrintf(psSession, "Usage: %s\n", peHeader);
	for (u32Loop = 0; psTable[u32Loop].s16Id != SETTING_END; u32Loop++)
	{
		ConsolePrintf(psSession, "  %-*s  %-*s  %s\n",
					  (int) u32NameWidth, psTable[u32Loop].peName,
					  (int) u32ArgsWidth, psTable[u32Loop].peArgs,
					  psTable[u32Loop].peHelp);
	}
}

static const SSettingInfo *SettingFind(const SSettingInfo *psTable,
                                       const char *peName)
{
	uint32_t u32Loop;

	for (u32Loop = 0; psTable[u32Loop].s16Id != SETTING_END; u32Loop++)
	{
		if (0 == strcasecmp(peName, psTable[u32Loop].peName))
		{
			return(&psTable[u32Loop]);
		}
	}

	return(NULL);
}

static const SCommand *CommandFind(const SCommand *psTable,
                                   const char *peName)
{
	uint32_t u32Loop;

	for (u32Loop = 0; psTable[u32Loop].peName; u32Loop++)
	{
		if (0 == strcasecmp(peName, psTable[u32Loop].peName))
		{
			return(&psTable[u32Loop]);
		}
	}

	return(NULL);
}

// --------------------------------------------------------------------------
// wifi
// --------------------------------------------------------------------------

static bool WifiSettingParse(SConsoleSession *psSession,
                             int16_t s16Id,
                             const char *peValue,
                             SWifiSettings *psWifi)
{
	uint32_t u32Value;
	uint32_t u32Length = strlen(peValue);

	switch ((EWifiSetting) s16Id)
	{
		case EWIFISETTING_CHANNEL:
		{
			if ((0 == strcasecmp(peValue, "auto")) || (0 == strcmp(peValue, "0")))
			{
				psWifi->u32Channel = 0;
				return(true);
			}

			if ((false == ParseU32(peValue, &u32Value)) || (u32Value < 1) || (u32Value > 13))
			{
				ConsolePrintf(psSession, "channel must be auto or 1-13\n");
				return(false);
			}

			psWifi->u32Channel = u32Value;
			return(true);
		}

		case EWIFISETTING_DHCP:
		{
			if (false == ParseBool(peValue, &psWifi->bDhcp))
			{
				ConsolePrintf(psSession, "dhcp must be on or off\n");
				return(false);
			}

			return(true);
		}

		case EWIFISETTING_GATEWAY:
		{
			if (false == ParseIp(peValue, &psWifi->u32Gateway))
			{
				ConsolePrintf(psSession, "gateway must be a.b.c.d\n");
				return(false);
			}

			return(true);
		}

		case EWIFISETTING_IP:
		{
			if (false == ParseIp(peValue, &psWifi->u32Ip))
			{
				ConsolePrintf(psSession, "ip must be a.b.c.d\n");
				return(false);
			}

			return(true);
		}

		case EWIFISETTING_NETMASK:
		{
			if ((false == ParseIp(peValue, &u32Value)) || (false == NetmaskValid(u32Value)))
			{
				ConsolePrintf(psSession, "netmask must be a contiguous a.b.c.d mask\n");
				return(false);
			}

			psWifi->u32Netmask = u32Value;
			return(true);
		}

		case EWIFISETTING_PASSWORD:
		{
			if ((u32Length) &&
				((u32Length < SETTINGS_PASSWORD_MIN) || (u32Length > SETTINGS_PASSWORD_MAX)))
			{
				ConsolePrintf(psSession, "password must be %u-%u characters, or \"\" for none\n",
							  SETTINGS_PASSWORD_MIN, SETTINGS_PASSWORD_MAX);
				return(false);
			}

			strcpy(psWifi->ePassword, peValue);
			return(true);
		}

		case EWIFISETTING_SSID:
		{
			if (u32Length > SETTINGS_SSID_MAX)
			{
				ConsolePrintf(psSession, "ssid is limited to %u characters\n", SETTINGS_SSID_MAX);
				return(false);
			}

			strcpy(psWifi->eSsid, peValue);
			return(true);
		}
	}

	return(false);
}

static void WifiSettingFormat(int16_t s16Id,
                              const SWifiSettings *psWifi,
                              char *peBuffer,
                              uint32_t u32Size)
{
	switch ((EWifiSetting) s16Id)
	{
		case EWIFISETTING_CHANNEL:
		{
			if (0 == psWifi->u32Channel)
			{
				snprintf(peBuffer, u32Size, "auto");
			}
			else
			{
				snprintf(peBuffer, u32Size, "%lu", (unsigned long) psWifi->u32Channel);
			}

			break;
		}

		case EWIFISETTING_DHCP:
		{
			snprintf(peBuffer, u32Size, "%s", psWifi->bDhcp ? "on" : "off");
			break;
		}

		case EWIFISETTING_GATEWAY:
		{
			FormatIp(psWifi->u32Gateway, peBuffer, u32Size);
			break;
		}

		case EWIFISETTING_IP:
		{
			FormatIp(psWifi->u32Ip, peBuffer, u32Size);
			break;
		}

		case EWIFISETTING_NETMASK:
		{
			FormatIp(psWifi->u32Netmask, peBuffer, u32Size);
			break;
		}

		case EWIFISETTING_PASSWORD:
		{
			snprintf(peBuffer, u32Size, "%s", ('\0' == psWifi->ePassword[0]) ? "(none)" : "********");
			break;
		}

		case EWIFISETTING_SSID:
		{
			snprintf(peBuffer, u32Size, "\"%s\"", psWifi->eSsid);
			break;
		}
	}
}

static void WifiSettingReset(int16_t s16Id,
                             SWifiSettings *psWifi)
{
	SWifiSettings sDefault;

	SettingsWifiDefaults(&sDefault);

	switch ((EWifiSetting) s16Id)
	{
		case EWIFISETTING_CHANNEL:	psWifi->u32Channel = sDefault.u32Channel;	break;
		case EWIFISETTING_DHCP:		psWifi->bDhcp = sDefault.bDhcp;				break;
		case EWIFISETTING_GATEWAY:	psWifi->u32Gateway = sDefault.u32Gateway;	break;
		case EWIFISETTING_IP:		psWifi->u32Ip = sDefault.u32Ip;				break;
		case EWIFISETTING_NETMASK:	psWifi->u32Netmask = sDefault.u32Netmask;	break;
		case EWIFISETTING_PASSWORD:	strcpy(psWifi->ePassword, sDefault.ePassword); break;
		case EWIFISETTING_SSID:		strcpy(psWifi->eSsid, sDefault.eSsid);		break;
	}
}

static void WifiCommit(SConsoleSession *psSession,
                       const SWifiSettings *psNew)
{
	SWifiSettings sOld = g_sSettings.sWifi;

	if ((false == psNew->bDhcp) && ((0 == psNew->u32Ip) || (0 == psNew->u32Netmask)))
	{
		ConsolePrintf(psSession, "Note: dhcp is off but ip/netmask are not set\n");
	}

	g_sSettings.sWifi = *psNew;
	(void) SettingsSave();
	WifiApply(&sOld, psSession);
}

static void CmdWifiDisable(SConsoleSession *psSession,
                           uint32_t u32Argc,
                           char **ppeArgv)
{
	SWifiSettings sNew = g_sSettings.sWifi;

	sNew.bEnabled = false;
	WifiCommit(psSession, &sNew);
}

static void CmdWifiEnable(SConsoleSession *psSession,
                          uint32_t u32Argc,
                          char **ppeArgv)
{
	SWifiSettings sNew = g_sSettings.sWifi;

	if (g_sSettings.sWifi.bEnabled)
	{
		WifiEnable(true);
		return;
	}

	sNew.bEnabled = true;
	WifiCommit(psSession, &sNew);
}

static void CmdWifiGet(SConsoleSession *psSession,
                       uint32_t u32Argc,
                       char **ppeArgv)
{
	const SSettingInfo *psInfo;
	char eValue[48];
	uint32_t u32Loop;

	if (u32Argc > 2)
	{
		ConsolePrintf(psSession, "Usage: wifi get [<name>]\n");
		return;
	}

	if (2 == u32Argc)
	{
		psInfo = SettingFind(sg_sWifiSettings, ppeArgv[1]);
		if (NULL == psInfo)
		{
			ConsolePrintf(psSession, "Unknown setting '%s'\n", ppeArgv[1]);
			return;
		}

		WifiSettingFormat(psInfo->s16Id, &g_sSettings.sWifi, eValue, sizeof(eValue));
		ConsolePrintf(psSession, "%s\n", eValue);
		return;
	}

	ConsolePrintf(psSession, "  %-10s %s\n", "enabled", g_sSettings.sWifi.bEnabled ? "yes" : "no");
	for (u32Loop = 0; sg_sWifiSettings[u32Loop].s16Id != SETTING_END; u32Loop++)
	{
		WifiSettingFormat(sg_sWifiSettings[u32Loop].s16Id, &g_sSettings.sWifi, eValue, sizeof(eValue));
		ConsolePrintf(psSession, "  %-10s %s\n", sg_sWifiSettings[u32Loop].peName, eValue);
	}
}

static void CmdWifiReconnect(SConsoleSession *psSession,
                             uint32_t u32Argc,
                             char **ppeArgv)
{
	if (false == g_sSettings.sWifi.bEnabled)
	{
		ConsolePrintf(psSession, "WiFi is disabled - use 'wifi enable'\n");
		return;
	}

	if (ECONSOLETYPE_TELNET == psSession->eType)
	{
		ConsolePrintf(psSession, "Rejoining - this telnet session will drop\n");
	}

	WifiReconnect(true);
}

static void CmdWifiReset(SConsoleSession *psSession,
                         uint32_t u32Argc,
                         char **ppeArgv)
{
	SWifiSettings sNew = g_sSettings.sWifi;
	const SSettingInfo *psInfo;
	uint32_t u32Loop;

	if (1 == u32Argc)
	{
		SettingsWifiDefaults(&sNew);
		WifiCommit(psSession, &sNew);
		return;
	}

	for (u32Loop = 1; u32Loop < u32Argc; u32Loop++)
	{
		psInfo = SettingFind(sg_sWifiSettings, ppeArgv[u32Loop]);
		if (NULL == psInfo)
		{
			ConsolePrintf(psSession, "Unknown setting '%s'\n", ppeArgv[u32Loop]);
			return;
		}

		WifiSettingReset(psInfo->s16Id, &sNew);
	}

	WifiCommit(psSession, &sNew);
}

static void CmdWifiScan(SConsoleSession *psSession,
                        uint32_t u32Argc,
                        char **ppeArgv)
{
	EStatus eStatus = WifiScanStart(psSession);

	if (eStatus != ESTATUS_OK)
	{
		ConsolePrintf(psSession, "Scan not started (%s)\n", StatusText(eStatus));
		return;
	}

	ConsolePrintf(psSession, "Scanning...\n");
}

static void CmdWifiSet(SConsoleSession *psSession,
                       uint32_t u32Argc,
                       char **ppeArgv)
{
	SWifiSettings sNew = g_sSettings.sWifi;
	const SSettingInfo *psInfo;
	uint32_t u32Loop;

	if ((u32Argc < 3) || (0 == (u32Argc & 1)))
	{
		UsageSettings(psSession, "wifi set <name> <value> [<name> <value> ...]", sg_sWifiSettings);
		return;
	}

	for (u32Loop = 1; u32Loop < u32Argc; u32Loop += 2)
	{
		psInfo = SettingFind(sg_sWifiSettings, ppeArgv[u32Loop]);
		if (NULL == psInfo)
		{
			ConsolePrintf(psSession, "Unknown setting '%s' - nothing changed\n", ppeArgv[u32Loop]);
			return;
		}

		if (false == WifiSettingParse(psSession, psInfo->s16Id, ppeArgv[u32Loop + 1], &sNew))
		{
			ConsolePrintf(psSession, "Nothing changed\n");
			return;
		}
	}

	WifiCommit(psSession, &sNew);
}

static void CmdWifiStats(SConsoleSession *psSession,
                         uint32_t u32Argc,
                         char **ppeArgv)
{
	WifiStatsPrint(psSession);
}

static const SCommand sg_sWifiCommands[] =
{
	{ "disable",	"Disconnect and stay off",								CmdWifiDisable },
	{ "enable",		"Connect again (resets statistics)",					CmdWifiEnable },
	{ "get",		"get [<name>]",											CmdWifiGet },
	{ "reconnect",	"Drop and rejoin (resets statistics)",					CmdWifiReconnect },
	{ "reset",		"reset [<name> ...]  (back to defaults)",				CmdWifiReset },
	{ "scan",		"List networks in range",								CmdWifiScan },
	{ "set",		"set <name> <value> [<name> <value> ...] | set ?",		CmdWifiSet },
	{ "stats",		"Connection details and statistics",					CmdWifiStats },
	{ NULL,			NULL,													NULL },
};

static void CmdWifi(SConsoleSession *psSession,
                    uint32_t u32Argc,
                    char **ppeArgv)
{
	const SCommand *psCommand = NULL;

	if (u32Argc >= 2)
	{
		psCommand = CommandFind(sg_sWifiCommands, ppeArgv[1]);
	}

	if (NULL == psCommand)
	{
		UsageCommands(psSession, "wifi <subcommand>", sg_sWifiCommands);
		return;
	}

	psCommand->pHandler(psSession, u32Argc - 1, &ppeArgv[1]);
}

// --------------------------------------------------------------------------
// port
// --------------------------------------------------------------------------

static void PortBaudList(SConsoleSession *psSession)
{
	uint32_t u32Loop;

	ConsolePrintf(psSession, "  Baud rates:");
	for (u32Loop = 0; g_u32UartBaudRates[u32Loop]; u32Loop++)
	{
		if (0 == (u32Loop % 10))
		{
			ConsolePrintf(psSession, "\n   ");
		}

		ConsolePrintf(psSession, " %lu", (unsigned long) g_u32UartBaudRates[u32Loop]);
	}

	ConsolePrintf(psSession, "\n");
}

static bool PortSettingParse(SConsoleSession *psSession,
                             int16_t s16Id,
                             const char *peValue,
                             SPortSettings *psPort)
{
	uint32_t u32Value;
	int32_t s32Index;

	switch ((EPortSetting) s16Id)
	{
		case EPORTSETTING_BAUD:
		{
			if ((false == ParseU32(peValue, &u32Value)) || (false == UartBaudValid(u32Value)))
			{
				ConsolePrintf(psSession, "Unsupported baud rate '%s'\n", peValue);
				PortBaudList(psSession);
				return(false);
			}

			psPort->sLine.u32Baud = u32Value;
			return(true);
		}

		case EPORTSETTING_CLEARONCONNECT:
		{
			if (false == ParseBool(peValue, &psPort->bClearOnConnect))
			{
				ConsolePrintf(psSession, "clearonconnect must be on or off\n");
				return(false);
			}

			return(true);
		}

		case EPORTSETTING_DATABITS:
		{
			if ((false == ParseU32(peValue, &u32Value)) || (u32Value < 5) || (u32Value > 8))
			{
				ConsolePrintf(psSession, "databits must be 5-8\n");
				return(false);
			}

			psPort->sLine.u8DataBits = (uint8_t) u32Value;
			return(true);
		}

		case EPORTSETTING_FLOW:
		{
			s32Index = NameLookup(peValue, sg_peFlowNames, sizeof(sg_peFlowNames) / sizeof(sg_peFlowNames[0]));
			if (s32Index < 0)
			{
				ConsolePrintf(psSession, "flow must be none or rtscts\n");
				return(false);
			}

			psPort->sLine.eFlow = (EFlow) s32Index;
			return(true);
		}

		case EPORTSETTING_NAME:
		{
			if (strlen(peValue) > SETTINGS_PORT_NAME_MAX)
			{
				ConsolePrintf(psSession, "name is limited to %u characters\n", SETTINGS_PORT_NAME_MAX);
				return(false);
			}

			strcpy(psPort->eName, peValue);
			return(true);
		}

		case EPORTSETTING_PARITY:
		{
			s32Index = NameLookup(peValue, sg_peParityNames, sizeof(sg_peParityNames) / sizeof(sg_peParityNames[0]));
			if (s32Index < 0)
			{
				ConsolePrintf(psSession, "parity must be none, even, odd, mark or space\n");
				return(false);
			}

			psPort->sLine.eParity = (EParity) s32Index;
			return(true);
		}

		case EPORTSETTING_STOPBITS:
		{
			if ((false == ParseU32(peValue, &u32Value)) || (u32Value < 1) || (u32Value > 2))
			{
				ConsolePrintf(psSession, "stopbits must be 1 or 2\n");
				return(false);
			}

			psPort->sLine.u8StopBits = (uint8_t) u32Value;
			return(true);
		}

		case EPORTSETTING_TCPPORT:
		{
			if ((false == ParseU32(peValue, &u32Value)) || (u32Value > 65535))
			{
				ConsolePrintf(psSession, "tcpport must be 0-65535\n");
				return(false);
			}

			psPort->u16TcpPort = (uint16_t) u32Value;
			return(true);
		}
	}

	return(false);
}

static void PortSettingFormat(int16_t s16Id,
                              const SPortSettings *psPort,
                              char *peBuffer,
                              uint32_t u32Size)
{
	switch ((EPortSetting) s16Id)
	{
		case EPORTSETTING_BAUD:
		{
			snprintf(peBuffer, u32Size, "%lu", (unsigned long) psPort->sLine.u32Baud);
			break;
		}

		case EPORTSETTING_CLEARONCONNECT:
		{
			snprintf(peBuffer, u32Size, "%s", psPort->bClearOnConnect ? "on" : "off");
			break;
		}

		case EPORTSETTING_DATABITS:
		{
			snprintf(peBuffer, u32Size, "%u", psPort->sLine.u8DataBits);
			break;
		}

		case EPORTSETTING_FLOW:
		{
			snprintf(peBuffer, u32Size, "%s", sg_peFlowNames[psPort->sLine.eFlow]);
			break;
		}

		case EPORTSETTING_NAME:
		{
			snprintf(peBuffer, u32Size, "\"%s\"", psPort->eName);
			break;
		}

		case EPORTSETTING_PARITY:
		{
			snprintf(peBuffer, u32Size, "%s", sg_peParityNames[psPort->sLine.eParity]);
			break;
		}

		case EPORTSETTING_STOPBITS:
		{
			snprintf(peBuffer, u32Size, "%u", psPort->sLine.u8StopBits);
			break;
		}

		case EPORTSETTING_TCPPORT:
		{
			if (0 == psPort->u16TcpPort)
			{
				snprintf(peBuffer, u32Size, "0 (off)");
			}
			else
			{
				snprintf(peBuffer, u32Size, "%u", psPort->u16TcpPort);
			}

			break;
		}
	}
}

static void PortSettingReset(uint8_t u8Port,
                             int16_t s16Id,
                             SPortSettings *psPort)
{
	SPortSettings sDefault;

	SettingsPortDefaults(u8Port, &sDefault);

	switch ((EPortSetting) s16Id)
	{
		case EPORTSETTING_BAUD:		psPort->sLine.u32Baud = sDefault.sLine.u32Baud;			break;
		case EPORTSETTING_CLEARONCONNECT:	psPort->bClearOnConnect = sDefault.bClearOnConnect;	break;
		case EPORTSETTING_DATABITS:	psPort->sLine.u8DataBits = sDefault.sLine.u8DataBits;	break;
		case EPORTSETTING_FLOW:		psPort->sLine.eFlow = sDefault.sLine.eFlow;				break;
		case EPORTSETTING_NAME:		strcpy(psPort->eName, sDefault.eName);					break;
		case EPORTSETTING_PARITY:	psPort->sLine.eParity = sDefault.sLine.eParity;			break;
		case EPORTSETTING_STOPBITS:	psPort->sLine.u8StopBits = sDefault.sLine.u8StopBits;	break;
		case EPORTSETTING_TCPPORT:	psPort->u16TcpPort = sDefault.u16TcpPort;				break;
	}
}

static void PortCommit(SConsoleSession *psSession,
                       uint8_t u8Port,
                       const SPortSettings *psNew)
{
	SPortSettings sOld = g_sSettings.sPort[u8Port];
	uint32_t u32Loop;

	// TCP port must not collide with telnet, the web page or the other bridge
	if (psNew->u16TcpPort)
	{
		if (CONFIG_TELNET_PORT == psNew->u16TcpPort)
		{
			ConsolePrintf(psSession, "TCP port %u is the telnet console - nothing changed\n", psNew->u16TcpPort);
			return;
		}

		if (CONFIG_HTTP_PORT == psNew->u16TcpPort)
		{
			ConsolePrintf(psSession, "TCP port %u is the web page - nothing changed\n", psNew->u16TcpPort);
			return;
		}

		for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
		{
			if ((u32Loop != u8Port) && (g_sSettings.sPort[u32Loop].u16TcpPort == psNew->u16TcpPort))
			{
				ConsolePrintf(psSession, "TCP port %u is used by port %lu - nothing changed\n",
							  psNew->u16TcpPort, (unsigned long) u32Loop);
				return;
			}
		}
	}

	g_sSettings.sPort[u8Port] = *psNew;
	(void) SettingsSave();

	if (memcmp(&sOld.sLine, &psNew->sLine, sizeof(sOld.sLine)))
	{
		UartConfigure(u8Port, &psNew->sLine);
	}

	if (sOld.u16TcpPort != psNew->u16TcpPort)
	{
		BridgeApplyPort(u8Port);
	}
}

// Port subcommands: argv[0] is the subcommand name, the port number is passed separately
typedef void (*FPortCommand)(SConsoleSession *psSession,
                             uint8_t u8Port,
                             uint32_t u32Argc,
                             char **ppeArgv);

typedef struct SPortCommand
{
	const char *peName;			// NULL terminates
	const char *peHelp;
	FPortCommand pHandler;
} SPortCommand;

static void CmdPortClear(SConsoleSession *psSession,
                         uint8_t u8Port,
                         uint32_t u32Argc,
                         char **ppeArgv)
{
	UartStatsClear(u8Port);
	BridgeStatsClear(u8Port);
}

static void CmdPortDisconnect(SConsoleSession *psSession,
                              uint8_t u8Port,
                              uint32_t u32Argc,
                              char **ppeArgv)
{
	BridgeDisconnect(u8Port);
}

static void CmdPortGet(SConsoleSession *psSession,
                       uint8_t u8Port,
                       uint32_t u32Argc,
                       char **ppeArgv)
{
	const SSettingInfo *psInfo;
	char eValue[SETTINGS_PORT_NAME_MAX + 8];
	uint32_t u32Loop;

	if (u32Argc > 2)
	{
		ConsolePrintf(psSession, "Usage: port %u get [<name>]\n", u8Port);
		return;
	}

	if (2 == u32Argc)
	{
		psInfo = SettingFind(sg_sPortSettings, ppeArgv[1]);
		if (NULL == psInfo)
		{
			ConsolePrintf(psSession, "Unknown setting '%s'\n", ppeArgv[1]);
			return;
		}

		PortSettingFormat(psInfo->s16Id, &g_sSettings.sPort[u8Port], eValue, sizeof(eValue));
		ConsolePrintf(psSession, "%s\n", eValue);
		return;
	}

	for (u32Loop = 0; sg_sPortSettings[u32Loop].s16Id != SETTING_END; u32Loop++)
	{
		PortSettingFormat(sg_sPortSettings[u32Loop].s16Id, &g_sSettings.sPort[u8Port], eValue, sizeof(eValue));
		ConsolePrintf(psSession, "  %-14s %s\n", sg_sPortSettings[u32Loop].peName, eValue);
	}
}

static void CmdPortReset(SConsoleSession *psSession,
                         uint8_t u8Port,
                         uint32_t u32Argc,
                         char **ppeArgv)
{
	SPortSettings sNew = g_sSettings.sPort[u8Port];
	const SSettingInfo *psInfo;
	uint32_t u32Loop;

	if (1 == u32Argc)
	{
		SettingsPortDefaults(u8Port, &sNew);
		PortCommit(psSession, u8Port, &sNew);
		return;
	}

	for (u32Loop = 1; u32Loop < u32Argc; u32Loop++)
	{
		psInfo = SettingFind(sg_sPortSettings, ppeArgv[u32Loop]);
		if (NULL == psInfo)
		{
			ConsolePrintf(psSession, "Unknown setting '%s'\n", ppeArgv[u32Loop]);
			return;
		}

		PortSettingReset(u8Port, psInfo->s16Id, &sNew);
	}

	PortCommit(psSession, u8Port, &sNew);
}

static void CmdPortSet(SConsoleSession *psSession,
                       uint8_t u8Port,
                       uint32_t u32Argc,
                       char **ppeArgv)
{
	SPortSettings sNew = g_sSettings.sPort[u8Port];
	const SSettingInfo *psInfo;
	uint32_t u32Loop;

	if ((u32Argc < 3) || (0 == (u32Argc & 1)))
	{
		UsageSettings(psSession, "port <n> set <name> <value> [<name> <value> ...]", sg_sPortSettings);
		PortBaudList(psSession);
		return;
	}

	for (u32Loop = 1; u32Loop < u32Argc; u32Loop += 2)
	{
		psInfo = SettingFind(sg_sPortSettings, ppeArgv[u32Loop]);
		if (NULL == psInfo)
		{
			ConsolePrintf(psSession, "Unknown setting '%s' - nothing changed\n", ppeArgv[u32Loop]);
			return;
		}

		if (false == PortSettingParse(psSession, psInfo->s16Id, ppeArgv[u32Loop + 1], &sNew))
		{
			ConsolePrintf(psSession, "Nothing changed\n");
			return;
		}
	}

	PortCommit(psSession, u8Port, &sNew);
}

static void CmdPortStats(SConsoleSession *psSession,
                         uint8_t u8Port,
                         uint32_t u32Argc,
                         char **ppeArgv)
{
	const SUartLine *psLine = &g_sSettings.sPort[u8Port].sLine;
	const SUartPins *psPins = &g_sUartPins[u8Port];
	SUartPort *psPort = &g_sUartPorts[u8Port];
	SUartStats sStats;

	UartStatsGet(u8Port, &sStats);

	ConsolePrintf(psSession, "Name         \"%s\"\n", g_sSettings.sPort[u8Port].eName);
	ConsolePrintf(psSession, "Pins         TX GP%u  RX GP%u  CTS GP%u  RTS GP%u\n",
				  psPins->u8Tx, psPins->u8Rx, psPins->u8Cts, psPins->u8Rts);
	ConsolePrintf(psSession, "Line         %lu %u%c%u, flow %s (actual %lu)\n",
				  (unsigned long) psLine->u32Baud,
				  psLine->u8DataBits,
				  sg_eParityLetters[psLine->eParity],
				  psLine->u8StopBits,
				  sg_peFlowNames[psLine->eFlow],
				  (unsigned long) psPort->u32ActualBaud);
	ConsolePrintf(psSession, "CTS          %s\n", UartCtsAsserted(u8Port) ? "asserted" : "deasserted");
	ConsolePrintf(psSession, "UART rx      %lu bytes\n", (unsigned long) sStats.u32RxBytes);
	ConsolePrintf(psSession, "UART tx      %lu bytes\n", (unsigned long) sStats.u32TxBytes);
	ConsolePrintf(psSession, "Errors       overrun %lu, framing %lu, parity %lu, break %lu\n",
				  (unsigned long) sStats.u32Overrun,
				  (unsigned long) sStats.u32Framing,
				  (unsigned long) sStats.u32Parity,
				  (unsigned long) sStats.u32Break);
	ConsolePrintf(psSession, "Dropped      %lu bytes (rx buffer full)\n", (unsigned long) sStats.u32RxDropped);
	ConsolePrintf(psSession, "Rx buffer    %lu of %lu KB used\n",
				  (unsigned long) (RingUsed(&psPort->sRx) / 1024),
				  (unsigned long) (psPort->sRx.u32Size / 1024));
	ConsolePrintf(psSession, "Tx buffer    %lu of %lu KB used\n",
				  (unsigned long) (RingUsed(&psPort->sTx) / 1024),
				  (unsigned long) (psPort->sTx.u32Size / 1024));
	BridgeStatsPrint(u8Port, psSession);
}

static const SPortCommand sg_sPortCommands[] =
{
	{ "clear",		"Reset statistics",										CmdPortClear },
	{ "disconnect",	"Drop the TCP client",									CmdPortDisconnect },
	{ "get",		"get [<name>]",											CmdPortGet },
	{ "reset",		"reset [<name> ...]  (back to defaults)",				CmdPortReset },
	{ "set",		"set <name> <value> [<name> <value> ...] | set ?",		CmdPortSet },
	{ "stats",		"Line, buffer and connection statistics",				CmdPortStats },
	{ NULL,			NULL,													NULL },
};

static void PortUsage(SConsoleSession *psSession)
{
	uint32_t u32Loop;

	ConsolePrintf(psSession, "Usage: port <0..%u> <subcommand>\n", UART_PORT_COUNT - 1);
	for (u32Loop = 0; sg_sPortCommands[u32Loop].peName; u32Loop++)
	{
		ConsolePrintf(psSession, "  %-10s  %s\n", sg_sPortCommands[u32Loop].peName, sg_sPortCommands[u32Loop].peHelp);
	}
}

static void CmdPort(SConsoleSession *psSession,
                    uint32_t u32Argc,
                    char **ppeArgv)
{
	uint32_t u32Port;
	uint32_t u32Loop;

	if ((u32Argc < 3) ||
		(false == ParseU32(ppeArgv[1], &u32Port)) ||
		(u32Port >= UART_PORT_COUNT))
	{
		PortUsage(psSession);
		return;
	}

	for (u32Loop = 0; sg_sPortCommands[u32Loop].peName; u32Loop++)
	{
		if (0 == strcasecmp(ppeArgv[2], sg_sPortCommands[u32Loop].peName))
		{
			sg_sPortCommands[u32Loop].pHandler(psSession, (uint8_t) u32Port, u32Argc - 2, &ppeArgv[2]);
			return;
		}
	}

	PortUsage(psSession);
}

// --------------------------------------------------------------------------
// power
// --------------------------------------------------------------------------

typedef enum EPowerKind
{
	EPOWERKIND_ENUM = 0,			// One of ppeNames, stored as an enum
	EPOWERKIND_BOOL,				// off / on
	EPOWERKIND_MS,					// Milliseconds, > 0
	EPOWERKIND_MS0,					// Milliseconds, may be 0
	EPOWERKIND_MS100,				// Milliseconds, > 0, multiple of 100
} EPowerKind;

typedef struct SPowerSettingInfo
{
	const char *peName;
	const char *peArgs;
	const char *peHelp;
	EPowerKind eKind;
	const char **ppeNames;
	uint32_t u32NameCount;
	size_t eOffset;					// Into SPowerSettings
	size_t eSize;					// Field size (enums are packed by this toolchain)
	int16_t s16Group;				// 0 global, 1 ATX, 2 panel; ((int16_t) -1) terminates
} SPowerSettingInfo;

static const char *sg_pePowerModeNames[] = { "panel", "atx" };
static const char *sg_pePowerStartupNames[] = { "off", "on", "last" };
static const char *sg_pePolarityNames[] = { "low", "high" };
static const char *sg_peDriveNames[] = { "pushpull", "opencollector" };
static const char *sg_pePullNames[] = { "none", "up", "down" };
static const char *sg_peActionNames[] = { "pulse", "level" };
static const char *sg_peGroupNames[] = { "Global", "ATX mode", "Panel mode" };

#define POWER_ENUM(n)	EPOWERKIND_ENUM, n, (sizeof(n) / sizeof(n[0]))
#define POWER_PLAIN(k)	k, NULL, 0
#define POWER_FIELD(f)	offsetof(SPowerSettings, f), sizeof(((SPowerSettings *) 0)->f)

static const SPowerSettingInfo sg_sPowerSettings[] =
{
	{ "mode",			"panel | atx",					"panel = front panel buttons, atx = PS_ON#/PWR_OK (target must be off)",
		POWER_ENUM(sg_pePowerModeNames),	POWER_FIELD(eMode),				0 },
	{ "startup",		"off | on | last",				"power state applied when this device starts",
		POWER_ENUM(sg_pePowerStartupNames),	POWER_FIELD(eStartup),			0 },
	{ "resetpolarity",	"low | high",					"reset asserted level",
		POWER_ENUM(sg_pePolarityNames),		POWER_FIELD(eResetPolarity),	0 },
	{ "resetdrive",		"pushpull | opencollector",		"reset drive (panel pulse reset is always open collector)",
		POWER_ENUM(sg_peDriveNames),		POWER_FIELD(eResetDrive),		0 },
	{ "resetpulse",		"<ms>",							"'power reset' pulse length",
		POWER_PLAIN(EPOWERKIND_MS),			POWER_FIELD(u32ResetPulseMs),	0 },
	{ "resethold",		"<ms>",							"reset held after power comes up (ATX, panel level reset)",
		POWER_PLAIN(EPOWERKIND_MS0),		POWER_FIELD(u32ResetHoldMs),	0 },

	{ "psonpolarity",	"low | high",					"PS_ON# asserted level (low per ATX spec)",
		POWER_ENUM(sg_pePolarityNames),		POWER_FIELD(ePsOnPolarity),		1 },
	{ "psondrive",		"pushpull | opencollector",		"PS_ON# drive",
		POWER_ENUM(sg_peDriveNames),		POWER_FIELD(ePsOnDrive),		1 },
	{ "pwroktimeout",	"<ms, 100 ms steps>",			"PS_ON# released if no PWR_OK by then",
		POWER_PLAIN(EPOWERKIND_MS100),		POWER_FIELD(u32PwrOkTimeoutMs),	1 },

	{ "resetaction",	"pulse | level",				"pulse = button press, level = reset line held while not on (target must be off; needs sense or poweraction level)",
		POWER_ENUM(sg_peActionNames),		POWER_FIELD(eResetAction),		2 },
	{ "resetpull",		"none | up | down",				"pull on the reset output",
		POWER_ENUM(sg_pePullNames),			POWER_FIELD(eResetPull),		2 },
	{ "powerpolarity",	"low | high",					"power output asserted level",
		POWER_ENUM(sg_pePolarityNames),		POWER_FIELD(ePowerPolarity),	2 },
	{ "powerdrive",		"pushpull | opencollector",		"power output drive",
		POWER_ENUM(sg_peDriveNames),		POWER_FIELD(ePowerDrive),		2 },
	{ "powerpull",		"none | up | down",				"pull on the power output",
		POWER_ENUM(sg_pePullNames),			POWER_FIELD(ePowerPull),		2 },
	{ "poweraction",	"pulse | level",				"pulse = button press, level = held while on (target must be off)",
		POWER_ENUM(sg_peActionNames),		POWER_FIELD(ePowerAction),		2 },
	{ "powerpulse",		"<ms>",							"power button press length",
		POWER_PLAIN(EPOWERKIND_MS),			POWER_FIELD(u32PowerPulseMs),	2 },
	{ "forceoffhold",	"<ms, 100 ms steps>",			"press-and-hold length for 'power forceoff'",
		POWER_PLAIN(EPOWERKIND_MS100),		POWER_FIELD(u32ForceOffHoldMs),	2 },
	{ "sense",			"off | on",						"power sense input in use (target must be off)",
		POWER_PLAIN(EPOWERKIND_BOOL),		POWER_FIELD(bSense),			2 },
	{ "sensepolarity",	"high | low",					"sense level that means power is on",
		POWER_ENUM(sg_pePolarityNames),		POWER_FIELD(eSensePolarity),	2 },
	{ "sensetimeout",	"<ms, 100 ms steps>",			"power-up undone if not sensed by then",
		POWER_PLAIN(EPOWERKIND_MS100),		POWER_FIELD(u32SenseTimeoutMs),	2 },

	{ NULL,				NULL,							NULL,
		POWER_PLAIN(EPOWERKIND_BOOL),		0, 0,							SETTING_END },
};

static const SPowerSettingInfo *PowerSettingFind(const char *peName)
{
	uint32_t u32Loop;

	for (u32Loop = 0; sg_sPowerSettings[u32Loop].s16Group != SETTING_END; u32Loop++)
	{
		if (0 == strcasecmp(peName, sg_sPowerSettings[u32Loop].peName))
		{
			return(&sg_sPowerSettings[u32Loop]);
		}
	}

	return(NULL);
}

// Read / write a settings field of any size as uint32_t
static uint32_t PowerFieldGet(const SPowerSettingInfo *psInfo,
                              const SPowerSettings *psPower)
{
	const uint8_t *pu8Field = ((const uint8_t *) psPower) + psInfo->eOffset;

	if (sizeof(uint8_t) == psInfo->eSize)
	{
		return(*pu8Field);
	}

	if (sizeof(uint16_t) == psInfo->eSize)
	{
		return(*((const uint16_t *) pu8Field));
	}

	return(*((const uint32_t *) pu8Field));
}

static void PowerFieldSet(const SPowerSettingInfo *psInfo,
                          SPowerSettings *psPower,
                          uint32_t u32Value)
{
	uint8_t *pu8Field = ((uint8_t *) psPower) + psInfo->eOffset;

	if (sizeof(uint8_t) == psInfo->eSize)
	{
		*pu8Field = (uint8_t) u32Value;
	}
	else
	if (sizeof(uint16_t) == psInfo->eSize)
	{
		*((uint16_t *) pu8Field) = (uint16_t) u32Value;
	}
	else
	{
		*((uint32_t *) pu8Field) = u32Value;
	}
}

static bool PowerSettingParse(SConsoleSession *psSession,
                              const SPowerSettingInfo *psInfo,
                              const char *peValue,
                              SPowerSettings *psPower)
{
	uint32_t u32Value;
	int32_t s32Index;
	bool bValue;

	if (EPOWERKIND_ENUM == psInfo->eKind)
	{
		s32Index = NameLookup(peValue, psInfo->ppeNames, psInfo->u32NameCount);
		if (s32Index < 0)
		{
			ConsolePrintf(psSession, "%s must be %s\n", psInfo->peName, psInfo->peArgs);
			return(false);
		}

		PowerFieldSet(psInfo, psPower, (uint32_t) s32Index);
		return(true);
	}

	if (EPOWERKIND_BOOL == psInfo->eKind)
	{
		if (false == ParseBool(peValue, &bValue))
		{
			ConsolePrintf(psSession, "%s must be on or off\n", psInfo->peName);
			return(false);
		}

		PowerFieldSet(psInfo, psPower, (bValue) ? 1 : 0);
		return(true);
	}

	if ((false == ParseU32(peValue, &u32Value)) ||
		((0 == u32Value) && (psInfo->eKind != EPOWERKIND_MS0)) ||
		((EPOWERKIND_MS100 == psInfo->eKind) && (u32Value % 100)))
	{
		ConsolePrintf(psSession, "%s must be %s%s\n",
					  psInfo->peName,
					  psInfo->peArgs,
					  (EPOWERKIND_MS0 == psInfo->eKind) ? "" : ", not 0");
		return(false);
	}

	PowerFieldSet(psInfo, psPower, u32Value);
	return(true);
}

static void PowerSettingFormat(const SPowerSettingInfo *psInfo,
                               const SPowerSettings *psPower,
                               char *peBuffer,
                               uint32_t u32Size)
{
	uint32_t u32Value = PowerFieldGet(psInfo, psPower);

	if (EPOWERKIND_ENUM == psInfo->eKind)
	{
		snprintf(peBuffer, u32Size, "%s", (u32Value < psInfo->u32NameCount) ? psInfo->ppeNames[u32Value] : "?");
		return;
	}

	if (EPOWERKIND_BOOL == psInfo->eKind)
	{
		snprintf(peBuffer, u32Size, "%s", (u32Value) ? "on" : "off");
		return;
	}

	snprintf(peBuffer, u32Size, "%lu ms", (unsigned long) u32Value);
}

static void PowerSettingDefault(const SPowerSettingInfo *psInfo,
                                SPowerSettings *psPower)
{
	SPowerSettings sDefault;

	SettingsPowerDefaults(&sDefault);
	PowerFieldSet(psInfo, psPower, PowerFieldGet(psInfo, &sDefault));
}

static void PowerSettingsUsage(SConsoleSession *psSession)
{
	int16_t s16Group;
	uint32_t u32Loop;

	ConsolePrintf(psSession, "Usage: power set <name> <value> [<name> <value> ...]\n");
	for (s16Group = 0; s16Group < 3; s16Group++)
	{
		ConsolePrintf(psSession, " %s\n", sg_peGroupNames[s16Group]);
		for (u32Loop = 0; sg_sPowerSettings[u32Loop].s16Group != SETTING_END; u32Loop++)
		{
			const SPowerSettingInfo *psInfo = &sg_sPowerSettings[u32Loop];

			if (s16Group == psInfo->s16Group)
			{
				ConsolePrintf(psSession, "  %-14s %-26s %s\n", psInfo->peName, psInfo->peArgs, psInfo->peHelp);
			}
		}
	}
}

// Store and apply new power settings
static void PowerCommit(SConsoleSession *psSession,
                        const SPowerSettings *psNew)
{
	const SPowerSettings *psOld = &g_sSettings.sPower;
	bool bForget;

	// A level reset line needs to know when the target is really on
	if ((EPOWERMODE_PANEL == psNew->eMode) &&
		(EPANELACTION_LEVEL == psNew->eResetAction) &&
		(EPANELACTION_PULSE == psNew->ePowerAction) &&
		(false == psNew->bSense))
	{
		ConsolePrintf(psSession, "resetaction level needs sense on (or poweraction level) - nothing changed\n");
		return;
	}

	// These change what "on" means - only with the target off
	if (((psNew->eMode != psOld->eMode) ||
		 (psNew->ePowerAction != psOld->ePowerAction) ||
		 (psNew->eResetAction != psOld->eResetAction) ||
		 (psNew->bSense != psOld->bSense)) &&
		(false == PowerIsIdleOff()))
	{
		ConsolePrintf(psSession, "mode, poweraction, resetaction and sense can only change with the target off - nothing changed\n");
		return;
	}

	bForget = ((psNew->eMode != psOld->eMode) ||
			   (psNew->ePowerAction != psOld->ePowerAction) ||
			   (psNew->eResetAction != psOld->eResetAction) ||
			   (psNew->bSense != psOld->bSense)) ? true : false;

	g_sSettings.sPower = *psNew;
	(void) SettingsSave();
	PowerApplySettings(bForget);
}

static void CmdPowerOn(SConsoleSession *psSession,
                       uint32_t u32Argc,
                       char **ppeArgv)
{
	(void) PowerOn(psSession);
}

static void CmdPowerOff(SConsoleSession *psSession,
                        uint32_t u32Argc,
                        char **ppeArgv)
{
	(void) PowerOff(psSession);
}

static void CmdPowerForceOff(SConsoleSession *psSession,
                             uint32_t u32Argc,
                             char **ppeArgv)
{
	(void) PowerForceOff(psSession);
}

static void CmdPowerReset(SConsoleSession *psSession,
                          uint32_t u32Argc,
                          char **ppeArgv)
{
	if (1 == u32Argc)
	{
		(void) PowerReset(psSession);
		return;
	}

	if ((2 == u32Argc) && (0 == strcasecmp(ppeArgv[1], "hold")))
	{
		(void) PowerResetHold(psSession, true);
		return;
	}

	if ((2 == u32Argc) && (0 == strcasecmp(ppeArgv[1], "release")))
	{
		(void) PowerResetHold(psSession, false);
		return;
	}

	ConsolePrintf(psSession, "Usage: power reset [hold | release]\n");
}

static void CmdPowerStats(SConsoleSession *psSession,
                          uint32_t u32Argc,
                          char **ppeArgv)
{
	PowerStatsPrint(psSession);
}

static void CmdPowerGet(SConsoleSession *psSession,
                        uint32_t u32Argc,
                        char **ppeArgv)
{
	const SPowerSettingInfo *psInfo;
	char eValue[24];
	int16_t s16Group;
	uint32_t u32Loop;

	if (u32Argc > 2)
	{
		ConsolePrintf(psSession, "Usage: power get [<name>]\n");
		return;
	}

	if (2 == u32Argc)
	{
		psInfo = PowerSettingFind(ppeArgv[1]);
		if (NULL == psInfo)
		{
			ConsolePrintf(psSession, "Unknown setting '%s'\n", ppeArgv[1]);
			return;
		}

		PowerSettingFormat(psInfo, &g_sSettings.sPower, eValue, sizeof(eValue));
		ConsolePrintf(psSession, "%s\n", eValue);
		return;
	}

	for (s16Group = 0; s16Group < 3; s16Group++)
	{
		ConsolePrintf(psSession, " %s%s\n",
					  sg_peGroupNames[s16Group],
					  (s16Group == ((EPOWERMODE_ATX == g_sSettings.sPower.eMode) ? 1 : 2)) ? "  (active)" : "");
		for (u32Loop = 0; sg_sPowerSettings[u32Loop].s16Group != SETTING_END; u32Loop++)
		{
			psInfo = &sg_sPowerSettings[u32Loop];
			if (s16Group == psInfo->s16Group)
			{
				PowerSettingFormat(psInfo, &g_sSettings.sPower, eValue, sizeof(eValue));
				ConsolePrintf(psSession, "  %-14s %s\n", psInfo->peName, eValue);
			}
		}
	}
}

static void CmdPowerSet(SConsoleSession *psSession,
                        uint32_t u32Argc,
                        char **ppeArgv)
{
	SPowerSettings sNew = g_sSettings.sPower;
	const SPowerSettingInfo *psInfo;
	uint32_t u32Loop;

	if ((u32Argc < 3) || (0 == (u32Argc & 1)))
	{
		PowerSettingsUsage(psSession);
		return;
	}

	for (u32Loop = 1; u32Loop < u32Argc; u32Loop += 2)
	{
		psInfo = PowerSettingFind(ppeArgv[u32Loop]);
		if (NULL == psInfo)
		{
			ConsolePrintf(psSession, "Unknown setting '%s' - nothing changed\n", ppeArgv[u32Loop]);
			return;
		}

		if (false == PowerSettingParse(psSession, psInfo, ppeArgv[u32Loop + 1], &sNew))
		{
			ConsolePrintf(psSession, "Nothing changed\n");
			return;
		}
	}

	PowerCommit(psSession, &sNew);
}

static void CmdPowerDefault(SConsoleSession *psSession,
                            uint32_t u32Argc,
                            char **ppeArgv)
{
	SPowerSettings sNew = g_sSettings.sPower;
	const SPowerSettingInfo *psInfo;
	uint32_t u32Loop;
	bool bLastOn = sNew.bLastOn;

	if (1 == u32Argc)
	{
		SettingsPowerDefaults(&sNew);
		sNew.bLastOn = bLastOn;
		PowerCommit(psSession, &sNew);
		return;
	}

	for (u32Loop = 1; u32Loop < u32Argc; u32Loop++)
	{
		psInfo = PowerSettingFind(ppeArgv[u32Loop]);
		if (NULL == psInfo)
		{
			ConsolePrintf(psSession, "Unknown setting '%s' - nothing changed\n", ppeArgv[u32Loop]);
			return;
		}

		PowerSettingDefault(psInfo, &sNew);
	}

	PowerCommit(psSession, &sNew);
}

static const SCommand sg_sPowerCommands[] =
{
	{ "default",	"default [<name> ...]  (settings back to defaults)",		CmdPowerDefault },
	{ "forceoff",	"Press-and-hold override (panel) / off (ATX)",			CmdPowerForceOff },
	{ "get",		"get [<name>]",												CmdPowerGet },
	{ "off",		"Power the target off",										CmdPowerOff },
	{ "on",			"Power the target on",										CmdPowerOn },
	{ "reset",		"reset [hold | release]  (pulse, or hold until released)",	CmdPowerReset },
	{ "set",		"set <name> <value> [<name> <value> ...] | set ?",			CmdPowerSet },
	{ "stats",		"State, pins, buttons and counters",						CmdPowerStats },
	{ NULL,			NULL,														NULL },
};

static void CmdPower(SConsoleSession *psSession,
                     uint32_t u32Argc,
                     char **ppeArgv)
{
	const SCommand *psCommand = NULL;

	if (u32Argc >= 2)
	{
		psCommand = CommandFind(sg_sPowerCommands, ppeArgv[1]);
	}

	if (NULL == psCommand)
	{
		UsageCommands(psSession, "power <subcommand>", sg_sPowerCommands);
		return;
	}

	psCommand->pHandler(psSession, u32Argc - 1, &ppeArgv[1]);
}

// --------------------------------------------------------------------------
// system
// --------------------------------------------------------------------------

static void CmdSystemBootsel(SConsoleSession *psSession,
                             uint32_t u32Argc,
                             char **ppeArgv)
{
	ConsolePrintf(psSession, "Entering USB bootloader\n");
	MainRequestReboot(true);
}

static void CmdSystemReboot(SConsoleSession *psSession,
                            uint32_t u32Argc,
                            char **ppeArgv)
{
	ConsolePrintf(psSession, "Rebooting\n");
	MainRequestReboot(false);
}

static void CmdSystemReset(SConsoleSession *psSession,
                           uint32_t u32Argc,
                           char **ppeArgv)
{
	SWifiSettings sOldWifi = g_sSettings.sWifi;
	SPortSettings sOldPort[UART_PORT_COUNT];
	uint32_t u32Loop;

	// Defaults include the power mode - don't switch it under a running target
	if (false == PowerIsIdleOff())
	{
		ConsolePrintf(psSession, "Target power is on or sequencing - 'power off' first\n");
		return;
	}

	memcpy(sOldPort, g_sSettings.sPort, sizeof(sOldPort));
	SettingsDefaults(&g_sSettings);
	(void) SettingsSave();
	PowerApplySettings(true);

	for (u32Loop = 0; u32Loop < UART_PORT_COUNT; u32Loop++)
	{
		UartConfigure((uint8_t) u32Loop, &g_sSettings.sPort[u32Loop].sLine);
		if (sOldPort[u32Loop].u16TcpPort != g_sSettings.sPort[u32Loop].u16TcpPort)
		{
			BridgeApplyPort((uint8_t) u32Loop);
		}
	}

	ConsolePrintf(psSession, "All settings restored to defaults\n");
	WifiApply(&sOldWifi, psSession);
}

static void CmdSystemStats(SConsoleSession *psSession,
                           uint32_t u32Argc,
                           char **ppeArgv)
{
	char eText[24];

	FormatDuration(time_us_64(), eText, sizeof(eText));
	ConsolePrintf(psSession, "Uptime       %s\n", eText);
	ConsolePrintf(psSession, "Firmware     built " __DATE__ " " __TIME__ ", SDK " PICO_SDK_VERSION_STRING "\n");
	ConsolePrintf(psSession, "clk_sys      %lu MHz\n", (unsigned long) (clock_get_hz(clk_sys) / 1000000));
	ConsolePrintf(psSession, "clk_peri     %lu MHz\n", (unsigned long) (clock_get_hz(clk_peri) / 1000000));
	ConsolePrintf(psSession, "Ring buffers %lu KB each (rx + tx per port)\n",
				  (unsigned long) (g_sUartPorts[0].sRx.u32Size / 1024));
	ConsolePrintf(psSession, "Heap free    %lu KB unclaimed\n", (unsigned long) (UartHeapRemaining() / 1024));
	ConsolePrintf(psSession, "Telnet       %lu session(s)\n", (unsigned long) TelnetSessionCount());
	ConsolePrintf(psSession, "USB console  %s\n", UsbConsoleConnected() ? "open" : "closed");
	ConsolePrintf(psSession, "Bridges      %lu client(s)\n", (unsigned long) BridgeClientCount());
}

static const SCommand sg_sSystemCommands[] =
{
	{ "bootsel",	"Reboot into the USB bootloader",			CmdSystemBootsel },
	{ "reboot",		"Restart",									CmdSystemReboot },
	{ "reset",		"Restore every setting to its default",		CmdSystemReset },
	{ "stats",		"Uptime, clocks, memory, sessions",			CmdSystemStats },
	{ NULL,			NULL,										NULL },
};

static void CmdSystem(SConsoleSession *psSession,
                      uint32_t u32Argc,
                      char **ppeArgv)
{
	const SCommand *psCommand = NULL;

	if (u32Argc >= 2)
	{
		psCommand = CommandFind(sg_sSystemCommands, ppeArgv[1]);
	}

	if (NULL == psCommand)
	{
		UsageCommands(psSession, "system <subcommand>", sg_sSystemCommands);
		return;
	}

	psCommand->pHandler(psSession, u32Argc - 1, &ppeArgv[1]);
}

// --------------------------------------------------------------------------
// Top level
// --------------------------------------------------------------------------

static void CmdHelp(SConsoleSession *psSession,
                    uint32_t u32Argc,
                    char **ppeArgv);

static void CmdExit(SConsoleSession *psSession,
                    uint32_t u32Argc,
                    char **ppeArgv)
{
	if (psSession->eType != ECONSOLETYPE_TELNET)
	{
		ConsolePrintf(psSession, "Only telnet sessions can exit\n");
		return;
	}

	ConsolePrintf(psSession, "Bye\n");
	psSession->bClosing = true;
}

static const SCommand sg_sAreas[] =
{
	{ "port",		"Serial port settings and statistics",		CmdPort },
	{ "power",		"Target power, reset and sensing",			CmdPower },
	{ "system",		"Status, reboot, factory reset",			CmdSystem },
	{ "wifi",		"WiFi settings and statistics",				CmdWifi },
	{ "help",		"This list",								CmdHelp },
	{ "exit",		"Close this telnet session",				CmdExit },
	{ NULL,			NULL,										NULL },
};

static void CmdHelp(SConsoleSession *psSession,
                    uint32_t u32Argc,
                    char **ppeArgv)
{
	UsageCommands(psSession, "<area> <subcommand> ...   (an area alone shows its usage)", sg_sAreas);
}

void CommandExecute(SConsoleSession *psSession,
                    uint32_t u32Argc,
                    char **ppeArgv)
{
	const SCommand *psCommand;

	if (0 == strcmp(ppeArgv[0], "?"))
	{
		CmdHelp(psSession, u32Argc, ppeArgv);
		return;
	}

	psCommand = CommandFind(sg_sAreas, ppeArgv[0]);
	if (NULL == psCommand)
	{
		ConsolePrintf(psSession, "Unknown command '%s' - type 'help'\n", ppeArgv[0]);
		return;
	}

	psCommand->pHandler(psSession, u32Argc, ppeArgv);
}
