//*****************************************************************************
//
// relay_pulse.h - Timed relay pulse (close for N ms, then auto-open).
//
// Usage:
//   Call RelayPulseTick(SYSTICKMS) from the main loop every SysTick.
//   Call RelayPulseStart(relay, ms) to close a relay for a fixed duration.
//   The relay opens automatically and MQTTAppPublishRelayState() is called.
//
// MQTT topic: <base>/relay/<N>/pulse   payload: duration in ms (e.g. "500")
//
//*****************************************************************************

#ifndef __RELAY_PULSE_H__
#define __RELAY_PULSE_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Start a timed pulse on relay iRelay.  The relay is immediately set ON and
// will be set OFF after ui32Ms milliseconds.  Minimum resolved granularity is
// SYSTICKMS (10 ms).  Calling while a pulse is already active restarts the
// timer.  ui32Ms == 0 cancels a running pulse without changing relay state.
//
void RelayPulseStart(int iRelay, uint32_t ui32Ms);

//
// True while relay iRelay has an active pulse countdown.
//
bool RelayPulseActive(int iRelay);

//
// Advance all pulse timers by ui32ElapsedMs milliseconds.  Call from the
// main loop after each SysTick (typically RelayPulseTick(SYSTICKMS)).
// Relays that expire are set OFF and their MQTT state is published.
//
void RelayPulseTick(uint32_t ui32ElapsedMs);

#ifdef __cplusplus
}
#endif

#endif // __RELAY_PULSE_H__
