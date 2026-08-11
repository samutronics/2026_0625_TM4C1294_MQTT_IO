//*****************************************************************************
//
// webui.h - shared config web UI: the SSI + CGI handlers for the field-I/O
//           gateway's web pages, portable across the TM4C (TivaWare lwIP 1.4.1)
//           and CC35x1 (SimpleLink SDK lwIP 2.1.3) builds.
//
// The handler bodies live in webui.c (moved out of the TM4C enet_io.c so both
// platforms serve identical pages from one source).  Each platform registers
// them with its httpd via WebUIRegister(), keeps g_ui32IPAddress + the live
// input snapshot current, polls the *Pending() request flags from its tick,
// and provides the OTA-chunk seam.
//
//*****************************************************************************

#ifndef WEBUI_H
#define WEBUI_H

#include <stdbool.h>
#include <stdint.h>
#include "din_chain.h"      // DIN_MAX_BYTES (size of the live input snapshot)

#ifdef __cplusplus
extern "C"
{
#endif

//
// Register the config web UI's SSI + CGI handlers with the lwIP httpd.  Call
// once, after httpd_init().  On NO_SYS=0 builds it must run with the tcpip core
// lock held (it mutates httpd globals).
//
void WebUIRegister(void);

//
// Live input-state snapshot the "instates" SSI tag renders (packed, LSB-first
// per byte).  Written by the platform input scan (DINChainScan); reads as zero
// until that scan runs.
//
extern uint8_t g_pui8LiveInState[DIN_MAX_BYTES];

//
// Current STA IPv4 address as a raw lwIP u32 (network order on little-endian),
// rendered by the "ipaddr" SSI tag.  Each platform keeps this current.
//
extern uint32_t g_ui32IPAddress;

//
// Web -> main-loop requests.  A CGI handler raises a request; the platform tick
// polls the matching *Pending() accessor, which returns and clears it.  The
// Request* setters let platform code raise the same request (e.g. the OTA
// handler asking for a reset, or DHCP-up asking for an MQTT (re)connect).
//
void WebUIRequestReset(void);
bool WebUIResetPending(void);
void WebUIRequestMqttApply(void);
bool WebUIMqttApplyPending(void);
void WebUIRequestMqttRepublish(void);
bool WebUIMqttRepublishPending(void);

//
// Platform-provided OTA chunk CGI (/fwchunk.cgi).  TM4C programs internal flash
// (its former FwChunkCGIHandler); the CC35x1 stub reports "unsupported" (OTA
// there is PSA FWU, out of scope for this slice).  Legacy signature kept so the
// existing TM4C body moves verbatim; webui.c casts it to tCGIHandler.
//
extern char *WebPlatformOtaChunkCGI(int32_t iIndex, int32_t i32NumParams,
                                    char *pcParam[], char *pcValue[]);

//
// Largest firmware image the platform will accept, rendered by the "otamax" SSI
// tag so the browser rejects an oversized upload up front.  TM4C returns its
// flash staging-region size; CC35x1 returns the PSA vendor-image slot size.
//
extern uint32_t WebPlatformOtaMaxBytes(void);

#ifdef __cplusplus
}
#endif

#endif // WEBUI_H
