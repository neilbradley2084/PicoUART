// Target power / reset control.
//
// ATX mode
//   PS_ON# drives the supply, PWR_OK reports it.  Reset is asserted whenever the
//   target is not fully on, is asserted before PS_ON# is released, and is released
//   only u32ResetHoldMs after PWR_OK.  If PWR_OK does not arrive within
//   u32PwrOkTimeoutMs, PS_ON# is released and power stays off.  If PWR_OK drops
//   while on, core 1 asserts reset within one pass of its loop (microseconds, and
//   unaffected by flash writes on core 0); core 0 then releases PS_ON#.  Both are
//   failures: the power LED blinks 1 Hz and "last state" becomes off.
//
// Panel mode
//   Power is a pulse (button press) or a level (held while on).  Reset is a pulse
//   (open collector button press) or a level (a real reset line: held whenever the
//   target is not on, released u32ResetHoldMs after it comes on).  With sensing on,
//   a power-up not sensed within u32SenseTimeoutMs is undone: level mode releases
//   the output, pulse mode does a press-and-hold override.
//
// Front panel buttons pass through: in panel pulse mode the power button is mirrored
// onto the output; in ATX and panel level mode it toggles on/off.  The reset button
// holds reset for as long as it is pressed, in both modes.
//
// Pins survive a reboot of this device.  Outputs are driven with IO_BANK0 output
// overrides rather than SIO, the reboot (Main.c) and boot-time resets (BootResets.c)
// leave IO_BANK0 / PADS_BANK0 alone, and the logical state is kept in a watchdog
// scratch register.  On a warm boot the state is adopted and every output is
// rewritten with the value it already has.
//
// Core 0 owns all pins except that core 1 may assert reset (ATX guard).  A hardware
// spinlock orders core 0's reset release against core 1's assertion.

#include "Power.h"
#include "Settings.h"
#include "Config.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/structs/watchdog.h"

typedef enum EPowerState
{
	EPOWERSTATE_OFF = 0,
	EPOWERSTATE_STARTING,			// Waiting for PWR_OK / sense
	EPOWERSTATE_HOLD,				// ATX: PWR_OK seen, holding reset
	EPOWERSTATE_ON,
	EPOWERSTATE_STOPPING,			// Panel: soft-off pressed, waiting for sense to drop
	EPOWERSTATE_FORCING,			// Panel: press-and-hold override in progress
} EPowerState;

typedef enum EPress
{
	EPRESS_ON = 0,
	EPRESS_OFF,
	EPRESS_FORCE,
	EPRESS_FAILOFF,					// Override after a failed power-up
} EPress;

typedef struct SButton
{
	uint8_t u8Pin;
	bool bRaw;
	bool bPressed;					// Debounced
	uint64_t u64ChangeUs;
} SButton;

// Shared with core 1
typedef struct SPowerGuard
{
	volatile bool bArmed;			// ATX, PWR_OK established: watch for loss
	volatile bool bTripped;			// Core 1 asserted reset on PWR_OK loss
	volatile bool bResetLevel;		// Pin level that asserts reset
	volatile bool bResetOc;			// Reset is open collector
} SPowerGuard;

// Watchdog scratch[0] / [1]: logical state carried across a warm reboot
#define POWER_SCRATCH_MAGIC			0x5a000000
#define POWER_SCRATCH_MAGIC_MASK	0xff000000
#define POWER_SCRATCH_ATX			(1u << 0)
#define POWER_SCRATCH_STATE_LSB		1				// 3 bits
#define POWER_SCRATCH_LEVELON		(1u << 4)
#define POWER_SCRATCH_FAILED		(1u << 5)
#define POWER_SCRATCH_RESETHELD		(1u << 6)
#define POWER_SCRATCH_RESETPULSE	(1u << 7)		// Timed actions in progress: restarted in full
#define POWER_SCRATCH_SETTLING		(1u << 8)
#define POWER_SCRATCH_PRESS			(1u << 9)
#define POWER_SCRATCH_PRESS_LSB		10				// 2 bits, EPress
#define POWER_SCRATCH_TRIPPED		(1u << 12)		// Core 1 asserted reset on PWR_OK loss

static const char *sg_peStateNames[] =
{
	"off",
	"starting",
	"holding reset",
	"on",
	"stopping",
	"forcing off",
};

static SPowerGuard sg_sGuard;
static spin_lock_t *sg_psLock;

static EPowerState sg_eState = EPOWERSTATE_OFF;
static uint64_t sg_u64DeadlineUs;
static bool sg_bLevelOn;					// Panel level mode power output
static bool sg_bPressActive;
static EPress sg_ePress;
static uint64_t sg_u64PressEndUs;
static bool sg_bResetPulse;
static uint64_t sg_u64ResetPulseEndUs;
static bool sg_bResetHeld;					// "power reset hold"
static bool sg_bResetSettling;				// Panel level reset: hold after power-up
static uint64_t sg_u64ResetSettleEndUs;
static bool sg_bFailed;						// Power LED blinks
static bool sg_bWarmBoot;					// State adopted from before a reboot
static bool sg_bWarmLastOff;				// Adoption turned "last state" off - save it
static const char *sg_peLastEvent = "none";

// What is currently driven, so pins are written only on change
static bool sg_bPowerDriven;
static bool sg_bResetDriven;
static bool sg_bForceWrite = true;

static uint32_t sg_u32Ons;
static uint32_t sg_u32Offs;
static uint32_t sg_u32Failures;
static uint32_t sg_u32PwrOkLosses;

static SButton sg_sPowerButton = { CONFIG_BUTTON_PIN_POWER, false, false, 0 };
static SButton sg_sResetButton = { CONFIG_BUTTON_PIN_RESET, false, false, 0 };

#define POWER_INLINE	static inline __attribute__((always_inline))

