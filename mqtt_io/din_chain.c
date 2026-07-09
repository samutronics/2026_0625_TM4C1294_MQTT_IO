//*****************************************************************************
//
// din_chain.c - Cascaded SN65HVS882 digital-input chain (bit-banged).
//
// Read sequence (all lines idle: CLK low, CE high, LD high):
//   1. LD low  -> latch the parallel input states into the shift registers.
//   2. LD high -> switch to shift mode; the first bit is presented on SOP.
//   3. CE low  -> enable the serial output.
//   4. For each of (devices * 8) bits: sample SOP, then pulse CLK to advance.
//   5. CE high -> disable.
//
// Devices cascade SOP -> SIP, so the bits arrive device-by-device: the first
// 8 bits are the device nearest the MCU, and so on down the chain.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/gpio.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"
#include "board_pins.h"
#include "din_chain.h"

//
// Half-clock / setup delay.  SysCtlDelay runs 3 cycles per count, so ~30 counts
// is well under a microsecond - comfortably within the SN65HVS882 timing while
// staying slow enough to tolerate the isolator and cabling.
//
#define DIN_DELAY()         MAP_SysCtlDelay(30)

//
// Number of cascaded SN65HVS882 devices currently configured.
//
static uint8_t g_ui8Devices;

//*****************************************************************************
//
// Idle the three control lines: CLK low, CE high (output disabled), LD high
// (shift mode).
//
//*****************************************************************************
static void
DINChainIdle(void)
{
    MAP_GPIOPinWrite(INPUT_SSI_CLK_PORT, INPUT_SSI_CLK_PIN, 0);
    MAP_GPIOPinWrite(INPUT_SSI_FSS_PORT, INPUT_SSI_FSS_PIN, INPUT_SSI_FSS_PIN);
    MAP_GPIOPinWrite(INPUT_LATCH_PORT, INPUT_LATCH_PIN, INPUT_LATCH_PIN);
}

//*****************************************************************************
//
// Configure the chain GPIO and set the device count.
//
//*****************************************************************************
void
DINChainInit(uint8_t ui8Devices)
{
    //
    // Enable the GPIO ports (idempotent - PA0/PA1 stay owned by UART0).
    //
    MAP_SysCtlPeripheralEnable(INPUT_SSI_GPIO_PERIPH);
    MAP_SysCtlPeripheralEnable(INPUT_LATCH_PERIPH);
    while(!MAP_SysCtlPeripheralReady(INPUT_SSI_GPIO_PERIPH))
    {
    }
    while(!MAP_SysCtlPeripheralReady(INPUT_LATCH_PERIPH))
    {
    }

    //
    // CLK, CE and LD are outputs; SOP is an input.
    //
    MAP_GPIOPinTypeGPIOOutput(INPUT_SSI_CLK_PORT, INPUT_SSI_CLK_PIN);
    MAP_GPIOPinTypeGPIOOutput(INPUT_SSI_FSS_PORT, INPUT_SSI_FSS_PIN);
    MAP_GPIOPinTypeGPIOOutput(INPUT_LATCH_PORT, INPUT_LATCH_PIN);
    MAP_GPIOPinTypeGPIOInput(INPUT_SSI_RX_PORT, INPUT_SSI_RX_PIN);

    DINChainIdle();
    DINChainSetDevices(ui8Devices);
}

//*****************************************************************************
//
// Change / query the cascaded device count.
//
//*****************************************************************************
void
DINChainSetDevices(uint8_t ui8Devices)
{
    g_ui8Devices = (ui8Devices > DIN_MAX_DEVICES) ? DIN_MAX_DEVICES
                                                  : ui8Devices;
}

uint8_t
DINChainGetDevices(void)
{
    return(g_ui8Devices);
}

uint16_t
DINChainInputCount(void)
{
    return((uint16_t)g_ui8Devices * 8u);
}

//*****************************************************************************
//
// Sample the whole chain.
//
//*****************************************************************************
uint8_t
DINChainRead(uint8_t *pui8Buf, uint8_t ui8BufLen)
{
    uint16_t ui16Bits, ui16I;

    if((g_ui8Devices == 0) || (ui8BufLen < g_ui8Devices))
    {
        return(0);
    }

    memset(pui8Buf, 0, g_ui8Devices);
    ui16Bits = (uint16_t)g_ui8Devices * 8u;

    //
    // Latch the parallel inputs, then enable the serial output.
    //
    MAP_GPIOPinWrite(INPUT_SSI_FSS_PORT, INPUT_SSI_FSS_PIN, INPUT_SSI_FSS_PIN);
    MAP_GPIOPinWrite(INPUT_LATCH_PORT, INPUT_LATCH_PIN, 0);          // LD low
    DIN_DELAY();
    MAP_GPIOPinWrite(INPUT_LATCH_PORT, INPUT_LATCH_PIN,
                     INPUT_LATCH_PIN);                               // LD high
    DIN_DELAY();
    MAP_GPIOPinWrite(INPUT_SSI_FSS_PORT, INPUT_SSI_FSS_PIN, 0);      // CE low
    DIN_DELAY();

    //
    // Sample-then-advance for every bit, MSB first into each byte.
    //
    for(ui16I = 0; ui16I < ui16Bits; ui16I++)
    {
        if(MAP_GPIOPinRead(INPUT_SSI_RX_PORT, INPUT_SSI_RX_PIN))
        {
            pui8Buf[ui16I >> 3] |= (uint8_t)(0x80u >> (ui16I & 7u));
        }

        MAP_GPIOPinWrite(INPUT_SSI_CLK_PORT, INPUT_SSI_CLK_PIN,
                         INPUT_SSI_CLK_PIN);                         // CLK high
        DIN_DELAY();
        MAP_GPIOPinWrite(INPUT_SSI_CLK_PORT, INPUT_SSI_CLK_PIN, 0);  // CLK low
        DIN_DELAY();
    }

    //
    // Disable the serial output again.
    //
    MAP_GPIOPinWrite(INPUT_SSI_FSS_PORT, INPUT_SSI_FSS_PIN, INPUT_SSI_FSS_PIN);

    return(g_ui8Devices);
}
