//*****************************************************************************
//
// input_events.c - Per-input debounce and single/double-click detection for
//                  pushbutton-type inputs.
//
// State machine per input:
//
//   Inputs WITH a double-click action (full single/double discrimination):
//   IDLE ──press──> DEBOUNCE ──30 ms──> DOWN ──release──> UP_WAIT
//                                                        ──350 ms timer──>
//   UP_WAIT ──press within 350 ms──> DBL_DEBOUNCE ──30 ms──> FIRE_DOUBLE
//           ──350 ms expires (1 click)──> FIRE_SINGLE
//
//   Inputs WITHOUT a double-click action (minimum latency):
//   IDLE ──press──> DEBOUNCE ──30 ms──> FIRE_SINGLE ──> HELD ──release──> IDLE
//   "single" fires on the confirmed press edge (~30 ms) instead of on release,
//   and HELD suppresses re-firing until the button is released.
//
// FIRE_SINGLE / FIRE_DOUBLE emit the callback.
// A bounce (release during DEBOUNCE) returns to IDLE silently.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "utils/uartstdio.h"
#include "input_events.h"

//
// Timing constants (milliseconds).
//
#define DEBOUNCE_MS     30u
#define DBLCLICK_MS    350u

//
// Per-input state values.
//
typedef enum
{
    IE_IDLE = 0,
    IE_DEBOUNCE,        // press detected, waiting for debounce
    IE_DOWN,            // confirmed pressed
    IE_UP_WAIT,         // released after 1st click, watching for 2nd
    IE_DBL_DEBOUNCE,    // 2nd press within window, debouncing
    IE_HELD,            // fired "single" on press edge, waiting for release
}
tIEState;

typedef struct
{
    tIEState eState;
    uint32_t ui32Timer;  // countdown in ms
}
tInputEvent;

static tInputEvent g_asEvents[CFG_MAX_INPUTS];
static void (*g_pfnCallback)(int iInput, const char *pcEvt);
static bool (*g_pfnNeedsDouble)(int iInput);

//*****************************************************************************
//
// Register the event callback.
//
//*****************************************************************************
void
InputEventsSetCallback(void (*pfnCallback)(int iInput, const char *pcEvt))
{
    g_pfnCallback = pfnCallback;
}

//*****************************************************************************
//
// Register the double-click-needed predicate (see header).
//
//*****************************************************************************
void
InputEventsSetDoubleQuery(bool (*pfnNeedsDouble)(int iInput))
{
    g_pfnNeedsDouble = pfnNeedsDouble;
}

//*****************************************************************************
//
// True when double-click detection is required for this input.  Defaults to
// true (wait out the window) when no predicate has been registered.
//
//*****************************************************************************
static bool
IENeedsDouble(int iInput)
{
    return g_pfnNeedsDouble ? g_pfnNeedsDouble(iInput) : true;
}

//*****************************************************************************
//
// Fire the callback if registered.
//
//*****************************************************************************
static void
IEFire(int iInput, const char *pcEvt)
{
    UARTprintf("InputEvents: in%d %s\n", iInput, pcEvt);
    if(g_pfnCallback)
    {
        g_pfnCallback(iInput, pcEvt);
    }
}

//*****************************************************************************
//
// Feed raw level for one pushbutton input.
//
//*****************************************************************************
void
InputEventsUpdate(int iInput, bool bActive)
{
    tInputEvent *p;

    if((iInput < 0) || (iInput >= CFG_MAX_INPUTS))
    {
        return;
    }
    p = &g_asEvents[iInput];

    switch(p->eState)
    {
        case IE_IDLE:
            if(bActive)
            {
                p->eState = IE_DEBOUNCE;
                p->ui32Timer = DEBOUNCE_MS;
            }
            break;

        case IE_DEBOUNCE:
            if(!bActive)
            {
                //
                // Bounced before debounce expired — back to idle.
                //
                p->eState = IE_IDLE;
                p->ui32Timer = 0;
            }
            //
            // Timer expiry is handled in InputEventsTick.
            //
            break;

        case IE_DOWN:
            //
            // Only reached for inputs that have a double-click action (single-
            // only inputs fire on the press edge and move straight to IE_HELD).
            // On release, open the double-click window.
            //
            if(!bActive)
            {
                p->eState = IE_UP_WAIT;
                p->ui32Timer = DBLCLICK_MS;
            }
            break;

        case IE_HELD:
            //
            // "single" already fired on the press edge; just wait for release.
            //
            if(!bActive)
            {
                p->eState = IE_IDLE;
                p->ui32Timer = 0;
            }
            break;

        case IE_UP_WAIT:
            if(bActive)
            {
                p->eState = IE_DBL_DEBOUNCE;
                p->ui32Timer = DEBOUNCE_MS;
            }
            //
            // Timer expiry (single click) is handled in InputEventsTick.
            //
            break;

        case IE_DBL_DEBOUNCE:
            if(!bActive)
            {
                //
                // Bounced — treat as end of second press; fire double now.
                //
                p->eState = IE_IDLE;
                p->ui32Timer = 0;
                IEFire(iInput, "double");
            }
            //
            // Timer expiry (confirmed second press) handled in Tick.
            //
            break;

        default:
            p->eState = IE_IDLE;
            p->ui32Timer = 0;
            break;
    }
}

//*****************************************************************************
//
// Advance all timers and fire callbacks when they expire.
//
//*****************************************************************************
void
InputEventsTick(uint32_t ui32Ms)
{
    int i;

    for(i = 0; i < CFG_MAX_INPUTS; i++)
    {
        tInputEvent *p = &g_asEvents[i];

        if(p->ui32Timer == 0)
        {
            continue;
        }

        if(ui32Ms >= p->ui32Timer)
        {
            p->ui32Timer = 0;
            switch(p->eState)
            {
                case IE_DEBOUNCE:
                    //
                    // Debounce done — press confirmed.  When the input has no
                    // double-click action, fire "single" now on the press edge
                    // for minimum latency and hold until release.  Otherwise
                    // stay pressed and discriminate single vs double on release.
                    //
                    if(!IENeedsDouble(i))
                    {
                        p->eState = IE_HELD;
                        IEFire(i, "single");
                    }
                    else
                    {
                        p->eState = IE_DOWN;
                    }
                    break;

                case IE_UP_WAIT:
                    //
                    // Double-click window expired with only 1 click.
                    //
                    p->eState = IE_IDLE;
                    IEFire(i, "single");
                    break;

                case IE_DBL_DEBOUNCE:
                    //
                    // Second press debounced — fire double and wait for release.
                    // We return to IDLE immediately; the release is ignored since
                    // we've already committed to "double".
                    //
                    p->eState = IE_IDLE;
                    IEFire(i, "double");
                    break;

                default:
                    p->eState = IE_IDLE;
                    break;
            }
        }
        else
        {
            p->ui32Timer -= ui32Ms;
        }
    }
}
