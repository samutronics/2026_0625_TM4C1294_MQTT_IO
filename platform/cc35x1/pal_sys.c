//*****************************************************************************
//
// pal_sys.c (CC35x1) - PAL system control: MCU reset.
//
// PalReboot issues a Cortex-M33 system reset via the Application Interrupt and
// Reset Control Register (SCB->AIRCR) SYSRESETREQ - the same request CMSIS
// NVIC_SystemReset() makes, written directly here so this file carries no CMSIS
// device-header dependency.  This resets the M33; the Wi-Fi NWP re-initialises
// on the next connect, which is fine for a web-triggered config reboot.
//
//*****************************************************************************

#include <stdint.h>
#include "pal_sys.h"

//
// Cortex-M33 System Control Block - Application Interrupt and Reset Control.
//
#define SCB_AIRCR           (*(volatile uint32_t *)0xE000ED0Cu)
#define AIRCR_VECTKEY       (0x05FAu << 16)
#define AIRCR_SYSRESETREQ   (1u << 2)

void
PalReboot(void)
{
    //
    // Drain outstanding memory accesses, request a system reset, then spin
    // until it takes effect.
    //
    __asm volatile ("dsb sy" ::: "memory");
    SCB_AIRCR = AIRCR_VECTKEY | AIRCR_SYSRESETREQ;
    __asm volatile ("dsb sy" ::: "memory");

    for(;;)
    {
    }
}
