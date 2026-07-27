//*****************************************************************************
//
// pal_irq.c (CC35x1) - PAL critical section via the TI DPL HwiP layer.
//
// HwiP_disable() masks interrupts and returns a key capturing the prior mask
// state; HwiP_restore(key) puts it back.  On the single-core Cortex-M33 this is
// a PRIMASK mask, so it also holds off the FreeRTOS scheduler (and therefore the
// lwIP tcpip_thread) for the short bracket the MQTT client needs - the same
// atomicity the TM4C build gets from masking the Ethernet/SysTick interrupts.
//
// NOTE (deferred, threading correctness): under NO_SYS=0 the lwIP raw TCP API
// is meant to be entered on the tcpip_thread.  Masking interrupts makes the
// call sequence atomic but does not by itself satisfy lwIP's thread model for
// longer operations; hardening the raw-API entry points to marshal onto
// tcpip_thread (tcpip_callback / core locking) is a later step.  This seam is
// correct for the build+link goal and adequate for single-core bring-up.
//
//*****************************************************************************

#include <stdint.h>
#include <ti/drivers/dpl/HwiP.h>
#include "pal_irq.h"

uintptr_t
PalIrqLock(void)
{
    return(HwiP_disable());
}

void
PalIrqUnlock(uintptr_t uiKey)
{
    HwiP_restore(uiKey);
}