// Drive a pin through its IO_BANK0 output overrides (one register write, independent
// of SIO, so it survives a processor reset).  Open collector: high = released.
POWER_INLINE void PinWrite(uint32_t u32Pin,
                           bool bLevel,
                           bool bOpenCollector)
{
	uint32_t u32Ctrl = IO_BANK0_GPIO0_CTRL_FUNCSEL_VALUE_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;

	if (bOpenCollector && bLevel)
	{
		u32Ctrl |= (IO_BANK0_GPIO0_CTRL_OEOVER_VALUE_DISABLE << IO_BANK0_GPIO0_CTRL_OEOVER_LSB) |
				   (IO_BANK0_GPIO0_CTRL_OUTOVER_VALUE_LOW << IO_BANK0_GPIO0_CTRL_OUTOVER_LSB);
	}
	else
	{
		u32Ctrl |= (IO_BANK0_GPIO0_CTRL_OEOVER_VALUE_ENABLE << IO_BANK0_GPIO0_CTRL_OEOVER_LSB) |
				   (((bLevel) ? IO_BANK0_GPIO0_CTRL_OUTOVER_VALUE_HIGH : IO_BANK0_GPIO0_CTRL_OUTOVER_VALUE_LOW) <<
					IO_BANK0_GPIO0_CTRL_OUTOVER_LSB);
	}

	io_bank0_hw->io[u32Pin].ctrl = u32Ctrl;
}

POWER_INLINE bool PinRead(uint32_t u32Pin)
{
	return((sio_hw->gpio_in & (1u << u32Pin)) ? true : false);
}

// True if a pin is under our output override (i.e. we drove it before a warm reboot)
static bool PinOverridden(uint32_t u32Pin)
{
	return((io_bank0_hw->io[u32Pin].ctrl & IO_BANK0_GPIO0_CTRL_OEOVER_BITS) ? true : false);
}

// Pad: input enabled, output allowed, de-isolated.  Rewriting the same values on a
// warm boot changes nothing on the pin.
static void PinPad(uint32_t u32Pin)
{
	hw_write_masked(&pads_bank0_hw->io[u32Pin],
					PADS_BANK0_GPIO0_IE_BITS,
					PADS_BANK0_GPIO0_IE_BITS | PADS_BANK0_GPIO0_OD_BITS);
#ifdef PADS_BANK0_GPIO0_ISO_BITS
	hw_clear_bits(&pads_bank0_hw->io[u32Pin], PADS_BANK0_GPIO0_ISO_BITS);
#endif
}

// Pin level for an asserted / deasserted signal
static bool PolarityLevel(EPinPolarity ePolarity,
                          bool bAssert)
{
	return((bAssert == (EPINPOLARITY_HIGH == ePolarity)) ? true : false);
}

static void PinPulls(uint32_t u32Pin,
                     EPinPull ePull)
{
	gpio_set_pulls(u32Pin, (EPINPULL_UP == ePull) ? true : false, (EPINPULL_DOWN == ePull) ? true : false);
}

// Active low LEDs
static void LedWrite(uint32_t u32Pin,
                     bool bOn)
{
	PinWrite(u32Pin, (bOn) ? false : true, false);
}

static bool PowerIsAtx(void)
{
	return((EPOWERMODE_ATX == g_sSettings.sPower.eMode) ? true : false);
}

static bool PowerPanelResetLevel(void)
{
	return(((false == PowerIsAtx()) && (EPANELACTION_LEVEL == g_sSettings.sPower.eResetAction)) ? true : false);
}

// Reset pin drive: panel pulse reset is always open collector
static bool PowerResetOc(void)
{
	if ((false == PowerIsAtx()) && (false == PowerPanelResetLevel()))
	{
		return(true);
	}

	return((EPINDRIVE_OPENCOLLECTOR == g_sSettings.sPower.eResetDrive) ? true : false);
}

// Power reported by the supply / sense input
static bool PowerSensed(void)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;

	if (PowerIsAtx())
	{
		return(PinRead(CONFIG_POWER_PIN_SENSE));
	}

	if (false == psPower->bSense)
	{
		return(false);
	}

	return((PinRead(CONFIG_POWER_PIN_SENSE) == PolarityLevel(psPower->eSensePolarity, true)) ? true : false);
}

static void PowerEvent(const char *peEvent)
{
	sg_peLastEvent = peEvent;
	LogMsg("power: %s", peEvent);
}

// Remember the commanded state for startup "last"
static void PowerRemember(bool bOn)
{
	if (g_sSettings.sPower.bLastOn != bOn)
	{
		g_sSettings.sPower.bLastOn = bOn;
		(void) SettingsSave();
	}
}

static void PowerPress(EPress ePress,
                       uint32_t u32Ms)
{
	sg_ePress = ePress;
	sg_bPressActive = true;
	sg_u64PressEndUs = time_us_64() + ((uint64_t) u32Ms * 1000);
}

// --------------------------------------------------------------------------
// Core 1
// --------------------------------------------------------------------------

void __not_in_flash_func(PowerCore1Service)(void)
{
	if ((false == sg_sGuard.bArmed) || PinRead(CONFIG_POWER_PIN_SENSE))
	{
		return;
	}

	spin_lock_unsafe_blocking(sg_psLock);
	if (sg_sGuard.bArmed)
	{
		// Recorded first: a reboot before core 0 notices must not resurrect "on"
		watchdog_hw->scratch[0] |= POWER_SCRATCH_TRIPPED;
		watchdog_hw->scratch[1] &= ~POWER_SCRATCH_TRIPPED;

		PinWrite(CONFIG_POWER_PIN_RESET, sg_sGuard.bResetLevel, sg_sGuard.bResetOc);
		PinWrite(CONFIG_LED_PIN_RESET, false, false);
		sg_sGuard.bTripped = true;
		sg_sGuard.bArmed = false;
	}

	spin_unlock_unsafe(sg_psLock);
}

static void GuardArm(void)
{
	sg_sGuard.bTripped = false;
	__dmb();
	sg_sGuard.bArmed = true;
}

