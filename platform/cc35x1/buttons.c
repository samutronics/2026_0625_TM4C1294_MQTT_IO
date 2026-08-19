//*****************************************************************************
//
// buttons.c (CC35x1) - read the LP-EM-CC35X1 on-board user buttons (SW1, SW2).
//
// SW1 = GPIO2 (BoosterPack pin 19), SW2 = GPIO36 (BoosterPack pin 40).  Both are
// active-high: the board pulls the pin to 3.3V when pressed (LP-EM-CC35X1 board
// AGENTS.md, "Button Configurations"), so an internal pull-down makes a released
// button read 0 and a pressed button read 1.
//
// No SysConfig instance is needed for these pins.  On CC35xx the TI Drivers GPIO
// "index" IS the physical GPIO number (see ti_drivers_config.h: CONFIG_GPIO_*
// resolve to the GPIOn number), and the generated gpioPinConfigs[] array reserves
// a slot for every device pin (GPIO_NUMBER_OF_CONFIGS spans GPIO_pinLowerBound..
// GPIO_pinUpperBound = 0..37).  GPIO_setConfig()/GPIO_read() therefore configure
// and sample any in-range pin at runtime - the same mechanism pal_gpio.c already
// relies on - so the buttons work with an unmodified SysConfig configuration.
//
//*****************************************************************************

#include <stdint.h>

#include <ti/drivers/GPIO.h>

#include "buttons.h"

//
// Physical CC35xx GPIO numbers of the two on-board buttons (== TI Drivers GPIO
// index on this device).
//
#define BTN_SW1_PIN     2u
#define BTN_SW2_PIN     36u
#define BTN_COUNT       2

void
ButtonsInit(void)
{
    //
    // GPIO_init() readies the module and is safe to call more than once (also run
    // from Board_init()).  Configure both button pins as inputs with a pull-down.
    //
    GPIO_init();
    GPIO_setConfig(BTN_SW1_PIN, GPIO_CFG_IN_PD);
    GPIO_setConfig(BTN_SW2_PIN, GPIO_CFG_IN_PD);
}

int
ButtonsCount(void)
{
    return(BTN_COUNT);
}

uint8_t
ButtonsReadByte(void)
{
    uint8_t ui8Val = 0;

    if(GPIO_read(BTN_SW1_PIN) != 0) { ui8Val |= 0x01u; }
    if(GPIO_read(BTN_SW2_PIN) != 0) { ui8Val |= 0x02u; }
    return(ui8Val);
}
