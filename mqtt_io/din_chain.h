//*****************************************************************************
//
// din_chain.h - Driver for the cascaded SN65HVS882 digital-input chain.
//
// The control board's input boards carry cascaded SN65HVS882 serializers
// (8 inputs each).  They are read as one long shift register: pulse LD to latch
// all inputs, then clock (device_count * 8) bits out on SOP.  See HARDWARE.md
// sections 2.5 and 3 for the pin map and chain protocol.
//
// This first cut bit-bangs the interface on the SSI0 pins (PA2 CLK, PA5 SOP,
// PA3 CE) plus PH0 (LD), which keeps the load/latch timing explicit for
// bring-up.  It can be migrated to hardware SSI0 later if throughput demands.
//
//*****************************************************************************

#ifndef __DIN_CHAIN_H__
#define __DIN_CHAIN_H__

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#ifdef __cplusplus
extern "C"
{
#endif

//
// One byte per cascaded device.
//
#define DIN_MAX_DEVICES     CFG_DIN_MAX_DEVICES
#define DIN_MAX_BYTES       DIN_MAX_DEVICES

//
// Configure the chain GPIO and set the number of cascaded devices.
//
void DINChainInit(uint8_t ui8Devices);

//
// Change the cascaded device count at runtime (clamped to [0, DIN_MAX_DEVICES]).
//
void DINChainSetDevices(uint8_t ui8Devices);
uint8_t DINChainGetDevices(void);

//
// Total number of input channels currently configured (device count * 8).
//
uint16_t DINChainInputCount(void);

//
// Sample the whole chain.  Writes one byte per device into pui8Buf; bit 7 of
// byte d is the first bit clocked out of device d (physical channel mapping to
// be confirmed at bring-up - HARDWARE.md open question 2).  Returns the number
// of bytes written (== device count), or 0 if the buffer is too small or the
// device count is 0.
//
uint8_t DINChainRead(uint8_t *pui8Buf, uint8_t ui8BufLen);

#ifdef __cplusplus
}
#endif

#endif // __DIN_CHAIN_H__
