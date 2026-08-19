//*****************************************************************************
//
// buttons.h (CC35x1) - LP-EM-CC35X1 on-board user buttons (SW1, SW2) as a
// packed input byte, so they can be fed into the shared field-I/O scan the same
// way as the SN65HVS882 SPI inputs.
//
// SW1 = GPIO2, SW2 = GPIO36.  These are exposed to the portable io_scan layer
// through the WebPlatformLocalInput* seam (webui.h); see buttons.c for why no
// SysConfig instance is required.
//
//*****************************************************************************

#ifndef __BUTTONS_H__
#define __BUTTONS_H__

#include <stdint.h>

//
// Configure the two button GPIOs as inputs with a pull-down (idempotent).
//
void ButtonsInit(void);

//
// Number of on-board buttons exposed as inputs (2).
//
int ButtonsCount(void);

//
// Sample the buttons into a packed byte: bit0 = SW1, bit1 = SW2, 1 = pressed
// (active-high).  Only bits 0..1 are used.
//
uint8_t ButtonsReadByte(void);

#endif // __BUTTONS_H__
