//*****************************************************************************
//
// pal_gpio.c (TM4C1294) - PAL GPIO backed by TivaWare driverlib.
//
// Thin pass-through over MAP_GPIO* / MAP_SysCtl*.  The bit-bang half-clock delay
// (30 SysCtlDelay counts = ~0.75 us at 120 MHz, 3 cycles/count) lived in the
// chain drivers as a magic number; it now belongs here so portable code carries
// no clock-specific constant.
//
//*****************************************************************************

#include <stdint.h>
#include <stdbool.h>
#include "inc/hw_types.h"
#include "driverlib/gpio.h"
#include "driverlib/sysctl.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "pal_gpio.h"

//
// Bit-bang half-clock delay count (see din_chain.c / relay_chain.c rationale):
// well under a microsecond, comfortably within the SN65HVS882 / DRV8860 timing.
//
#define PAL_GPIO_BIT_DELAY_COUNT    30u

void
PalGpioEnablePort(uint32_t ui32PortId)
{
    MAP_SysCtlPeripheralEnable(ui32PortId);
    while(!MAP_SysCtlPeripheralReady(ui32PortId))
    {
    }
}

void
PalGpioConfigOutput(uint32_t ui32Port, uint32_t ui32Pin)
{
    MAP_GPIOPinTypeGPIOOutput(ui32Port, ui32Pin);
}

void
PalGpioConfigInput(uint32_t ui32Port, uint32_t ui32Pin)
{
    MAP_GPIOPinTypeGPIOInput(ui32Port, ui32Pin);
}

void
PalGpioConfigInputPullup(uint32_t ui32Port, uint32_t ui32Pin)
{
    MAP_GPIOPinTypeGPIOInput(ui32Port, ui32Pin);
    MAP_GPIOPadConfigSet(ui32Port, ui32Pin,
                         GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
}

void
PalGpioWrite(uint32_t ui32Port, uint32_t ui32Pin, uint32_t ui32Val)
{
    MAP_GPIOPinWrite(ui32Port, ui32Pin, ui32Val);
}

uint32_t
PalGpioRead(uint32_t ui32Port, uint32_t ui32Pin)
{
    return((uint32_t)MAP_GPIOPinRead(ui32Port, ui32Pin));
}

void
PalGpioBitDelay(void)
{
    MAP_SysCtlDelay(PAL_GPIO_BIT_DELAY_COUNT);
}
