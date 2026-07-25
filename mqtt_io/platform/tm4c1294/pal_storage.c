//*****************************************************************************
//
// pal_storage.c (TM4C1294) - PAL storage backed by the on-chip EEPROM.
//
// Thin pass-through over the TivaWare EEPROM block API.  This is the ONLY file
// that talks to driverlib/eeprom.h; config.c is now platform-independent and
// reaches persistent storage exclusively through pal_storage.h.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_types.h"
#include "driverlib/eeprom.h"
#include "driverlib/sysctl.h"
#include "pal_storage.h"

//
// On-chip EEPROM size on the TM4C1294NCPDT (6 KB).
//
#define PAL_STORAGE_SIZE    6144u

//*****************************************************************************
//
// Enable and initialise the EEPROM peripheral.
//
//*****************************************************************************
bool
PalStorageInit(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_EEPROM0);
    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_EEPROM0))
    {
    }
    return(EEPROMInit() == EEPROM_INIT_OK);
}

uint32_t
PalStorageSize(void)
{
    return(PAL_STORAGE_SIZE);
}

//*****************************************************************************
//
// EEPROMRead has a void return and cannot fail at the API level, so always
// report success.
//
//*****************************************************************************
uint32_t
PalStorageRead(void *pvBuf, uint32_t ui32Addr, uint32_t ui32Len)
{
    EEPROMRead((uint32_t *)pvBuf, ui32Addr, ui32Len);
    return(0);
}

//*****************************************************************************
//
// EEPROMProgram returns 0 on success or a non-zero fault code; pass it straight
// through.  The source pointer is not modified, so casting away const is safe.
//
//*****************************************************************************
uint32_t
PalStorageWrite(const void *pvBuf, uint32_t ui32Addr, uint32_t ui32Len)
{
    return(EEPROMProgram((uint32_t *)(uintptr_t)pvBuf, ui32Addr, ui32Len));
}
