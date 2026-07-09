//*****************************************************************************
//
// relay_chain.c - Cascaded DRV8860 relay-output chain (bit-banged).
//
// Write sequence (idle: CLK low, DIN low, LATCH low, ENABLE high):
//   1. For each of (devices * 8) bits: drive DIN, then pulse CLK high->low.
//      DRV8860 samples DIN on the rising CLK edge.
//   2. Pulse LATCH high->low to transfer the shift register to the outputs.
//
// Devices cascade DIN -> DOUT, so the first bits clocked in propagate to the
// device farthest from the MCU.  We therefore shift the highest-indexed device
// first and, within a byte, MSB first; state byte 0 ends up in the device
// nearest the MCU.  The absolute relay-index -> physical-output mapping is a
// bring-up detail (HARDWARE.md open question 3) - observe which relay clicks and
// adjust the shift order / bit order here if needed.
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
#include "relay_chain.h"

//
// Half-clock / setup delay (see din_chain.c for the rationale).
//
#define RELAY_DELAY()       MAP_SysCtlDelay(30)

//
// Number of cascaded DRV8860 devices and the desired relay state, one bit per
// relay (bit b of byte d = relay d*8 + b).
//
static uint8_t g_ui8Devices;
static uint8_t g_pui8State[RELAY_MAX_BYTES];

//*****************************************************************************
//
// Shift the current state into the chain and latch it to the outputs.
//
//*****************************************************************************
static void
RelayChainShift(void)
{
    int16_t i16Dev;
    uint8_t ui8Bit;

    //
    // Highest-indexed device first so state byte 0 lands nearest the MCU.
    //
    for(i16Dev = (int16_t)g_ui8Devices - 1; i16Dev >= 0; i16Dev--)
    {
        uint8_t ui8Byte = g_pui8State[i16Dev];

        for(ui8Bit = 0x80u; ui8Bit != 0u; ui8Bit >>= 1)
        {
            MAP_GPIOPinWrite(OUTPUT_SSI_TX_PORT, OUTPUT_SSI_TX_PIN,
                             (ui8Byte & ui8Bit) ? OUTPUT_SSI_TX_PIN : 0);
            RELAY_DELAY();
            MAP_GPIOPinWrite(OUTPUT_SSI_CLK_PORT, OUTPUT_SSI_CLK_PIN,
                             OUTPUT_SSI_CLK_PIN);                    // CLK high
            RELAY_DELAY();
            MAP_GPIOPinWrite(OUTPUT_SSI_CLK_PORT, OUTPUT_SSI_CLK_PIN, 0);
            RELAY_DELAY();
        }
    }

    //
    // Latch the shift register to the outputs.
    //
    MAP_GPIOPinWrite(OUTPUT_LATCH_PORT, OUTPUT_LATCH_PIN, OUTPUT_LATCH_PIN);
    RELAY_DELAY();
    MAP_GPIOPinWrite(OUTPUT_LATCH_PORT, OUTPUT_LATCH_PIN, 0);
    RELAY_DELAY();
}

