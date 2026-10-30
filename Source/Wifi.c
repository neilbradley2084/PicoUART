// WiFi station state machine.  Polled from core 0's main loop.

#include <stdio.h>
#include "Wifi.h"
#include "Config.h"
#include "Uart.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"

typedef enum EWifiState
{
	EWIFISTATE_DISABLED = 0,	// Turned off by the user
	EWIFISTATE_NO_SSID,			// Nothing to join yet
	EWIFISTATE_START,			// Begin a join attempt
	EWIFISTATE_JOINING,			// Association and address in progress
	EWIFISTATE_CONNECTED,
	EWIFISTATE_RETRY_WAIT,		// Pausing before the next attempt
} EWifiState;

typedef struct SScanResult
{
	struct SScanResult *psNext;
	uint8_t u8Bssid[6];
	char eSsid[SETTINGS_SSID_MAX + 1];
	uint16_t u16Channel;
	int16_t s16Rssi;
	uint8_t u8Auth;
} SScanResult;

static const char *sg_peStateNames[] =
{
	"disabled",
	"no ssid configured",
	"starting",
	"joining",
	"connected",
	"waiting to retry",
};

static EWifiState sg_eState = EWIFISTATE_DISABLED;
static uint64_t sg_u64DeadlineUs;
static uint64_t sg_u64ConnectedUs;
static uint32_t sg_u32Attempts;
static uint32_t sg_u32Connects;
static uint32_t sg_u32Disconnects;
static int32_t sg_s32LastFailure;			// CYW43_LINK_xxx of the last failed attempt, 0 = none
static bool sg_bDhcpRunning = true;			// cyw43 starts DHCP when the interface comes up
static bool sg_bLed;
static uint32_t sg_u32LedActivity;			// Last UartActivityCount() seen
static uint64_t sg_u64LedActivityUs;		// When it last changed
static SScanResult *sg_psScanResults;
static SConsoleSession *sg_psScanSession;
static bool sg_bScanning;

static struct netif *WifiNetif(void)
{
	return(&cyw43_state.netif[CYW43_ITF_STA]);
}

static const char *WifiLinkText(int32_t s32Link)
{
	if (CYW43_LINK_BADAUTH == s32Link)
	{
		return("bad password");
	}

	if (CYW43_LINK_NONET == s32Link)
	{
		return("network not found");
	}

	if (CYW43_LINK_FAIL == s32Link)
	{
		return("join failed");
	}

	if (CYW43_LINK_NOIP == s32Link)
	{
		return("no IP address");
	}

	if (0 == s32Link)
	{
		return("none");
	}

	return("timeout");
}

static void WifiStatsReset(void)
{
	sg_u32Attempts = 0;
	sg_u32Connects = 0;
	sg_u32Disconnects = 0;
	sg_s32LastFailure = 0;
}

// DHCP or static addressing, applied to the live interface right now
static void WifiApplyAddress(void)
{
	const SWifiSettings *psWifi = &g_sSettings.sWifi;
	struct netif *psNetif = WifiNetif();
	ip4_addr_t sIp;
	ip4_addr_t sMask;
	ip4_addr_t sGateway;

	if (psWifi->bDhcp)
	{
		if (false == sg_bDhcpRunning)
		{
			ip4_addr_set_zero(&sIp);
			netif_set_addr(psNetif, &sIp, &sIp, &sIp);
			(void) dhcp_start(psNetif);
			sg_bDhcpRunning = true;
		}

		return;
	}

	if (sg_bDhcpRunning)
	{
		dhcp_release_and_stop(psNetif);
		sg_bDhcpRunning = false;
	}

	ip4_addr_set_u32(&sIp, lwip_htonl(psWifi->u32Ip));
	ip4_addr_set_u32(&sMask, lwip_htonl(psWifi->u32Netmask));
	ip4_addr_set_u32(&sGateway, lwip_htonl(psWifi->u32Gateway));
	netif_set_addr(psNetif, &sIp, &sMask, &sGateway);
}