static void GuardDisarm(void)
{
	sg_sGuard.bArmed = false;
	__dmb();
}

// Core 0 has acted on a trip (power is off): forget it.  Only with the guard
// disarmed and reset already asserted.
static void GuardClearTrip(void)
{
	uint32_t u32Interrupts = spin_lock_blocking(sg_psLock);

	sg_sGuard.bTripped = false;
	spin_unlock(sg_psLock, u32Interrupts);
}

// --------------------------------------------------------------------------
// Outputs
// --------------------------------------------------------------------------

static void PowerResetWrite(bool bAssert)
{
	bool bOc = PowerResetOc();
	bool bLevel = PolarityLevel(g_sSettings.sPower.eResetPolarity, bAssert);
	uint32_t u32Interrupts;

	if (bAssert)
	{
		PinWrite(CONFIG_POWER_PIN_RESET, bLevel, bOc);
		sg_bResetDriven = true;
		return;
	}

	// Never release reset over core 1's PWR_OK-loss assertion
	u32Interrupts = spin_lock_blocking(sg_psLock);
	if (false == sg_sGuard.bTripped)
	{
		PinWrite(CONFIG_POWER_PIN_RESET, bLevel, bOc);
		sg_bResetDriven = false;
	}

	spin_unlock(sg_psLock, u32Interrupts);
}

static void PowerOutWrite(bool bAssert)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;

	if (PowerIsAtx())
	{
		PinWrite(CONFIG_POWER_PIN_OUT,
				 PolarityLevel(psPower->ePsOnPolarity, bAssert),
				 (EPINDRIVE_OPENCOLLECTOR == psPower->ePsOnDrive) ? true : false);
	}
	else
	{
		PinWrite(CONFIG_POWER_PIN_OUT,
				 PolarityLevel(psPower->ePowerPolarity, bAssert),
				 (EPINDRIVE_OPENCOLLECTOR == psPower->ePowerDrive) ? true : false);
	}

	sg_bPowerDriven = bAssert;
}

// Record the logical state for a warm reboot
static void PowerScratchSave(void)
{
	uint32_t u32Value = POWER_SCRATCH_MAGIC | (((uint32_t) sg_eState) << POWER_SCRATCH_STATE_LSB);
	uint32_t u32Interrupts;

	if (PowerIsAtx())
	{
		u32Value |= POWER_SCRATCH_ATX;
	}

	if (sg_bLevelOn)
	{
		u32Value |= POWER_SCRATCH_LEVELON;
	}

	if (sg_bFailed)
	{
		u32Value |= POWER_SCRATCH_FAILED;
	}

	if (sg_bResetHeld)
	{
		u32Value |= POWER_SCRATCH_RESETHELD;
	}

	if (sg_bResetPulse)
	{
		u32Value |= POWER_SCRATCH_RESETPULSE;
	}

	if (sg_bResetSettling)
	{
		u32Value |= POWER_SCRATCH_SETTLING;
	}

	if (sg_bPressActive)
	{
		u32Value |= POWER_SCRATCH_PRESS | (((uint32_t) sg_ePress) << POWER_SCRATCH_PRESS_LSB);
	}

	// Under the lock so core 1's trip bit can't be overwritten with a stale word
	u32Interrupts = spin_lock_blocking(sg_psLock);
	if (sg_sGuard.bTripped)
	{
		u32Value |= POWER_SCRATCH_TRIPPED;
	}

	watchdog_hw->scratch[0] = u32Value;
	watchdog_hw->scratch[1] = ~u32Value;
	spin_unlock(sg_psLock, u32Interrupts);
}

// Work out what every output should be and drive whatever changed.
// Order: assert reset, then power, then release reset.
static void PowerOutputs(uint64_t u64Now)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;
	bool bPower;
	bool bReset;
	bool bLed;

	if (sg_bResetSettling && (u64Now >= sg_u64ResetSettleEndUs))
	{
		sg_bResetSettling = false;
	}

	if (PowerIsAtx())
	{
		bPower = ((EPOWERSTATE_STARTING == sg_eState) ||
				  (EPOWERSTATE_HOLD == sg_eState) ||
				  (EPOWERSTATE_ON == sg_eState)) ? true : false;
		bReset = ((sg_eState != EPOWERSTATE_ON) ||
				  sg_bResetPulse ||
				  sg_bResetHeld ||
				  sg_sResetButton.bPressed ||
				  sg_sGuard.bTripped) ? true : false;
	}
	else
	{
		bPower = (sg_bPressActive ||
				  ((EPANELACTION_LEVEL == psPower->ePowerAction) && sg_bLevelOn) ||
				  ((EPANELACTION_PULSE == psPower->ePowerAction) && sg_sPowerButton.bPressed)) ? true : false;
		bReset = (sg_bResetPulse || sg_bResetHeld || sg_sResetButton.bPressed) ? true : false;

		// Level reset: held while off or coming up - not during a shutdown, which the
		// target is still running
		if (PowerPanelResetLevel() &&
			((EPOWERSTATE_OFF == sg_eState) || (EPOWERSTATE_STARTING == sg_eState) || sg_bResetSettling))
		{
			bReset = true;
		}
	}

	if (bReset && (sg_bForceWrite || (false == sg_bResetDriven)))
	{
		PowerResetWrite(true);
	}

	if (sg_bForceWrite || (bPower != sg_bPowerDriven))
	{
		PowerOutWrite(bPower);
	}

	if ((false == bReset) && (sg_bForceWrite || sg_bResetDriven))
	{
		PowerResetWrite(false);
	}

	sg_bForceWrite = false;

	// LEDs
	LedWrite(CONFIG_LED_PIN_RESET, sg_bResetDriven);

	bLed = sg_bPowerDriven;
	if (sg_bFailed)
	{
		bLed = ((u64Now % 1000000) < 500000) ? true : false;
	}

	LedWrite(CONFIG_LED_PIN_POWER, bLed);
	LedWrite(CONFIG_LED_PIN_SENSED, PowerSensed());

	PowerScratchSave();
}