//*****************************************************************************
//
// Configure the chain GPIO and set the device count.
//
//*****************************************************************************
void
RelayChainInit(uint8_t ui8Devices)
{
    //
    // Enable the GPIO ports used by the chain.
    //
    MAP_SysCtlPeripheralEnable(OUTPUT_SSI_CLK_PERIPH);      // GPIOB (CLK, LATCH)
    MAP_SysCtlPeripheralEnable(OUTPUT_SSI_E_PERIPH);        // GPIOE (DIN, DOUT)
    MAP_SysCtlPeripheralEnable(OUTPUT_ENABLE_PERIPH);       // GPIOH (EN, nFAULT)
    while(!MAP_SysCtlPeripheralReady(OUTPUT_SSI_CLK_PERIPH))
    {
    }
    while(!MAP_SysCtlPeripheralReady(OUTPUT_SSI_E_PERIPH))
    {
    }
    while(!MAP_SysCtlPeripheralReady(OUTPUT_ENABLE_PERIPH))
    {
    }

    //
    // CLK, DIN, LATCH and ENABLE are outputs; DOUT and nFAULT are inputs.
    //
    MAP_GPIOPinTypeGPIOOutput(OUTPUT_SSI_CLK_PORT, OUTPUT_SSI_CLK_PIN);
    MAP_GPIOPinTypeGPIOOutput(OUTPUT_SSI_TX_PORT, OUTPUT_SSI_TX_PIN);
    MAP_GPIOPinTypeGPIOOutput(OUTPUT_LATCH_PORT, OUTPUT_LATCH_PIN);
    MAP_GPIOPinTypeGPIOOutput(OUTPUT_ENABLE_PORT, OUTPUT_ENABLE_PIN);
    MAP_GPIOPinTypeGPIOInput(OUTPUT_SSI_RX_PORT, OUTPUT_SSI_RX_PIN);

    //
    // nFAULT is open-drain on the device; enable a pull-up so it reads high
    // when no fault is present.
    //
    MAP_GPIOPinTypeGPIOInput(OUTPUT_NFAULT_PORT, OUTPUT_NFAULT_PIN);
    MAP_GPIOPadConfigSet(OUTPUT_NFAULT_PORT, OUTPUT_NFAULT_PIN,
                         GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);

    //
    // Idle the control lines and disable outputs while we set up.
    //
    MAP_GPIOPinWrite(OUTPUT_SSI_CLK_PORT, OUTPUT_SSI_CLK_PIN, 0);
    MAP_GPIOPinWrite(OUTPUT_SSI_TX_PORT, OUTPUT_SSI_TX_PIN, 0);
    MAP_GPIOPinWrite(OUTPUT_LATCH_PORT, OUTPUT_LATCH_PIN, 0);
    MAP_GPIOPinWrite(OUTPUT_ENABLE_PORT, OUTPUT_ENABLE_PIN, 0);

    g_ui8Devices = (ui8Devices > RELAY_MAX_DEVICES) ? RELAY_MAX_DEVICES
                                                    : ui8Devices;
    memset(g_pui8State, 0, sizeof(g_pui8State));

    //
    // Shift a known all-off state, then enable the outputs so relays follow the
    // register from now on.
    //
    RelayChainShift();
    MAP_GPIOPinWrite(OUTPUT_ENABLE_PORT, OUTPUT_ENABLE_PIN, OUTPUT_ENABLE_PIN);
}

//*****************************************************************************
//
// Change / query the cascaded device count.
//
//*****************************************************************************
void
RelayChainSetDevices(uint8_t ui8Devices)
{
    g_ui8Devices = (ui8Devices > RELAY_MAX_DEVICES) ? RELAY_MAX_DEVICES
                                                    : ui8Devices;
    memset(g_pui8State, 0, sizeof(g_pui8State));
    RelayChainShift();
}

uint8_t
RelayChainGetDevices(void)
{
    return(g_ui8Devices);
}

uint16_t
RelayChainCount(void)
{
    return((uint16_t)g_ui8Devices * 8u);
}

//*****************************************************************************
//
// Set / query a single relay.
//
//*****************************************************************************
void
RelayChainSet(uint16_t ui16Relay, bool bOn)
{
    uint8_t ui8Mask;

    if(ui16Relay >= RelayChainCount())
    {
        return;
    }

    ui8Mask = (uint8_t)(1u << (ui16Relay & 7u));
    if(bOn)
    {
        g_pui8State[ui16Relay >> 3] |= ui8Mask;
    }
    else
    {
        g_pui8State[ui16Relay >> 3] &= (uint8_t)~ui8Mask;
    }

    RelayChainShift();
}

bool
RelayChainGet(uint16_t ui16Relay)
{
    if(ui16Relay >= RelayChainCount())
    {
        return(false);
    }
    return((g_pui8State[ui16Relay >> 3] & (1u << (ui16Relay & 7u))) != 0u);
}

void
RelayChainAllOff(void)
{
    memset(g_pui8State, 0, sizeof(g_pui8State));
    RelayChainShift();
}

//*****************************************************************************
//
// Read the wired-OR nFAULT line (active low).
//
//*****************************************************************************
bool
RelayChainFault(void)
{
    return(MAP_GPIOPinRead(OUTPUT_NFAULT_PORT, OUTPUT_NFAULT_PIN) == 0);
}
