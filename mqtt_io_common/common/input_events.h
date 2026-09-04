//*****************************************************************************
//
// input_events.h - Per-input debounce and single/double-click detection for
//                  pushbutton-type inputs.
//
//*****************************************************************************

#ifndef __INPUT_EVENTS_H__
#define __INPUT_EVENTS_H__

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#ifdef __cplusplus
extern "C"
{
#endif

//
// Register the callback that fires when a click event is confirmed.  The
// callback receives the zero-based input index and one of the strings
// "single" or "double".  Must be called once at start-up before any ticks.
//
void InputEventsSetCallback(void (*pfnCallback)(int iInput, const char *pcEvt));

//
// Register an optional predicate that returns true when the given input has a
// double-click action that must be detected.  When it returns false, the click
// detector fires "single" on the confirmed press edge (~30 ms) instead of
// waiting out the double-click window on release, removing ~350 ms of latency
// for single-only inputs.  If never registered, the detector always waits
// (double-click always enabled).
//
void InputEventsSetDoubleQuery(bool (*pfnNeedsDouble)(int iInput));

//
// Feed raw (post-scan) level for one pushbutton input.  Call once per scan
// cycle for every input that is currently configured as a pushbutton.
// bActive = true while the physical input is asserted.
//
void InputEventsUpdate(int iInput, bool bActive);

//
// Advance all timers by ui32Ms milliseconds.  Call from the main loop at
// each system tick regardless of scan results.
//
void InputEventsTick(uint32_t ui32Ms);

#ifdef __cplusplus
}
#endif

#endif // __INPUT_EVENTS_H__
