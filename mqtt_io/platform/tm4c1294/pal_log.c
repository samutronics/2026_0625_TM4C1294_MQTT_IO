//*****************************************************************************
//
// pal_log.c (TM4C1294) - PAL logging backed by the TivaWare uartstdio driver.
//
// Thin forward to UARTvprintf so format-specifier behavior is byte-for-byte the
// same as the previous direct UARTprintf calls.  UARTStdioConfig() is still done
// once at startup by the application (io.c), not here.
//
//*****************************************************************************

#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include "utils/uartstdio.h"
#include "pal_log.h"

void
PalLog(const char *pcFormat, ...)
{
    va_list vaArgP;

    va_start(vaArgP, pcFormat);
    UARTvprintf(pcFormat, vaArgP);
    va_end(vaArgP);
}
