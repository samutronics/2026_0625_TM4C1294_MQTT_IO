//*****************************************************************************
//
// pal_str.h - Platform abstraction for the lean string/format helpers portable
//             code needs: a bounded snprintf and string-to-unsigned-long.
//
// On the TM4C these map to TivaWare's ustdlib — a compact, no-heap, no-float
// formatter — so portable code never drags libc's heavy printf into the image.
// On other platforms they map to libc (snprintf/strtoul).
//
//   PalSnprintf  specifier subset (TM4C/ustdlib): %c %d %i %u %x %X %s %p with
//                field width/padding.  NO floating point — keep formats within it.
//
//*****************************************************************************

#ifndef __PAL_STR_H__
#define __PAL_STR_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Bounded formatted print (like C99 snprintf): writes at most szSize-1 chars
// plus a NUL to pcBuf and returns the number of chars that would have been
// written.
//
int PalSnprintf(char *pcBuf, size_t szSize, const char *pcFormat, ...);

//
// Parse an unsigned long from pcStr in the given base (like C strtoul).  ppcEnd
// may be NULL.
//
unsigned long PalStrToUl(const char *pcStr, const char **ppcEnd, int iBase);

#ifdef __cplusplus
}
#endif

#endif // __PAL_STR_H__