static void WifiRetryLater(void)
{
	(void) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
	sg_u64DeadlineUs = time_us_64() + ((uint64_t) CONFIG_WIFI_RETRY_DELAY_MS * 1000);
	sg_eState = EWIFISTATE_RETRY_WAIT;
}

static void WifiStartJoin(void)
{
	const SWifiSettings *psWifi = &g_sSettings.sWifi;
	uint32_t u32PasswordLength = strlen(psWifi->ePassword);
	uint32_t u32Auth = CYW43_AUTH_WPA2_MIXED_PSK;
	uint32_t u32Channel = CYW43_CHANNEL_NONE;
	int s32Result;

	if (0 == u32PasswordLength)
	{
		u32Auth = CYW43_AUTH_OPEN;
	}

	if (psWifi->u32Channel)
	{
		u32Channel = psWifi->u32Channel;
	}

	sg_u32Attempts++;
	s32Result = cyw43_wifi_join(&cyw43_state,
								strlen(psWifi->eSsid),
								(const uint8_t *) psWifi->eSsid,
								u32PasswordLength,
								(const uint8_t *) psWifi->ePassword,
								u32Auth,
								NULL,
								u32Channel);

	if (0 != s32Result)
	{
		sg_s32LastFailure = CYW43_LINK_FAIL;
		WifiRetryLater();
		return;
	}

	sg_u64DeadlineUs = time_us_64() + ((uint64_t) CONFIG_WIFI_JOIN_TIMEOUT_MS * 1000);
	sg_eState = EWIFISTATE_JOINING;
}

// LED patterns:
//   WiFi disabled                 off
//   Not connected                 1 Hz, 50/50
//   Connected, no data            1 Hz, on 900 ms / off 100 ms
//   Connected, data flowing       4 Hz, 50/50
static void WifiLedUpdate(void)
{
	bool bLed = false;
	uint64_t u64Now = time_us_64();
	uint32_t u32Activity = UartActivityCount();

	if (u32Activity != sg_u32LedActivity)
	{
		sg_u32LedActivity = u32Activity;
		sg_u64LedActivityUs = u64Now;
	}

	if (EWIFISTATE_CONNECTED == sg_eState)
	{
		if ((u64Now - sg_u64LedActivityUs) < ((uint64_t) CONFIG_LED_ACTIVITY_HOLD_MS * 1000))
		{
			bLed = ((u64Now % 250000) < 125000) ? true : false;
		}
		else
		{
			bLed = ((u64Now % 1000000) < 900000) ? true : false;
		}
	}
	else
	if (EWIFISTATE_DISABLED != sg_eState)
	{
		bLed = ((u64Now % 1000000) < 500000) ? true : false;
	}

	if (bLed != sg_bLed)
	{
		sg_bLed = bLed;
		cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, bLed);
	}
}

static int WifiScanResult(void *pvEnv,
                          const cyw43_ev_scan_result_t *psResult)
{
	SScanResult *psScan;
	uint32_t u32Length;

	// The radio reports each AP several times - keep the strongest
	for (psScan = sg_psScanResults; psScan; psScan = psScan->psNext)
	{
		if (0 == memcmp(psScan->u8Bssid, psResult->bssid, sizeof(psScan->u8Bssid)))
		{
			if (psResult->rssi > psScan->s16Rssi)
			{
				psScan->s16Rssi = psResult->rssi;
			}

			return(0);
		}
	}

	MEMALLOC(psScan, sizeof(*psScan));
	if (NULL == psScan)
	{
		return(0);
	}

	u32Length = psResult->ssid_len;
	if (u32Length > SETTINGS_SSID_MAX)
	{
		u32Length = SETTINGS_SSID_MAX;
	}

	memcpy(psScan->eSsid, psResult->ssid, u32Length);
	memcpy(psScan->u8Bssid, psResult->bssid, sizeof(psScan->u8Bssid));
	psScan->u16Channel = psResult->channel;
	psScan->s16Rssi = psResult->rssi;
	psScan->u8Auth = psResult->auth_mode;
	psScan->psNext = sg_psScanResults;
	sg_psScanResults = psScan;
	return(0);
}

