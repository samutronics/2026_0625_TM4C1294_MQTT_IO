//*****************************************************************************
//
// output_ctrl.c - Per-output behavior: Standard / Timed / Shutter.
//
// See output_ctrl.h for the model.  The shutter logic is an event-driven state
// machine (Idle / MovingUp / MovingDown / SwitchingDirection) that guarantees
// the two relays of a shutter are never energized simultaneously: the opposite
// relay is always turned OFF before entering SwitchingDirection, and the newly
// requested relay is only energized after a mandatory 500 ms interlock delay.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "utils/uartstdio.h"
#include "config.h"
#include "relay_chain.h"
#include "relay_pulse.h"
#include "mqtt_app.h"
#include "output_ctrl.h"

//
// Mandatory delay between de-energizing one shutter relay and energizing the
// opposite one, to guarantee the previous contactor has fully disengaged.
//
#define SHUTTER_INTERLOCK_MS  500u

//
// Cover-state strings (also the MQTT payloads).  Kept as file-scope constants so
// the runtime state can hold a pointer without copying.
//
static const char PCS_OPENING[] = "opening";
static const char PCS_CLOSING[] = "closing";
static const char PCS_OPEN[]    = "open";
static const char PCS_CLOSED[]  = "closed";
static const char PCS_STOPPED[] = "stopped";

typedef enum
{
    SH_IDLE = 0,
    SH_MOVING_UP,
    SH_MOVING_DOWN,
    SH_SWITCH_DELAY
}
tShState;

typedef struct
{
    bool        bValid;
    uint8_t     ui8Up;
    uint8_t     ui8Down;
    uint32_t    ui32TravelMs;
    tShState    eState;
    uint32_t    ui32TimerMs;      // travel countdown while MOVING_*
    uint32_t    ui32InterlockMs;  // interlock countdown while SWITCH_DELAY
    bool        bPendingUp;       // direction to energize after the interlock
    bool        bLastUp;          // last commanded direction (single-button cycle)
    const char *pcState;          // last published cover state
}
tShutterRt;

static tShutterRt g_asSh[CFG_MAX_SHUTTERS];

//*****************************************************************************
//
// Publish and remember a cover state.
//
//*****************************************************************************
static void
ShPublish(int iSh, const char *pcState)
{
    g_asSh[iSh].pcState = pcState;
    MQTTAppPublishCoverState(iSh, pcState);
}

//*****************************************************************************
//
// Force both relays of a shutter OFF (raw chain write, no switch-state publish).
//
//*****************************************************************************
static void
ShBothOff(tShutterRt *p)
{
    RelayChainSet(p->ui8Up,   false);
    RelayChainSet(p->ui8Down, false);
}

//*****************************************************************************
//
// (Re)load the shutter table from the live configuration.
//
//*****************************************************************************
void
OutputCtrlReload(void)
{
    int i;

    for(i = 0; i < CFG_MAX_SHUTTERS; i++)
    {
        tShutterRt *p = &g_asSh[i];
        uint8_t     ui8Up, ui8Down;
        uint32_t    ui32Travel;

        if(ConfigShutterGet(i, &ui8Up, &ui8Down, &ui32Travel))
        {
            //
            // If a slot's relay pairing changed (or it is newly defined), stop
            // any motion and leave both relays safely OFF.
            //
            if(!p->bValid || (p->ui8Up != ui8Up) || (p->ui8Down != ui8Down))
            {
                RelayChainSet(ui8Up,   false);
                RelayChainSet(ui8Down, false);
                p->eState         = SH_IDLE;
                p->ui32TimerMs    = 0;
                p->ui32InterlockMs = 0;
                p->pcState        = PCS_STOPPED;
            }
            p->bValid       = true;
            p->ui8Up        = ui8Up;
            p->ui8Down      = ui8Down;
            p->ui32TravelMs = ui32Travel;
        }
        else
        {
            //
            // Slot removed: make sure its former relays are not left energized.
            //
            if(p->bValid)
            {
                ShBothOff(p);
            }
            p->bValid         = false;
            p->eState         = SH_IDLE;
            p->ui32TimerMs    = 0;
            p->ui32InterlockMs = 0;
        }
    }
}

