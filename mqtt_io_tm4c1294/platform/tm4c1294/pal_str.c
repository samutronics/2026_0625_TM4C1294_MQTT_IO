//*****************************************************************************
//
// pal_str.c (TM4C1294) - PAL string/format helpers backed by TivaWare ustdlib.
//
// Forwards to ustdlib's lean formatter (uvsnprintf) and ustrtoul so behavior is
// identical to the previous direct usnprintf/ustrtoul calls, and the compact
// no-float printf stays in the image instead of libc's heavy one.
//
//*****************************************************************************

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include "utils/ustdlib.h"
#include "pal_str.h"

int
PalSnprintf(char *pcBuf, size_t szSize, const char *pcFormat, ...)
{
    va_list vaArgP;
    int     iRet;

    va_start(vaArgP, pcFormat);
    iRet = uvsnprintf(pcBuf, szSize, pcFormat, vaArgP);
    va_end(vaArgP);
    return(iRet);
}

unsigned long
PalStrToUl(const char *pcStr, const char **ppcEnd, int iBase)
{
    return(ustrtoul(pcStr, ppcEnd, iBase));
}
