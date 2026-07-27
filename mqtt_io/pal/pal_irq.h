//*****************************************************************************
//
// pal_irq.h - Platform abstraction for a short critical section.
//
// mqtt_client.c drives the lwIP raw TCP API from the application context while
// lwIP's own processing runs elsewhere (an Ethernet/SysTick interrupt on the
// bare-metal TM4C build; the tcpip_thread on the RTOS CC35x1 build).  A brief
// critical section makes a sequence of raw-API calls atomic with respect to
// that processing.  The mechanism differs per platform, so the portable client
// brackets those sequences with these primitives instead of a native API.
//
//   Platform          Backing mechanism
//   ----------------  ---------------------------------------------------------
//   TM4C1294          TivaWare MAP_IntMasterDisable / MAP_IntMasterEnable
//   CC35x1            TI DPL HwiP_disable / HwiP_restore
//
// Both are single-core PRIMASK-style masks, so masking interrupts also prevents
// the RTOS from scheduling the tcpip_thread for the (short) duration - which is
// what makes the raw-API sequence atomic on the CC35x1 too.  Lock/unlock pairs
// nest: the returned key records the prior mask state and unlock restores it, so
// an inner pair never re-enables interrupts inside an outer section.
//
//*****************************************************************************

#ifndef __PAL_IRQ_H__
#define __PAL_IRQ_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Enter a critical section.  Returns an opaque key describing the prior state,
// to be handed back to PalIrqUnlock.
//
uintptr_t PalIrqLock(void);

//
// Leave a critical section, restoring the state captured by PalIrqLock.
//
void PalIrqUnlock(uintptr_t uiKey);

#ifdef __cplusplus
}
#endif

#endif // __PAL_IRQ_H__