static const char *WifiAuthText(uint8_t u8Auth)
{
	if (u8Auth & 4)
	{
		return("WPA2");
	}

	if (u8Auth & 2)
	{
		return("WPA");
	}

	if (u8Auth & 1)
	{
		return("WEP");
	}

	return("open");
}

// Scan finished: print strongest first, free the list
static void WifiScanComplete(void)
{
	SConsoleSession *psSession = sg_psScanSession;
	SScanResult *psSorted = NULL;
	SScanResult *psScan;
	uint32_t u32Count = 0;

	// Insertion sort by RSSI, descending
	while (sg_psScanResults)
	{
		SScanResult **ppsLink = &psSorted;

		psScan = sg_psScanResults;
		sg_psScanResults = psScan->psNext;

		while ((*ppsLink) && ((*ppsLink)->s16Rssi >= psScan->s16Rssi))
		{
			ppsLink = &(*ppsLink)->psNext;
		}

		psScan->psNext = *ppsLink;
		*ppsLink = psScan;
	}

	if (psSession)
	{
		ConsolePrintf(psSession, "\r\x1b[K  %-32s  Ch  RSSI  Auth  BSSID\n", "SSID");
	}

	while (psSorted)
	{
		psScan = psSorted;
		psSorted = psScan->psNext;

		if (psSession)
		{
			ConsolePrintf(psSession, "  %-32s  %2u  %4d  %-4s  %02x:%02x:%02x:%02x:%02x:%02x\n",
						  ('\0' == psScan->eSsid[0]) ? "(hidden)" : psScan->eSsid,
						  psScan->u16Channel,
						  psScan->s16Rssi,
						  WifiAuthText(psScan->u8Auth),
						  psScan->u8Bssid[0], psScan->u8Bssid[1], psScan->u8Bssid[2],
						  psScan->u8Bssid[3], psScan->u8Bssid[4], psScan->u8Bssid[5]);
		}

		u32Count++;
		MEMFREE(psScan);
	}

	if (psSession)
	{
		ConsolePrintf(psSession, "%lu network(s)\n", (unsigned long) u32Count);
		ConsolePrompt(psSession);
		ConsoleWrite(psSession, psSession->eLine, psSession->u32LineLength);
	}

	sg_psScanSession = NULL;
	sg_bScanning = false;
}

EStatus WifiScanStart(SConsoleSession *psSession)
{
	cyw43_wifi_scan_options_t sOptions;

	if (sg_bScanning)
	{
		return(ESTATUS_BUSY);
	}

	memset(&sOptions, 0, sizeof(sOptions));
	if (0 != cyw43_wifi_scan(&cyw43_state, &sOptions, NULL, WifiScanResult))
	{
		return(ESTATUS_BUSY);
	}

	sg_psScanSession = psSession;
	sg_bScanning = true;
	return(ESTATUS_OK);
}

// A console session closed - don't print scan results into freed memory
void WifiSessionGone(SConsoleSession *psSession)
{
	if (sg_psScanSession == psSession)
	{
		sg_psScanSession = NULL;
	}
}

bool WifiConnected(void)
{
	return((EWIFISTATE_CONNECTED == sg_eState) ? true : false);
}