//*****************************************************************************
//
// Stop a shutter: both relays off, timers cancelled, state Idle.
//
//*****************************************************************************
static void
ShStop(int iSh, tShutterRt *p)
{
    ShBothOff(p);
    p->eState          = SH_IDLE;
    p->ui32TimerMs     = 0;
    p->ui32InterlockMs = 0;
    ShPublish(iSh, PCS_STOPPED);
}

//*****************************************************************************
//
// Direct move in a direction.  Same direction while moving just restarts the
// travel timer (no relay cycle); the opposite direction de-energizes first and
// enters the 500 ms interlock before the new relay is energized (in the tick).
//
//*****************************************************************************
static void
ShMove(int iSh, tShutterRt *p, bool bUp)
{
    if(bUp && (p->eState == SH_MOVING_UP))
    {
        p->ui32TimerMs = p->ui32TravelMs;    // refresh, do not cycle relay
        return;
    }
    if(!bUp && (p->eState == SH_MOVING_DOWN))
    {
        p->ui32TimerMs = p->ui32TravelMs;
        return;
    }

    p->bLastUp = bUp;

    if(p->eState == SH_IDLE)
    {
        RelayChainSet(bUp ? p->ui8Down : p->ui8Up, false);  // opposite OFF first
        RelayChainSet(bUp ? p->ui8Up   : p->ui8Down, true);
        p->eState      = bUp ? SH_MOVING_UP : SH_MOVING_DOWN;
        p->ui32TimerMs = p->ui32TravelMs;
        ShPublish(iSh, bUp ? PCS_OPENING : PCS_CLOSING);
    }
    else
    {
        //
        // Reversing (MOVING opposite) or superseding a pending SWITCH_DELAY:
        // ensure both relays OFF and (re)arm the interlock.  A supersede keeps
        // the running interlock timer — the relays are already de-energized, so
        // the guaranteed OFF window is preserved.
        //
        ShBothOff(p);
        if(p->eState != SH_SWITCH_DELAY)
        {
            p->ui32InterlockMs = SHUTTER_INTERLOCK_MS;
            p->eState = SH_SWITCH_DELAY;
        }
        p->ui32TimerMs = 0;
        p->bPendingUp  = bUp;
        ShPublish(iSh, bUp ? PCS_OPENING : PCS_CLOSING);
    }
}

//*****************************************************************************
//
// Drive one shutter.  All safety invariants are enforced here and in the tick.
//
//*****************************************************************************
void
OutputCtrlShutter(int iShutter, tShCmd eCmd)
{
    tShutterRt *p;
    bool        bMoving;

    if((iShutter < 0) || (iShutter >= CFG_MAX_SHUTTERS)) { return; }
    p = &g_asSh[iShutter];
    if(!p->bValid) { return; }

    bMoving = (p->eState != SH_IDLE);

    switch(eCmd)
    {
        case SH_CMD_STOP:
            ShStop(iShutter, p);
            break;

        case SH_CMD_OPEN:                       // direct (MQTT)
            ShMove(iShutter, p, true);
            break;

        case SH_CMD_CLOSE:
            ShMove(iShutter, p, false);
            break;

        case SH_CMD_UP:                         // momentary button
            if(bMoving) { ShStop(iShutter, p); }
            else        { ShMove(iShutter, p, true); }
            break;

        case SH_CMD_DOWN:
            if(bMoving) { ShStop(iShutter, p); }
            else        { ShMove(iShutter, p, false); }
            break;

        case SH_CMD_TOGGLE:                     // single-button cycle
            if(bMoving) { ShStop(iShutter, p); }
            else        { ShMove(iShutter, p, !p->bLastUp); }
            break;

        default:
            break;
    }
}

