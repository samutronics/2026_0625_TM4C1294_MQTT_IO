//*****************************************************************************
//
// pal_irq.c (TM4C1294) - PAL critical section via TivaWare interrupt masking.
//
// Matches the semantics mqtt_client.c relied on before the seam existed:
// MAP_IntMasterDisable() masks interrupts and returns whether they were ALREADY
// masked, so the paired unlock re-enables only when this lock is the outermost.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include "driverlib/interrupt.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "pal_irq.h"

uintptr_t
PalIrqLock(void)
{
    //
    // Non-zero key => interrupts were already masked by an outer section.
    //
    return((uintptr_t)MAP_IntMasterDisable());
}

void
PalIrqUnlock(uintptr_t uiKey)
{
    if(uiKey == 0u)
    {
        MAP_IntMasterEnable();
    }
}