static void PowerConfigurePads(void)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;
	uint32_t u32Interrupts;

	if (PowerIsAtx())
	{
		PinPulls(CONFIG_POWER_PIN_OUT, EPINPULL_NONE);
		PinPulls(CONFIG_POWER_PIN_RESET, EPINPULL_NONE);
		PinPulls(CONFIG_POWER_PIN_SENSE, EPINPULL_DOWN);		// Absent PWR_OK = not OK
	}
	else
	{
		PinPulls(CONFIG_POWER_PIN_OUT, psPower->ePowerPull);
		PinPulls(CONFIG_POWER_PIN_RESET, psPower->eResetPull);
		PinPulls(CONFIG_POWER_PIN_SENSE,
				 (EPINPOLARITY_HIGH == psPower->eSensePolarity) ? EPINPULL_DOWN : EPINPULL_UP);
	}

	// Core 1 must never see half of a new reset configuration
	u32Interrupts = spin_lock_blocking(sg_psLock);
	sg_sGuard.bResetOc = PowerResetOc();
	sg_sGuard.bResetLevel = PolarityLevel(psPower->eResetPolarity, true);
	if (false == PowerIsAtx())
	{
		// The PWR_OK guard is ATX only; a trip left over from ATX mode must not pin reset
		sg_sGuard.bArmed = false;
		sg_sGuard.bTripped = false;
	}

	spin_unlock(sg_psLock, u32Interrupts);
}

void PowerApplySettings(bool bForgetState)
{
	if (bForgetState)
	{
		// What "on" means changed (mode / action / sensing): drop any assumed state
		// so nothing is asserted that the new mode didn't sequence itself
		sg_bPressActive = false;
		sg_bLevelOn = false;
		sg_bResetPulse = false;
		sg_bResetSettling = false;
		sg_eState = EPOWERSTATE_OFF;
		GuardDisarm();
	}

	PowerConfigurePads();
	sg_bForceWrite = true;
	PowerOutputs(time_us_64());
}

// --------------------------------------------------------------------------
// State helpers
// --------------------------------------------------------------------------

// Panel: the target is now on.  A level reset line is held a little longer.
static void PowerPanelOn(uint64_t u64Now)
{
	sg_eState = EPOWERSTATE_ON;
	if (PowerPanelResetLevel())
	{
		sg_bResetSettling = true;
		sg_u64ResetSettleEndUs = u64Now + ((uint64_t) g_sSettings.sPower.u32ResetHoldMs * 1000);
	}
}

// A power-up attempt: clears the failure blink
static void PowerAttempt(void)
{
	sg_bFailed = false;
	sg_u32Ons++;
}

// Power-up failed or power was lost: blink, and "last state" becomes off
static void PowerFailed(const char *peEvent)
{
	sg_bFailed = true;
	sg_u32Failures++;
	PowerEvent(peEvent);
	PowerRemember(false);
}

// Begin the ATX on sequence: PS_ON# asserted, reset held, waiting for PWR_OK
static void PowerAtxStart(uint64_t u64Now)
{
	GuardDisarm();
	GuardClearTrip();
	sg_eState = EPOWERSTATE_STARTING;
	sg_u64DeadlineUs = u64Now + ((uint64_t) g_sSettings.sPower.u32PwrOkTimeoutMs * 1000);
	PowerEvent("PS_ON# asserted, waiting for PWR_OK");
}

// --------------------------------------------------------------------------
// Commands
// --------------------------------------------------------------------------

static void PowerPrint(SConsoleSession *psSession,
                       const char *peText)
{
	if (psSession)
	{
		ConsolePrintf(psSession, "%s\n", peText);
	}
}

// Nothing driven and nothing running.  Without sensing in panel mode the on/off
// state is only a guess, so it does not count.
bool PowerIsIdleOff(void)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;

	if (sg_bPressActive || sg_bLevelOn)
	{
		return(false);
	}

	if ((false == PowerIsAtx()) && (false == psPower->bSense))
	{
		return(true);
	}

	return((EPOWERSTATE_OFF == sg_eState) ? true : false);
}

EStatus PowerOn(SConsoleSession *psSession)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;
	uint64_t u64Now = time_us_64();

	if (sg_bPressActive || (EPOWERSTATE_STARTING == sg_eState) || (EPOWERSTATE_HOLD == sg_eState) ||
		(EPOWERSTATE_FORCING == sg_eState))
	{
		PowerPrint(psSession, "Busy - a power sequence is in progress");
		return(ESTATUS_BUSY);
	}

	if (EPOWERSTATE_STOPPING == sg_eState)
	{
		// Soft-off press didn't take (yet) - the target is still on
		sg_eState = EPOWERSTATE_ON;
		PowerPrint(psSession, "Already on (sensed) - soft off abandoned");
		return(ESTATUS_OK);
	}

	if (EPOWERSTATE_ON == sg_eState)
	{
		PowerPrint(psSession, "Already on");
		return(ESTATUS_OK);
	}

	PowerAttempt();

	if (PowerIsAtx())
	{
		PowerAtxStart(u64Now);
	}
	else
	if (EPANELACTION_LEVEL == psPower->ePowerAction)
	{
		sg_bLevelOn = true;
		if (psPower->bSense)
		{
			sg_eState = EPOWERSTATE_STARTING;
			sg_u64DeadlineUs = u64Now + ((uint64_t) psPower->u32SenseTimeoutMs * 1000);
		}
		else
		{
			PowerPanelOn(u64Now);
		}

		PowerEvent("power output asserted");
	}
	else
	{
		if (psPower->bSense && PowerSensed())
		{
			sg_eState = EPOWERSTATE_ON;
			PowerPrint(psSession, "Already on (sensed)");
			PowerRemember(true);
			return(ESTATUS_OK);
		}

		PowerPress(EPRESS_ON, psPower->u32PowerPulseMs);
		if (psPower->bSense)
		{
			sg_eState = EPOWERSTATE_STARTING;
			sg_u64DeadlineUs = u64Now + ((uint64_t) psPower->u32SenseTimeoutMs * 1000);
		}
		else
		{
			PowerPanelOn(u64Now);
		}

		PowerEvent("power button pressed (on)");
	}

	// Pins first; the flash write for "last state" can wait
	PowerOutputs(u64Now);
	PowerRemember(true);
	return(ESTATUS_OK);
}

