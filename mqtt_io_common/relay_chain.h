//*****************************************************************************
//
// relay_chain.h - Driver for the cascaded DRV8860 relay-output chain.
//
// The control board's output boards carry cascaded DRV8860 8-channel low-side
// drivers (8 relays each).  They are written as one long shift register: clock
// (device_count * 8) bits in on DIN, then pulse LATCH to transfer them to the
// outputs.  ENABLE gates all outputs; nFAULT is an open-drain, wired-OR fault
// line.  See HARDWARE.md sections 2.5 and 4 for the pin map and chain protocol.
//
// This first cut bit-bangs the interface on the SSI1 pins (PB5 CLK, PE4 DIN,
// PE5 DOUT) plus PB4 (LATCH), PH2 (ENABLE) and PH1 (nFAULT), keeping the latch
// timing explicit for bring-up.
//
//*****************************************************************************

#ifndef __RELAY_CHAIN_H__
#define __RELAY_CHAIN_H__

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#ifdef __cplusplus
extern "C"
{
#endif

//
// One byte per cascaded device (8 relays each).
//
#define RELAY_MAX_DEVICES   CFG_RELAY_MAX_DEVICES
#define RELAY_MAX_BYTES     RELAY_MAX_DEVICES

//
// Configure the chain GPIO, set the device count, drive all relays off and
// enable the outputs.
//
void RelayChainInit(uint8_t ui8Devices);

//
// Change the cascaded device count at runtime (clamped to [0, RELAY_MAX_DEVICES]).
// Clears all relay state and re-latches.
//
void RelayChainSetDevices(uint8_t ui8Devices);
uint8_t RelayChainGetDevices(void);

//
// Total number of relays currently configured (device count * 8).
//
uint16_t RelayChainCount(void);

//
// Set / query a single relay (0-based).  RelayChainSet() re-shifts the whole
// chain and pulses LATCH.  Out-of-range indices are ignored.
//
void RelayChainSet(uint16_t ui16Relay, bool bOn);
bool RelayChainGet(uint16_t ui16Relay);

//
// Drive every relay off and re-latch.
//
void RelayChainAllOff(void);

//
// True while the DRV8860 nFAULT line is asserted (any device: overcurrent,
// over-temperature or open-load).
//
bool RelayChainFault(void);

#ifdef __cplusplus
}
#endif

#endif // __RELAY_CHAIN_H__