void WifiPoll(void)
{
	int32_t s32Link;
	char eIp[16];

	if (sg_bScanning && (false == cyw43_wifi_scan_active(&cyw43_state)))
	{
		WifiScanComplete();
	}

	WifiLedUpdate();

	switch (sg_eState)
	{
		case EWIFISTATE_DISABLED:
		{
			break;
		}

		case EWIFISTATE_NO_SSID:
		{
			if ('\0' != g_sSettings.sWifi.eSsid[0])
			{
				sg_eState = EWIFISTATE_START;
			}

			break;
		}

		case EWIFISTATE_START:
		{
			if ('\0' == g_sSettings.sWifi.eSsid[0])
			{
				sg_eState = EWIFISTATE_NO_SSID;
				break;
			}

			WifiStartJoin();
			break;
		}

		case EWIFISTATE_JOINING:
		{
			s32Link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

			if (CYW43_LINK_UP == s32Link)
			{
				sg_eState = EWIFISTATE_CONNECTED;
				sg_u32Connects++;
				sg_s32LastFailure = 0;
				sg_u64ConnectedUs = time_us_64();
				FormatIp(lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(WifiNetif()))), eIp, sizeof(eIp));
				LogMsg("wifi: connected to \"%s\", %s", g_sSettings.sWifi.eSsid, eIp);
				break;
			}

			if ((CYW43_LINK_FAIL == s32Link) ||
				(CYW43_LINK_NONET == s32Link) ||
				(CYW43_LINK_BADAUTH == s32Link))
			{
				sg_s32LastFailure = s32Link;
				WifiRetryLater();
				break;
			}

			if (time_us_64() >= sg_u64DeadlineUs)
			{
				sg_s32LastFailure = (CYW43_LINK_NOIP == s32Link) ? CYW43_LINK_NOIP : -100;
				WifiRetryLater();
			}

			break;
		}

		case EWIFISTATE_CONNECTED:
		{
			s32Link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
			if (CYW43_LINK_UP == s32Link)
			{
				break;
			}

			sg_u32Disconnects++;

			if (CYW43_LINK_JOIN == cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA))
			{
				// Still associated but lost the address - give DHCP a join timeout to recover
				LogMsg("wifi: lost IP address");
				sg_u64DeadlineUs = time_us_64() + ((uint64_t) CONFIG_WIFI_JOIN_TIMEOUT_MS * 1000);
				sg_eState = EWIFISTATE_JOINING;
				break;
			}

			LogMsg("wifi: disconnected from \"%s\"", g_sSettings.sWifi.eSsid);
			WifiRetryLater();
			break;
		}

		case EWIFISTATE_RETRY_WAIT:
		{
			if (time_us_64() >= sg_u64DeadlineUs)
			{
				sg_eState = EWIFISTATE_START;
			}

			break;
		}
	}
}

// Drop and rejoin with the current settings
void WifiReconnect(bool bResetStats)
{
	if (bResetStats)
	{
		WifiStatsReset();
	}

	if (EWIFISTATE_DISABLED == sg_eState)
	{
		return;
	}

	if (EWIFISTATE_CONNECTED == sg_eState)
	{
		LogMsg("wifi: leaving \"%s\"", g_sSettings.sWifi.eSsid);
	}

	(void) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
	sg_eState = EWIFISTATE_START;
}

void WifiEnable(bool bEnable)
{
	if (bEnable)
	{
		WifiStatsReset();
		(void) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
		sg_eState = EWIFISTATE_START;
		return;
	}

	if (EWIFISTATE_CONNECTED == sg_eState)
	{
		LogMsg("wifi: disabled, leaving \"%s\"", g_sSettings.sWifi.eSsid);
	}

	(void) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
	sg_eState = EWIFISTATE_DISABLED;
}

