//*****************************************************************************
//
// output_ctrl.h - Per-output behavior: Standard / Timed / Shutter.
//
// Central router that turns a logical output command into the right physical
// action based on the output's configured mode (see tOutputConfig in config.h):
//
//   Standard : plain ON/OFF.
//   Timed    : ON starts an auto-OFF countdown (reuses relay_pulse); OFF cancels.
//   Shutter  : a pair of relays (UP/DOWN) driven by an interlocked state machine
//              with mutual exclusion, a 500 ms direction-change delay, and a
//              travel-time auto-stop.  Exposed to MQTT/HA as a cover entity.
//
// Call OutputCtrlTick(SYSTICKMS) every SysTick from the main loop, and
// OutputCtrlReload() once after RelayChainInit() and again whenever the output
// configuration changes via the web UI.
//
//*****************************************************************************

#ifndef __OUTPUT_CTRL_H__
#define __OUTPUT_CTRL_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Relay-level command (from MQTT /relay/set, input bindings, or the web UI).
//
typedef enum { OUT_CMD_OFF = 0, OUT_CMD_ON, OUT_CMD_TOGGLE, OUT_CMD_CYCLE } tOutCmd;

//
// Shutter command.  Two flavors of open/close:
//   OPEN / CLOSE : direct — always move that way (reverse if already moving the
//                  other way).  Used by MQTT cover OPEN/CLOSE (proper HA).
//   UP   / DOWN  : momentary button — from idle move that way, but while moving
//                  (either direction) act as STOP.  Used by the web buttons and
//                  pushbutton bindings.
//   STOP         : stop and cancel timers.
//   TOGGLE       : single-button cycle — open -> stop -> close -> stop -> ...
//
typedef enum
{
    SH_CMD_OPEN = 0,
    SH_CMD_CLOSE,
    SH_CMD_STOP,
    SH_CMD_UP,
    SH_CMD_DOWN,
    SH_CMD_TOGGLE
}
tShCmd;

//
// (Re)load the shutter table and mode map from the live configuration.  Safe to
// call at runtime; a changed or removed shutter definition stops its relays.
//
void OutputCtrlReload(void);

//
// Route a relay-level command to output iOut according to its mode.  Shutter
// member relays are translated to shutter commands (UP-relay ON = up, DOWN-relay
// ON = down, either OFF = stop, TOGGLE = stop-if-moving-else-move).
//
void OutputCtrlCommand(int iOut, tOutCmd eCmd);

//
// Drive shutter iShutter (0..CFG_MAX_SHUTTERS-1) directly.
//
void OutputCtrlShutter(int iShutter, tShCmd eCmd);

//
// Advance shutter travel + interlock timers by ui32Ms milliseconds.
//
void OutputCtrlTick(uint32_t ui32Ms);

//
// True if relay iOut is the UP or DOWN member of a configured shutter (used to
// suppress switch discovery for those relays in the MQTT layer).
//
bool OutputCtrlIsShutterMember(int iOut);

//
// True if shutter slot iShutter is configured (has a valid UP/DOWN pair).
//
bool OutputCtrlShutterValid(int iShutter);

//
// Current cover-state string for shutter iShutter:
// "opening" | "closing" | "open" | "closed" | "stopped".
//
const char *OutputCtrlCoverState(int iShutter);

#ifdef __cplusplus
}
#endif

#endif // __OUTPUT_CTRL_H__