EStatus PowerOff(SConsoleSession *psSession)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;
	uint64_t u64Now = time_us_64();

	if (sg_bPressActive || (EPOWERSTATE_FORCING == sg_eState))
	{
		PowerPrint(psSession, "Busy - a power sequence is in progress");
		return(ESTATUS_BUSY);
	}

	if ((EPOWERSTATE_OFF == sg_eState) && (false == sg_bLevelOn))
	{
		PowerPrint(psSession, "Already off");
		PowerRemember(false);
		return(ESTATUS_OK);
	}

	sg_u32Offs++;

	if (PowerIsAtx())
	{
		// Reset is asserted, then PS_ON# released (PowerOutputs ordering), and
		// only then is the PWR_OK guard stood down
		sg_eState = EPOWERSTATE_OFF;
		PowerOutputs(u64Now);
		GuardDisarm();
		PowerEvent("reset asserted, PS_ON# released");
	}
	else
	if (EPANELACTION_LEVEL == psPower->ePowerAction)
	{
		sg_bLevelOn = false;
		sg_eState = EPOWERSTATE_OFF;
		PowerOutputs(u64Now);
		PowerEvent("power output released");
	}
	else
	{
		// Also repeats the press if an earlier soft off didn't take
		PowerPress(EPRESS_OFF, psPower->u32PowerPulseMs);
		sg_eState = (psPower->bSense) ? EPOWERSTATE_STOPPING : EPOWERSTATE_OFF;
		PowerOutputs(u64Now);
		PowerEvent("power button pressed (off)");
	}

	PowerRemember(false);
	return(ESTATUS_OK);
}

EStatus PowerForceOff(SConsoleSession *psSession)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;

	if (PowerIsAtx() || (EPANELACTION_LEVEL == psPower->ePowerAction))
	{
		// Nothing gentler to override - same as off
		return(PowerOff(psSession));
	}

	if (sg_bPressActive)
	{
		PowerPrint(psSession, "Busy - a power sequence is in progress");
		return(ESTATUS_BUSY);
	}

	sg_u32Offs++;
	PowerPress(EPRESS_FORCE, psPower->u32ForceOffHoldMs);
	sg_eState = EPOWERSTATE_FORCING;
	PowerOutputs(time_us_64());
	PowerEvent("power button held (forced off)");
	PowerRemember(false);
	return(ESTATUS_OK);
}

EStatus PowerReset(SConsoleSession *psSession)
{
	uint64_t u64Now = time_us_64();

	if ((PowerIsAtx() && (sg_eState != EPOWERSTATE_ON)) ||
		(PowerPanelResetLevel() && ((EPOWERSTATE_OFF == sg_eState) || (EPOWERSTATE_STARTING == sg_eState))))
	{
		PowerPrint(psSession, "Target is not on - reset is already asserted");
		return(ESTATUS_INVALID_PARAMETER);
	}

	if (sg_bResetPulse)
	{
		PowerPrint(psSession, "Busy - reset pulse in progress");
		return(ESTATUS_BUSY);
	}

	sg_bResetPulse = true;
	sg_u64ResetPulseEndUs = u64Now + ((uint64_t) g_sSettings.sPower.u32ResetPulseMs * 1000);
	PowerOutputs(u64Now);
	PowerEvent("reset pulsed");
	return(ESTATUS_OK);
}

EStatus PowerResetHold(SConsoleSession *psSession,
                       bool bHold)
{
	if (bHold == sg_bResetHeld)
	{
		PowerPrint(psSession, (bHold) ? "Reset is already held" : "Reset is not held");
		return(ESTATUS_OK);
	}

	sg_bResetHeld = bHold;
	PowerOutputs(time_us_64());
	PowerEvent((bHold) ? "reset held" : "reset hold released");
	return(ESTATUS_OK);
}

// Startup failed: target off, no sequencing
void PowerFailsafe(void)
{
	if (NULL == sg_psLock)
	{
		return;
	}

	sg_bPressActive = false;
	sg_bLevelOn = false;
	sg_bResetPulse = false;
	sg_bResetSettling = false;
	sg_eState = EPOWERSTATE_OFF;
	PowerOutputs(time_us_64());
	GuardDisarm();
}

// --------------------------------------------------------------------------
// Polling
// --------------------------------------------------------------------------

// Debounce; returns true on a press (not release) edge
static bool ButtonPoll(SButton *psButton,
                       uint64_t u64Now)
{
	bool bRaw = (PinRead(psButton->u8Pin)) ? false : true;	// Active low

	if (bRaw != psButton->bRaw)
	{
		psButton->bRaw = bRaw;
		psButton->u64ChangeUs = u64Now;
	}

	if ((bRaw != psButton->bPressed) &&
		((u64Now - psButton->u64ChangeUs) >= ((uint64_t) CONFIG_BUTTON_DEBOUNCE_MS * 1000)))
	{
		psButton->bPressed = bRaw;
		return(bRaw);
	}

	return(false);
}

