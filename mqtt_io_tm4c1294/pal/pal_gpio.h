//*****************************************************************************
//
// pal_gpio.h - Platform abstraction for discrete GPIO pin control.
//
// The DRV8860 relay chain and SN65HVS882 input chain are bit-banged: the DEVICE
// protocol (shift order, latch/CE/LD sequencing) is identical on every MCU, only
// the pin primitives differ.  Portable chain drivers call these primitives; each
// platform maps them to its GPIO API.
//
//   Platform          Mapping
//   ----------------  ---------------------------------------------------------
//   TM4C1294          TivaWare MAP_GPIO* / MAP_SysCtl* (bit-banged)
//   CC35x1 (future)   TI Drivers GPIO (bit-banged), or a pal_spi HW-SPI fast path
//
// Pin identity is two opaque words (port, pin) supplied by the board's
// board_pins.h — on the TM4C these are a GPIO port base + pin mask, so the calls
// pass straight through.  A "port" for PalGpioEnablePort is the port's clock/
// enable id (SYSCTL_PERIPH_GPIOx on the TM4C).
//
//*****************************************************************************

#ifndef __PAL_GPIO_H__
#define __PAL_GPIO_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Enable a GPIO port and block until it is ready for use.
//
void PalGpioEnablePort(uint32_t ui32PortId);

//
// Configure one pin as a push-pull output / plain input / input with pull-up.
//
void PalGpioConfigOutput(uint32_t ui32Port, uint32_t ui32Pin);
void PalGpioConfigInput(uint32_t ui32Port, uint32_t ui32Pin);
void PalGpioConfigInputPullup(uint32_t ui32Port, uint32_t ui32Pin);

//
// Drive a pin.  ui32Val != 0 (conventionally the pin mask) drives high; 0 drives
// low — matching the TivaWare GPIOPinWrite value convention.
//
void PalGpioWrite(uint32_t ui32Port, uint32_t ui32Pin, uint32_t ui32Val);

//
// Read a pin.  Returns the masked level (0 = low, non-zero = high).
//
uint32_t PalGpioRead(uint32_t ui32Port, uint32_t ui32Pin);

//
// Short busy-wait for one bit-bang half-clock / setup interval.  Each platform
// tunes the duration to its core clock and the device's timing budget.
//
void PalGpioBitDelay(void);

#ifdef __cplusplus
}
#endif

#endif // __PAL_GPIO_H__
