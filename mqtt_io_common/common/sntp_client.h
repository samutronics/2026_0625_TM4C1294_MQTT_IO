//*****************************************************************************
//
// sntp_client.h - Minimal SNTP (RFC 4330) client over raw lwIP UDP.
//
// Call SntpInit() once after the network interface acquires an IP address.
// Call SntpTick() every 10 ms from the main loop.
// Query the current time with SntpGetTimeStr() for SSI tag insertion.
//
//*****************************************************************************

#ifndef __SNTP_CLIENT_H__
#define __SNTP_CLIENT_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Initialise / reinitialise the SNTP client using the server and TZ offset
// from ConfigNtpGet().  Starts a DNS lookup; first sync may take a few
// seconds.  Safe to call again after a settings change.
//
void SntpInit(void);

//
// Advance internal timers.  Call from the main loop with elapsed milliseconds
// (typically SYSTICKMS = 10).
//
void SntpTick(uint32_t ui32ElapsedMs);

//
// Returns true once the first NTP sync has succeeded.
//
bool SntpIsSynced(void);

//
// Write "HH:MM:SS" (9 bytes including NUL) into pcBuf, applying the
// configured TZ offset.  Writes "--:--:--" when not synced.
//
void SntpGetTimeStr(char *pcBuf, int iBufLen);

#ifdef __cplusplus
}
#endif

#endif // __SNTP_CLIENT_H__