static void PowerPollAtx(uint64_t u64Now)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;
	bool bPwrOk = PinRead(CONFIG_POWER_PIN_SENSE);

	if (((EPOWERSTATE_HOLD == sg_eState) || (EPOWERSTATE_ON == sg_eState)) &&
		(sg_sGuard.bTripped || (false == bPwrOk)))
	{
		// Reset is already asserted (core 1, or PowerOutputs below); PS_ON# goes, power stays off
		sg_eState = EPOWERSTATE_OFF;
		PowerOutputs(u64Now);
		GuardDisarm();
		GuardClearTrip();
		sg_u32PwrOkLosses++;
		PowerFailed("PWR_OK lost - reset asserted, PS_ON# released");
		return;
	}

	if (EPOWERSTATE_STARTING == sg_eState)
	{
		if (bPwrOk)
		{
			sg_eState = EPOWERSTATE_HOLD;
			sg_u64DeadlineUs = u64Now + ((uint64_t) psPower->u32ResetHoldMs * 1000);
			GuardArm();
			return;
		}

		if (u64Now >= sg_u64DeadlineUs)
		{
			sg_eState = EPOWERSTATE_OFF;
			PowerOutputs(u64Now);
			PowerFailed("no PWR_OK within timeout - PS_ON# released, power left off");
		}

		return;
	}

	if ((EPOWERSTATE_HOLD == sg_eState) && (u64Now >= sg_u64DeadlineUs))
	{
		sg_eState = EPOWERSTATE_ON;
		PowerOutputs(u64Now);
		PowerEvent("on, reset released");
	}
}

static void PowerPollPanel(uint64_t u64Now)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;
	bool bSensed = PowerSensed();

	if (sg_bPressActive && (u64Now >= sg_u64PressEndUs))
	{
		sg_bPressActive = false;

		if ((EPRESS_FORCE == sg_ePress) || (EPRESS_FAILOFF == sg_ePress))
		{
			sg_eState = EPOWERSTATE_OFF;
			if (psPower->bSense && bSensed)
			{
				PowerEvent("still sensed on after forced off");
			}
		}
	}

	if (false == psPower->bSense)
	{
		return;
	}

	if (EPOWERSTATE_STARTING == sg_eState)
	{
		if (bSensed)
		{
			PowerPanelOn(u64Now);
			PowerEvent("on (sensed)");
			return;
		}

		if (u64Now >= sg_u64DeadlineUs)
		{
			if (EPANELACTION_LEVEL == psPower->ePowerAction)
			{
				sg_bLevelOn = false;
				sg_eState = EPOWERSTATE_OFF;
				PowerOutputs(u64Now);
				PowerFailed("power not sensed within timeout - output released");
			}
			else
			{
				// Make sure it is really off
				PowerPress(EPRESS_FAILOFF, psPower->u32ForceOffHoldMs);
				sg_eState = EPOWERSTATE_FORCING;
				PowerOutputs(u64Now);
				PowerFailed("power not sensed within timeout - forcing off");
			}
		}

		return;
	}

	if ((EPOWERSTATE_STOPPING == sg_eState) && (false == bSensed) && (false == sg_bPressActive))
	{
		sg_eState = EPOWERSTATE_OFF;
		PowerEvent("off (sensed)");
		return;
	}

	// Changes not caused by us - real button, OS shutdown, power loss
	if ((EPOWERSTATE_OFF == sg_eState) && bSensed && (false == sg_bPressActive))
	{
		PowerPanelOn(u64Now);
		sg_bFailed = false;
		PowerEvent("on (sensed, not commanded)");
	}
	else
	if ((EPOWERSTATE_ON == sg_eState) && (false == bSensed) && (false == sg_bPressActive))
	{
		// Like PS_ON#: power gone means the output is released and power stays off
		sg_bLevelOn = false;
		sg_eState = EPOWERSTATE_OFF;
		PowerOutputs(u64Now);
		PowerEvent("off (sensed, not commanded)");
	}
}

void PowerPoll(void)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;
	uint64_t u64Now = time_us_64();
	bool bPowerPress;

	if (NULL == sg_psLock)
	{
		// PowerInit() failed
		return;
	}

	bPowerPress = ButtonPoll(&sg_sPowerButton, u64Now);
	(void) ButtonPoll(&sg_sResetButton, u64Now);

	if (bPowerPress)
	{
		if (PowerIsAtx() || (EPANELACTION_LEVEL == psPower->ePowerAction))
		{
			// Toggle
			if (PowerIsIdleOff() || (EPOWERSTATE_OFF == sg_eState))
			{
				(void) PowerOn(NULL);
			}
			else
			{
				(void) PowerOff(NULL);
			}
		}
		else
		if (EPOWERSTATE_OFF == sg_eState)
		{
			// Mirrored onto the output; a press while off is a power-up attempt
			sg_bFailed = false;
		}
	}

	if (sg_bResetPulse && (u64Now >= sg_u64ResetPulseEndUs))
	{
		sg_bResetPulse = false;
	}

	if (PowerIsAtx())
	{
		PowerPollAtx(u64Now);
	}
	else
	{
		PowerPollPanel(u64Now);
	}

	PowerOutputs(u64Now);
}

// --------------------------------------------------------------------------
// Startup
// --------------------------------------------------------------------------

static bool PowerStartupWantsOn(void)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;

	if (EPOWERSTARTUP_ON == psPower->eStartup)
	{
		return(true);
	}

	if (EPOWERSTARTUP_LAST == psPower->eStartup)
	{
		return(psPower->bLastOn);
	}

	return(false);
}

