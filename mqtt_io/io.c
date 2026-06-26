//*****************************************************************************
//
// io.c - I/O routines for the enet_io example application.
//
// Copyright (c) 2013-2020 Texas Instruments Incorporated.  All rights reserved.
// Software License Agreement
// 
// Texas Instruments (TI) is supplying this software for use solely and
// exclusively on TI's microcontroller products. The software is owned by
// TI and/or its suppliers, and is protected under applicable copyright
// laws. You may not combine this software with "viral" open-source
// software in order to form a larger program.
// 
// THIS SOFTWARE IS PROVIDED "AS IS" AND WITH ALL FAULTS.
// NO WARRANTIES, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING, BUT
// NOT LIMITED TO, IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE APPLY TO THIS SOFTWARE. TI SHALL NOT, UNDER ANY
// CIRCUMSTANCES, BE LIABLE FOR SPECIAL, INCIDENTAL, OR CONSEQUENTIAL
// DAMAGES, FOR ANY REASON WHATSOEVER.
// 
// This is part of revision 2.2.0.295 of the EK-TM4C1294XL Firmware Package.
//
//*****************************************************************************
#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_pwm.h"
#include "inc/hw_types.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pwm.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/timer.h"
#include "utils/ustdlib.h"
#include "io.h"

//*****************************************************************************
//
// Hardware connection for the user LED.
//
//*****************************************************************************
#define LED_PORT_BASE GPIO_PORTN_BASE
#define LED_PIN GPIO_PIN_0

//*****************************************************************************
//
// Hardware connection for the animation LED.
//
//*****************************************************************************
#define LED_ANIM_PORT_BASE GPIO_PORTN_BASE
#define LED_ANIM_PIN GPIO_PIN_1

//*****************************************************************************
//
// The system clock speed.
//
//*****************************************************************************
extern uint32_t g_ui32SysClock;

//*****************************************************************************
//
// The current speed of the on-screen animation expressed as a percentage.
//
//*****************************************************************************
volatile unsigned long g_ulAnimSpeed = 10;

//*****************************************************************************
//
// Set the timer used to pace the animation.  We scale the timer timeout such
// that a speed of 100% causes the timer to tick once every 20 mS (50Hz).
//
//*****************************************************************************
static void
io_set_timer(unsigned long ulSpeedPercent)
{
    unsigned long ulTimeout;

    //
    // Turn the timer off while we are mucking with it.
    //
    MAP_TimerDisable(TIMER2_BASE, TIMER_A);

    //
    // If the speed is non-zero, we reset the timeout.  If it is zero, we
    // just leave the timer disabled.
    //
    if(ulSpeedPercent)
    {
        //
        // Set Timeout
        //
        ulTimeout = g_ui32SysClock / 50;
        ulTimeout = (ulTimeout * 100 ) / ulSpeedPercent;

        MAP_TimerLoadSet(TIMER2_BASE, TIMER_A, ulTimeout);
        MAP_TimerEnable(TIMER2_BASE, TIMER_A);
    }
}

//*****************************************************************************
//
// Initialize the IO used in this demo
//
//*****************************************************************************
void
io_init(void)
{
    //
    // Configure Port N0 for as an output for the status LED.
    //
    MAP_GPIOPinTypeGPIOOutput(LED_PORT_BASE, LED_PIN);

    //
    // Configure Port N0 for as an output for the animation LED.
    //
    MAP_GPIOPinTypeGPIOOutput(LED_ANIM_PORT_BASE, LED_ANIM_PIN);

    //
    // Initialize LED to OFF (0)
    //
    MAP_GPIOPinWrite(LED_PORT_BASE, LED_PIN, 0);

    //
    // Initialize animation LED to OFF (0)
    //
    MAP_GPIOPinWrite(LED_ANIM_PORT_BASE, LED_ANIM_PIN, 0);

    //
    // Enable the peripherals used by this example.
    //
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER2);

    //
    // Configure the timer used to pace the animation.
    //
    MAP_TimerConfigure(TIMER2_BASE, TIMER_CFG_PERIODIC);

    //
    // Setup the interrupts for the timer timeouts.
    //
    MAP_IntEnable(INT_TIMER2A);
    MAP_TimerIntEnable(TIMER2_BASE, TIMER_TIMA_TIMEOUT);

    //
    // Set the timer for the current animation speed.  This enables the
    // timer as a side effect.
    //
    io_set_timer(g_ulAnimSpeed);
}

//*****************************************************************************
//
// Set the status LED on or off.
//
//*****************************************************************************
void
io_set_led(bool bOn)
{
    //
    // Turn the LED on or off as requested.
    //
    MAP_GPIOPinWrite(LED_PORT_BASE, LED_PIN, bOn ? LED_PIN : 0);
}

//*****************************************************************************
//
// Return LED state
//
//*****************************************************************************
void
io_get_ledstate(char * pcBuf, int iBufLen)
{
    //
    // Get the state of the LED
    //
    if(MAP_GPIOPinRead(LED_PORT_BASE, LED_PIN))
    {
        usnprintf(pcBuf, iBufLen, "ON");
    }
    else
    {
        usnprintf(pcBuf, iBufLen, "OFF");
    }

}

//*****************************************************************************
//
// Return LED state as an integer, 1 on, 0 off.
//
//*****************************************************************************
int
io_is_led_on(void)
{
    //
    // Get the state of the LED
    //
    if(MAP_GPIOPinRead(LED_PORT_BASE, LED_PIN))
    {
        return(true);
    }
    else
    {
        return(0);
    }
}