// g_sSettings.sWifi was just changed from psOld - make it take effect now
void WifiApply(const SWifiSettings *psOld,
               SConsoleSession *psSession)
{
	const SWifiSettings *psNew = &g_sSettings.sWifi;
	bool bRejoin = false;

	if ((0 != strcmp(psOld->eSsid, psNew->eSsid)) ||
		(0 != strcmp(psOld->ePassword, psNew->ePassword)) ||
		(psOld->u32Channel != psNew->u32Channel))
	{
		bRejoin = true;
	}

	if ((psOld->bDhcp != psNew->bDhcp) ||
		(psOld->u32Ip != psNew->u32Ip) ||
		(psOld->u32Netmask != psNew->u32Netmask) ||
		(psOld->u32Gateway != psNew->u32Gateway))
	{
		if ((psSession) && (ECONSOLETYPE_TELNET == psSession->eType))
		{
			ConsolePrintf(psSession, "Address change applied - this telnet session may drop\n");
		}

		WifiApplyAddress();
	}

	if (psOld->bEnabled != psNew->bEnabled)
	{
		WifiEnable(psNew->bEnabled);
		return;
	}

	if (bRejoin)
	{
		if ((psSession) && (ECONSOLETYPE_TELNET == psSession->eType) && (EWIFISTATE_CONNECTED == sg_eState))
		{
			ConsolePrintf(psSession, "Rejoining - this telnet session will drop\n");
		}

		WifiReconnect(true);
	}
}

void WifiStatsPrint(SConsoleSession *psSession)
{
	const SWifiSettings *psWifi = &g_sSettings.sWifi;
	struct netif *psNetif = WifiNetif();
	uint8_t u8Mac[6];
	char eText[24];

	ConsolePrintf(psSession, "State        %s\n", sg_peStateNames[sg_eState]);
	ConsolePrintf(psSession, "SSID         \"%s\"\n", psWifi->eSsid);

	if (0 == cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, u8Mac))
	{
		ConsolePrintf(psSession, "MAC          %02x:%02x:%02x:%02x:%02x:%02x\n",
					  u8Mac[0], u8Mac[1], u8Mac[2], u8Mac[3], u8Mac[4], u8Mac[5]);
	}

	if (EWIFISTATE_CONNECTED == sg_eState)
	{
		int32_t s32Rssi;
		uint8_t u8Bssid[6];

		if (0 == cyw43_wifi_get_bssid(&cyw43_state, u8Bssid))
		{
			ConsolePrintf(psSession, "BSSID        %02x:%02x:%02x:%02x:%02x:%02x\n",
						  u8Bssid[0], u8Bssid[1], u8Bssid[2], u8Bssid[3], u8Bssid[4], u8Bssid[5]);
		}

		if (0 == cyw43_wifi_get_rssi(&cyw43_state, &s32Rssi))
		{
			ConsolePrintf(psSession, "RSSI         %ld dBm\n", (long) s32Rssi);
		}

		FormatIp(lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(psNetif))), eText, sizeof(eText));
		ConsolePrintf(psSession, "Address      %s (%s)\n", eText, sg_bDhcpRunning ? "DHCP" : "static");
		FormatIp(lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(psNetif))), eText, sizeof(eText));
		ConsolePrintf(psSession, "Netmask      %s\n", eText);
		FormatIp(lwip_ntohl(ip4_addr_get_u32(netif_ip4_gw(psNetif))), eText, sizeof(eText));
		ConsolePrintf(psSession, "Gateway      %s\n", eText);
		FormatDuration(time_us_64() - sg_u64ConnectedUs, eText, sizeof(eText));
		ConsolePrintf(psSession, "Connected    %s\n", eText);
	}

	ConsolePrintf(psSession, "Attempts     %lu\n", (unsigned long) sg_u32Attempts);
	ConsolePrintf(psSession, "Connects     %lu\n", (unsigned long) sg_u32Connects);
	ConsolePrintf(psSession, "Disconnects  %lu\n", (unsigned long) sg_u32Disconnects);
	ConsolePrintf(psSession, "Last failure %s\n", WifiLinkText(sg_s32LastFailure));
}

EStatus WifiInit(void)
{
	if (0 != cyw43_arch_init())
	{
		return(ESTATUS_HARDWARE);
	}

	cyw43_arch_enable_sta_mode();

	// Latency over power - this is a serial bridge
	(void) cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);

	WifiApplyAddress();

	if (g_sSettings.sWifi.bEnabled)
	{
		sg_eState = EWIFISTATE_START;
	}
	else
	{
		sg_eState = EWIFISTATE_DISABLED;
	}

	return(ESTATUS_OK);
}
