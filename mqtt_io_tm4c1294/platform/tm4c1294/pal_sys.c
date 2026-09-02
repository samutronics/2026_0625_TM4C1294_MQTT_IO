//*****************************************************************************
//
// pal_sys.c (TM4C1294) - PAL system control: MCU reset.
//
// PalReboot performs a full software reset via TivaWare's MAP_SysCtlReset.
// Used by the web UI reboot / factory-reset flow (WebUIResetPending()).
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_types.h"
#include "driverlib/sysctl.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "pal_sys.h"

void
PalReboot(void)
{
    MAP_SysCtlReset();

    //
    // MAP_SysCtlReset does not return; spin as a belt-and-braces guard.
    //
    for(;;)
    {
    }
}
