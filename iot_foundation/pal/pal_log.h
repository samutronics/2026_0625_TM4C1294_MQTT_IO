//*****************************************************************************
//
// pal_log.h - Platform abstraction for diagnostic/console logging.
//
// PalLog is a printf-style logger.  Portable code (config.c, the common/
// services) calls it instead of a platform console API so the same sources
// build on every target.
//
//   Platform          Backing sink
//   ----------------  ---------------------------------------------------------
//   TM4C1294          UART0 backchannel via the TivaWare uartstdio driver
//   CC35x1 (future)   SDK UART / RTT / Display sink
//
// Format-specifier support matches the underlying backend.  On the TM4C that is
// TivaWare's uartstdio set (%c %d %u %x %s %p and field widths) — notably NO
// floating point.  Keep format strings within that common subset.
//
//*****************************************************************************

#ifndef __PAL_LOG_H__
#define __PAL_LOG_H__

#ifdef __cplusplus
extern "C"
{
#endif

//
// Emit a formatted log line.  printf-style; no implicit newline is added.
//
void PalLog(const char *pcFormat, ...);

#ifdef __cplusplus
}
#endif

#endif // __PAL_LOG_H__