//*****************************************************************************
//
// Route a relay-level command by the output's mode.
//
//*****************************************************************************
void
OutputCtrlCommand(int iOut, tOutCmd eCmd)
{
    bool bIsUp = false;
    int  iSh   = ConfigShutterOfRelay(iOut, &bIsUp);
    bool bOn;

    //
    // Shutter member: translate the relay command into a shutter command.
    //
    if(iSh >= 0)
    {
        tShCmd eSh;
        if(eCmd == OUT_CMD_OFF)
        {
            eSh = SH_CMD_STOP;                       // OFF binding = stop
        }
        else if(eCmd == OUT_CMD_CYCLE)
        {
            eSh = SH_CMD_TOGGLE;                     // single-button up/stop/down
        }
        else /* OUT_CMD_ON or OUT_CMD_TOGGLE */
        {
            // The bound member decides the direction: momentary press = from idle
            // move this member's way, while moving stop.
            eSh = bIsUp ? SH_CMD_UP : SH_CMD_DOWN;
        }
        OutputCtrlShutter(iSh, eSh);
        return;
    }

    //
    // Resolve the desired level for Standard / Timed outputs.  Cycle has no
    // meaning for a non-shutter output, so treat it like Toggle.
    //
    if((eCmd == OUT_CMD_TOGGLE) || (eCmd == OUT_CMD_CYCLE))
    {
        bOn = !RelayChainGet((uint16_t)iOut);
    }
    else
    {
        bOn = (eCmd == OUT_CMD_ON);
    }

    if(ConfigOutMode(iOut) == OUT_MODE_TIMED)
    {
        if(bOn)
        {
            // RelayPulseStart sets the relay ON, publishes state, and (re)starts
            // the auto-OFF countdown with the per-output configured duration.
            RelayPulseStart(iOut, ConfigOutTimedMs(iOut));
        }
        else
        {
            RelayPulseStart(iOut, 0);       // cancel any running countdown
            MQTTAppSetRelay(iOut, false);   // OFF + publish
        }
        return;
    }

    // Standard output.
    MQTTAppSetRelay(iOut, bOn);
}

//*****************************************************************************
//
// Advance shutter timers.
//
//*****************************************************************************
void
OutputCtrlTick(uint32_t ui32Ms)
{
    int i;

    for(i = 0; i < CFG_MAX_SHUTTERS; i++)
    {
        tShutterRt *p = &g_asSh[i];
        if(!p->bValid) { continue; }

        if((p->eState == SH_MOVING_UP) || (p->eState == SH_MOVING_DOWN))
        {
            if(p->ui32TimerMs == 0) { continue; }
            if(ui32Ms >= p->ui32TimerMs)
            {
                bool bUp = (p->eState == SH_MOVING_UP);
                p->ui32TimerMs = 0;
                ShBothOff(p);
                p->eState = SH_IDLE;
                ShPublish(i, bUp ? PCS_OPEN : PCS_CLOSED);
            }
            else
            {
                p->ui32TimerMs -= ui32Ms;
            }
        }
        else if(p->eState == SH_SWITCH_DELAY)
        {
            if(ui32Ms >= p->ui32InterlockMs)
            {
                p->ui32InterlockMs = 0;
                //
                // Interlock elapsed: both relays have been OFF for >= 500 ms.
                // Energize the pending direction and start its travel timer.
                //
                if(p->bPendingUp)
                {
                    RelayChainSet(p->ui8Down, false);
                    RelayChainSet(p->ui8Up,   true);
                    p->eState = SH_MOVING_UP;
                    p->ui32TimerMs = p->ui32TravelMs;
                    ShPublish(i, PCS_OPENING);
                }
                else
                {
                    RelayChainSet(p->ui8Up,   false);
                    RelayChainSet(p->ui8Down, true);
                    p->eState = SH_MOVING_DOWN;
                    p->ui32TimerMs = p->ui32TravelMs;
                    ShPublish(i, PCS_CLOSING);
                }
            }
            else
            {
                p->ui32InterlockMs -= ui32Ms;
            }
        }
    }
}

//*****************************************************************************
//
// Queries used by the MQTT layer and web UI.
//
//*****************************************************************************
bool
OutputCtrlIsShutterMember(int iOut)
{
    return(ConfigShutterOfRelay(iOut, NULL) >= 0);
}

bool
OutputCtrlShutterValid(int iShutter)
{
    if((iShutter < 0) || (iShutter >= CFG_MAX_SHUTTERS)) { return(false); }
    return(g_asSh[iShutter].bValid);
}

const char *
OutputCtrlCoverState(int iShutter)
{
    if((iShutter < 0) || (iShutter >= CFG_MAX_SHUTTERS) ||
       !g_asSh[iShutter].bValid)
    {
        return(PCS_STOPPED);
    }
    return(g_asSh[iShutter].pcState ? g_asSh[iShutter].pcState : PCS_STOPPED);
}