//*****************************************************************************
//
// Set the speed of the animation shown on the display.  In this version, the
// speed is described as a decimal number encoded as an ASCII string.
//
//*****************************************************************************
void
io_set_animation_speed_string(char *pcBuf)
{
    unsigned long ulSpeed;

    //
    // Parse the passed parameter as a decimal number.
    //
    ulSpeed = 0;
    while((*pcBuf >= '0') && (*pcBuf <= '9'))
    {
        ulSpeed *= 10;
        ulSpeed += (*pcBuf - '0');
        pcBuf++;
    }

    //
    // If the number is valid, set the new speed.
    //
    if(ulSpeed <= 100)
    {
        g_ulAnimSpeed = ulSpeed;
        io_set_timer(g_ulAnimSpeed);
    }
}

//*****************************************************************************
//
// Set the speed of the animation shown on the display.
//
//*****************************************************************************
void
io_set_animation_speed(unsigned long ulSpeed)
{
    //
    // If the number is valid, set the new speed.
    //
    if(ulSpeed <= 100)
    {
        g_ulAnimSpeed = ulSpeed;
        io_set_timer(g_ulAnimSpeed);
    }
}

//*****************************************************************************
//
// Get the current animation speed as an ASCII string.
//
//*****************************************************************************
void
io_get_animation_speed_string(char *pcBuf, int iBufLen)
{
    usnprintf(pcBuf, iBufLen, "%d%%", g_ulAnimSpeed);
}

//*****************************************************************************
//
// Get the current animation speed as a number.
//
//*****************************************************************************
unsigned long
io_get_animation_speed(void)
{
    return(g_ulAnimSpeed);
}

//*****************************************************************************
//
// MQTT IO additions: status LEDs and user pushbuttons.
//
// Status LEDs: D2 (PN0) = network/IP, D1 (PN1) = MQTT connection.  These pins
// are already configured as outputs by io_init().
//
// Pushbuttons: SW1 = PJ0, SW2 = PJ1 (active low, internal pull-ups).  GPIOJ is
// enabled by PinoutSet().
//
//*****************************************************************************
#define SW_PORT_BASE        GPIO_PORTJ_BASE
#define SW1_PIN             GPIO_PIN_0
#define SW2_PIN             GPIO_PIN_1
#define NET_LED_PORT_BASE   GPIO_PORTN_BASE
#define NET_LED_PIN         GPIO_PIN_0
#define MQTT_LED_PORT_BASE  GPIO_PORTN_BASE
#define MQTT_LED_PIN        GPIO_PIN_1

//
// Debounce state for the two buttons.  A transition must persist for
// DEBOUNCE_POLLS consecutive polls before it is reported.
//
#define DEBOUNCE_POLLS      3
static uint8_t g_pui8BtnState[2];   // Debounced state: 1 = pressed.
static uint8_t g_pui8BtnCount[2];   // Consecutive opposite-state polls.

void
io_buttons_init(void)
{
    //
    // Configure the two switch pins as inputs with weak pull-ups.
    //
    MAP_GPIOPinTypeGPIOInput(SW_PORT_BASE, SW1_PIN | SW2_PIN);
    MAP_GPIOPadConfigSet(SW_PORT_BASE, SW1_PIN | SW2_PIN, GPIO_STRENGTH_2MA,
                         GPIO_PIN_TYPE_STD_WPU);

    g_pui8BtnState[0] = 0;
    g_pui8BtnState[1] = 0;
    g_pui8BtnCount[0] = 0;
    g_pui8BtnCount[1] = 0;
}

void
io_poll_buttons(void (*pfnEvent)(int iButton, bool bPressed))
{
    static const uint32_t pui32Pins[2] = { SW1_PIN, SW2_PIN };
    uint32_t ui32Raw;
    int i;

    ui32Raw = MAP_GPIOPinRead(SW_PORT_BASE, SW1_PIN | SW2_PIN);

    for(i = 0; i < 2; i++)
    {
        //
        // Active low: a pin reading 0 means the button is pressed.
        //
        uint8_t ui8Pressed = (ui32Raw & pui32Pins[i]) ? 0 : 1;

        if(ui8Pressed != g_pui8BtnState[i])
        {
            if(++g_pui8BtnCount[i] >= DEBOUNCE_POLLS)
            {
                g_pui8BtnState[i] = ui8Pressed;
                g_pui8BtnCount[i] = 0;
                if(pfnEvent)
                {
                    pfnEvent(i + 1, ui8Pressed ? true : false);
                }
            }
        }
        else
        {
            g_pui8BtnCount[i] = 0;
        }
    }
}

void
io_set_net_led(bool bOn)
{
    MAP_GPIOPinWrite(NET_LED_PORT_BASE, NET_LED_PIN, bOn ? NET_LED_PIN : 0);
}

void
io_set_mqtt_led(bool bOn)
{
    MAP_GPIOPinWrite(MQTT_LED_PORT_BASE, MQTT_LED_PIN, bOn ? MQTT_LED_PIN : 0);
}

//*****************************************************************************
//
// User-controllable LEDs (exposed via MQTT / Home Assistant).
// iLed 1 -> D1 (PN1), iLed 2 -> D2 (PN0).
//
//*****************************************************************************
static bool g_pbUserLed[2];

void
io_set_user_led(int iLed, bool bOn)
{
    uint32_t ui32Base = (iLed == 2) ? NET_LED_PORT_BASE : MQTT_LED_PORT_BASE;
    uint32_t ui32Pin = (iLed == 2) ? NET_LED_PIN : MQTT_LED_PIN;

    MAP_GPIOPinWrite(ui32Base, ui32Pin, bOn ? ui32Pin : 0);
    g_pbUserLed[(iLed == 2) ? 1 : 0] = bOn;
}

bool
io_get_user_led(int iLed)
{
    return(g_pbUserLed[(iLed == 2) ? 1 : 0]);
}
