//*****************************************************************************
//
// pal_str.c (CC35x1) - PAL string/format helpers backed by the ticlang libc.
//
// The CC35x1 image already links libc (the SDK and FreeRTOS use it), so unlike
// the TM4C - which forwards to TivaWare's lean ustdlib to keep libc's heavy
// printf out of the image - here the standard vsnprintf / strtoul are the right
// backend.  Behaviour matches the C99 contract the portable callers expect.
//
//*****************************************************************************

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "pal_str.h"

int
PalSnprintf(char *pcBuf, size_t szSize, const char *pcFormat, ...)
{
    va_list vaArgP;
    int     iRet;

    va_start(vaArgP, pcFormat);
    iRet = vsnprintf(pcBuf, szSize, pcFormat, vaArgP);
    va_end(vaArgP);
    return(iRet);
}

unsigned long
PalStrToUl(const char *pcStr, const char **ppcEnd, int iBase)
{
    //
    // strtoul's endptr is 'char **'; the PAL contract exposes 'const char **'
    // (callers only read through it).  The cast bridges the qualifier.
    //
    return(strtoul(pcStr, (char **)ppcEnd, iBase));
}