// After a software reboot of this device: pick the state back up so every pin is
// rewritten with the value it already has.  Returns false on a cold boot.
static bool PowerAdoptWarm(uint64_t u64Now)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;
	uint32_t u32Value = watchdog_hw->scratch[0];
	EPowerState eState;

	if ((watchdog_hw->scratch[1] != ~u32Value) ||
		((u32Value & POWER_SCRATCH_MAGIC_MASK) != POWER_SCRATCH_MAGIC) ||
		(((u32Value & POWER_SCRATCH_ATX) ? true : false) != PowerIsAtx()) ||
		(false == PinOverridden(CONFIG_POWER_PIN_OUT)) ||
		(false == PinOverridden(CONFIG_POWER_PIN_RESET)))
	{
		// Power-on, RUN pin, BOOTSEL or a mode change: the pins were reset
		return(false);
	}

	eState = (EPowerState) ((u32Value >> POWER_SCRATCH_STATE_LSB) & 7);
	sg_bLevelOn = (u32Value & POWER_SCRATCH_LEVELON) ? true : false;
	sg_bFailed = (u32Value & POWER_SCRATCH_FAILED) ? true : false;
	sg_bResetHeld = (u32Value & POWER_SCRATCH_RESETHELD) ? true : false;

	// Timed actions that were running restart in full - never cut short
	if (u32Value & POWER_SCRATCH_RESETPULSE)
	{
		sg_bResetPulse = true;
		sg_u64ResetPulseEndUs = u64Now + ((uint64_t) psPower->u32ResetPulseMs * 1000);
	}

	if (u32Value & POWER_SCRATCH_SETTLING)
	{
		sg_bResetSettling = true;
		sg_u64ResetSettleEndUs = u64Now + ((uint64_t) psPower->u32ResetHoldMs * 1000);
	}

	if ((false == PowerIsAtx()) && (u32Value & POWER_SCRATCH_PRESS))
	{
		EPress ePress = (EPress) ((u32Value >> POWER_SCRATCH_PRESS_LSB) & 3);

		PowerPress(ePress,
				   ((EPRESS_FORCE == ePress) || (EPRESS_FAILOFF == ePress)) ?
				   psPower->u32ForceOffHoldMs : psPower->u32PowerPulseMs);
	}

	if (PowerIsAtx())
	{
		if (((EPOWERSTATE_ON == eState) || (EPOWERSTATE_HOLD == eState)) &&
			((u32Value & POWER_SCRATCH_TRIPPED) || (false == PinRead(CONFIG_POWER_PIN_SENSE))))
		{
			// PWR_OK was lost (before or during the reboot): reset, power off, failure
			sg_eState = EPOWERSTATE_OFF;
			sg_bFailed = true;
			sg_u32PwrOkLosses++;
			sg_peLastEvent = "PWR_OK lost - reset asserted, PS_ON# released";
			if (g_sSettings.sPower.bLastOn)
			{
				g_sSettings.sPower.bLastOn = false;
				sg_bWarmLastOff = true;
			}
		}
		else
		if ((EPOWERSTATE_ON == eState) || (EPOWERSTATE_HOLD == eState))
		{
			// Still powered: carry on (core 1 starts right after PowerInit)
			sg_eState = eState;
			sg_u64DeadlineUs = u64Now + ((uint64_t) psPower->u32ResetHoldMs * 1000);
			GuardArm();
		}
		else
		if ((EPOWERSTATE_STARTING == eState) && PinRead(CONFIG_POWER_PIN_SENSE))
		{
			// PWR_OK arrived while we rebooted: normal reset hold from here
			sg_eState = EPOWERSTATE_HOLD;
			sg_u64DeadlineUs = u64Now + ((uint64_t) psPower->u32ResetHoldMs * 1000);
			GuardArm();
		}
		else
		if (EPOWERSTATE_STARTING == eState)
		{
			// The time already spent is unknown, so no more waiting: PS_ON# released
			sg_eState = EPOWERSTATE_OFF;
			sg_bFailed = true;
			sg_u32Failures++;
			sg_peLastEvent = "no PWR_OK by the end of a reboot - PS_ON# released, power left off";
			if (g_sSettings.sPower.bLastOn)
			{
				g_sSettings.sPower.bLastOn = false;
				sg_bWarmLastOff = true;
			}
		}
		else
		{
			sg_eState = EPOWERSTATE_OFF;
		}
	}
	else
	{
		if ((false == psPower->bSense) && ((EPOWERSTATE_STARTING == eState) || (EPOWERSTATE_STOPPING == eState)))
		{
			eState = (EPOWERSTATE_STARTING == eState) ? EPOWERSTATE_ON : EPOWERSTATE_OFF;
		}

		sg_eState = eState;
		sg_u64DeadlineUs = u64Now + ((uint64_t) psPower->u32SenseTimeoutMs * 1000);
	}

	return(true);
}

EStatus PowerInit(void)
{
	static const uint8_t sg_u8Outputs[] =
	{
		CONFIG_POWER_PIN_OUT,
		CONFIG_POWER_PIN_RESET,
		CONFIG_LED_PIN_RESET,
		CONFIG_LED_PIN_POWER,
		CONFIG_LED_PIN_SENSED,
		0xff
	};
	const SPowerSettings *psPower = &g_sSettings.sPower;
	uint64_t u64Now = time_us_64();
	uint32_t u32Loop;
	int32_t s32Lock;

	s32Lock = spin_lock_claim_unused(false);
	if (s32Lock < 0)
	{
		return(ESTATUS_HARDWARE);
	}

	sg_psLock = spin_lock_init((uint) s32Lock);

	// Inputs (SIO, no override)
	gpio_init(CONFIG_POWER_PIN_SENSE);
	gpio_init(CONFIG_BUTTON_PIN_POWER);
	gpio_init(CONFIG_BUTTON_PIN_RESET);
	gpio_pull_up(CONFIG_BUTTON_PIN_POWER);
	gpio_pull_up(CONFIG_BUTTON_PIN_RESET);
	busy_wait_us(50);									// Pull-ups settle

	// A button held through a reboot stays held - no release for the debounce time
	sg_sPowerButton.bRaw = (PinRead(CONFIG_BUTTON_PIN_POWER)) ? false : true;
	sg_sPowerButton.bPressed = sg_sPowerButton.bRaw;
	sg_sPowerButton.u64ChangeUs = u64Now;
	sg_sResetButton.bRaw = (PinRead(CONFIG_BUTTON_PIN_RESET)) ? false : true;
	sg_sResetButton.bPressed = sg_sResetButton.bRaw;
	sg_sResetButton.u64ChangeUs = u64Now;

	sg_bWarmBoot = PowerAdoptWarm(u64Now);
	if (false == sg_bWarmBoot)
	{
		// Cold: startup wants the target on - keep the supply (ATX) / level output
		// asserted through our own init rather than dropping it for seconds.  Reset
		// stays asserted until the normal PWR_OK / sense sequence releases it.
		sg_eState = EPOWERSTATE_OFF;
		if (PowerStartupWantsOn())
		{
			if (PowerIsAtx())
			{
				// The PWR_OK timeout runs from PS_ON# assertion, i.e. now
				PowerAttempt();
				sg_eState = EPOWERSTATE_STARTING;
				sg_u64DeadlineUs = u64Now + ((uint64_t) psPower->u32PwrOkTimeoutMs * 1000);
			}
			else
			if (EPANELACTION_LEVEL == psPower->ePowerAction)
			{
				sg_bLevelOn = true;
				sg_eState = EPOWERSTATE_OFF;	// PowerStartup() takes it from here
			}
		}
	}

	// Outputs: values first (overrides), then the pads.  On a warm boot both are
	// rewritten with what they already hold.
	PowerApplySettings(false);
	for (u32Loop = 0; sg_u8Outputs[u32Loop] != 0xff; u32Loop++)
	{
		PinPad(sg_u8Outputs[u32Loop]);
	}

	return(ESTATUS_OK);
}

