//*****************************************************************************
//
// pal_gpio.c (CC35x1) - PAL GPIO backed by the TI Drivers GPIO module.
//
// The DRV8860 / SN65HVS882 bit-bang protocol in din_chain.c / relay_chain.c is
// identical on every MCU; only these pin primitives differ.  The TI Drivers GPIO
// API is index-based (a flat SysConfig pin index), not base+mask like TivaWare,
// so the PAL's opaque (port, pin) pair carries the index in 'pin' and ignores
// 'port'.  board_pins.h for this platform therefore maps each chain signal to a
// CONFIG_GPIO_* index instead of a port base + bit mask.
//
// Pin direction/pull are normally fixed in SysConfig; the Config* calls below
// still assert them via GPIO_setConfig so the portable chain init is honoured on
// either target.  Write takes the TivaWare value convention (non-zero => high).
//
//*****************************************************************************

#include <stdint.h>
#include <ti/drivers/GPIO.h>
#include "pal_gpio.h"

//
// Bit-bang half-clock delay.  The TM4C uses MAP_SysCtlDelay(30) (~0.75 us at
// 120 MHz); there is no equivalent cycle-counted busy-wait in TI Drivers, so a
// short volatile NOP loop stands in.  The count is a placeholder to tune on the
// 160 MHz M33 against the SN65HVS882 / DRV8860 timing once the chains are wired
// (comfortably sub-microsecond either way for these low-rate devices).
//
#define PAL_GPIO_BIT_DELAY_LOOPS    40u

void
PalGpioEnablePort(uint32_t ui32PortId)
{
    //
    // TI Drivers has no per-port clock gate; GPIO_init() readies the whole
    // module and is safe to call more than once (also run from Board_init()).
    //
    (void)ui32PortId;
    GPIO_init();
}

void
PalGpioConfigOutput(uint32_t ui32Port, uint32_t ui32Pin)
{
    (void)ui32Port;
    GPIO_setConfig((uint_least8_t)ui32Pin, GPIO_CFG_OUTPUT | GPIO_CFG_OUT_LOW);
}

void
PalGpioConfigInput(uint32_t ui32Port, uint32_t ui32Pin)
{
    (void)ui32Port;
    GPIO_setConfig((uint_least8_t)ui32Pin, GPIO_CFG_INPUT);
}

void
PalGpioConfigInputPullup(uint32_t ui32Port, uint32_t ui32Pin)
{
    (void)ui32Port;
    GPIO_setConfig((uint_least8_t)ui32Pin, GPIO_CFG_IN_PU);
}

void
PalGpioWrite(uint32_t ui32Port, uint32_t ui32Pin, uint32_t ui32Val)
{
    (void)ui32Port;
    GPIO_write((uint_least8_t)ui32Pin, (ui32Val != 0u) ? 1u : 0u);
}

uint32_t
PalGpioRead(uint32_t ui32Port, uint32_t ui32Pin)
{
    (void)ui32Port;
    return((uint32_t)GPIO_read((uint_least8_t)ui32Pin));
}

void
PalGpioBitDelay(void)
{
    volatile uint32_t ui32Count = PAL_GPIO_BIT_DELAY_LOOPS;

    while(ui32Count--)
    {
    }
}