void PowerStartup(void)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;
	uint64_t u64Now = time_us_64();

	if (sg_bWarmBoot)
	{
		if (sg_bWarmLastOff)
		{
			// PWR_OK lost around the reboot: make "last state" off stick
			(void) SettingsSave();
		}

		LogMsg("power: warm boot, state adopted (%s)", sg_peStateNames[sg_eState]);
		return;
	}

	if (PowerIsAtx())
	{
		if (EPOWERSTATE_STARTING == sg_eState)
		{
			// PS_ON# asserted (and its PWR_OK timeout running) since PowerInit()
			PowerEvent("PS_ON# asserted at startup, waiting for PWR_OK");
		}

		return;
	}

	// Panel with sensing: an already-running target is simply on
	if (psPower->bSense && PowerSensed())
	{
		if (EPANELACTION_LEVEL == psPower->ePowerAction)
		{
			sg_bLevelOn = true;
		}

		PowerPanelOn(u64Now);
		PowerOutputs(u64Now);
		PowerEvent("on at startup (sensed)");
		return;
	}

	if (EPANELACTION_LEVEL == psPower->ePowerAction)
	{
		if (sg_bLevelOn)
		{
			// Output asserted since PowerInit()
			PowerAttempt();
			if (psPower->bSense)
			{
				sg_eState = EPOWERSTATE_STARTING;
				sg_u64DeadlineUs = u64Now + ((uint64_t) psPower->u32SenseTimeoutMs * 1000);
			}
			else
			{
				PowerPanelOn(u64Now);
			}

			PowerOutputs(u64Now);
			PowerEvent("power output asserted at startup");
		}

		return;
	}

	if (PowerStartupWantsOn())
	{
		(void) PowerOn(NULL);
	}
}

// --------------------------------------------------------------------------
// Statistics
// --------------------------------------------------------------------------

void PowerStatsPrint(SConsoleSession *psSession)
{
	const SPowerSettings *psPower = &g_sSettings.sPower;
	bool bAtx = PowerIsAtx();

	ConsolePrintf(psSession, "Mode         %s\n", bAtx ? "ATX" : "panel");
	ConsolePrintf(psSession, "State        %s%s%s\n",
				  sg_peStateNames[sg_eState],
				  ((false == bAtx) && (false == psPower->bSense) && (sg_eState != EPOWERSTATE_OFF)) ? " (assumed, no sensing)" : "",
				  sg_bFailed ? " - FAILED (power-up failed or power lost)" : "");
	ConsolePrintf(psSession, "%s  GP%u %s\n",
				  bAtx ? "PS_ON#     " : "Power out  ",
				  CONFIG_POWER_PIN_OUT,
				  sg_bPowerDriven ? "asserted" : "released");
	ConsolePrintf(psSession, "Reset        GP%u %s%s\n",
				  CONFIG_POWER_PIN_RESET,
				  sg_bResetDriven ? "asserted" : "released",
				  sg_bResetHeld ? " (held)" : "");

	if (bAtx || psPower->bSense)
	{
		ConsolePrintf(psSession, "%s  GP%u %s\n",
					  bAtx ? "PWR_OK     " : "Sense      ",
					  CONFIG_POWER_PIN_SENSE,
					  PowerSensed() ? "power present" : "no power");
	}
	else
	{
		ConsolePrintf(psSession, "Sense        disabled\n");
	}

	ConsolePrintf(psSession, "Buttons      power GP%u %s, reset GP%u %s\n",
				  CONFIG_BUTTON_PIN_POWER, sg_sPowerButton.bPressed ? "pressed" : "released",
				  CONFIG_BUTTON_PIN_RESET, sg_sResetButton.bPressed ? "pressed" : "released");
	ConsolePrintf(psSession, "LEDs         reset GP%u, power GP%u, sensed GP%u (active low)\n",
				  CONFIG_LED_PIN_RESET, CONFIG_LED_PIN_POWER, CONFIG_LED_PIN_SENSED);
	ConsolePrintf(psSession, "Boot         %s\n", sg_bWarmBoot ? "warm - pins kept through reboot" : "cold");
	ConsolePrintf(psSession, "Last event   %s\n", sg_peLastEvent);
	ConsolePrintf(psSession, "Counts       on %lu, off %lu, failures %lu, PWR_OK losses %lu\n",
				  (unsigned long) sg_u32Ons,
				  (unsigned long) sg_u32Offs,
				  (unsigned long) sg_u32Failures,
				  (unsigned long) sg_u32PwrOkLosses);
}
