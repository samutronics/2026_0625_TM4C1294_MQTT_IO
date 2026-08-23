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
extern uint8_t g_pui8LiveInState[IO_MAX_BYTES];

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
// Wi-Fi provisioning (CC35x1 SoftAP setup flow).  The /wificfg.cgi and
// /wififorget.cgi handlers raise these; the CC35x1 tick polls the *Pending()
// accessors to persist the entered credentials and switch Wi-Fi role live.  The
// provision accessor copies the pending SSID/passphrase into the caller's
// buffers and clears the request.  No-ops on the TM4C build (wired Ethernet).
//
void WebUIRequestWifiProvision(const char *pcSsid, const char *pcPass);
bool WebUIWifiProvisionPending(char *pcSsid, int iSsidLen,
                               char *pcPass, int iPassLen);
void WebUIRequestWifiForget(void);
bool WebUIWifiForgetPending(void);

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

//
// Upload transport the Tools page should use, rendered by the "otapost" SSI tag:
// 1 = stream the image as a single binary POST to /fwupload (CC35x1 fast path),
// 0 = legacy hex-encoded GET chunk loop (TM4C internal-flash programming).
//
extern uint32_t WebPlatformOtaUsePost(void);

//
// Platform-local digital inputs (beyond the SN65HVS882 SPI chain), appended to
// the input index space immediately after the SPI inputs by the io_scan
// aggregation layer (IOInputCount / IOInputReadAll).  On the CC35x1 these are the
// two on-board LaunchPad buttons (SW1, SW2); the TM4C build has none (wired
// Ethernet field-I/O only) and returns 0.  WebPlatformLocalInputRead() packs the
// local inputs LSB-first into one byte (bit0 = first local input), matching the
// per-byte bit order of the SPI live-state matrix.
//
extern int     WebPlatformLocalInputCount(void);
extern uint8_t WebPlatformLocalInputRead(void);

//
// On-board temperature sensor seam.  WebPlatformHasTempSensor() is non-zero on
// platforms with a sensor (the CC35x1's TMP1075); the TM4C has none and returns
// 0, which suppresses the MQTT temperature entity and the web status item.
// WebPlatformTempStr() renders the current reading for the "temp" SSI tag as a
// short HTML string (e.g. "23.4 &deg;C", or "&mdash;" / "n/a" when unavailable);
// it always NUL-terminates.
//
extern int  WebPlatformHasTempSensor(void);
extern void WebPlatformTempStr(char *pcInsert, int iInsertLen);

//
// Render the setup page's SSID dropdown, emitted by the "wifiopts" SSI tag as a
// run of <option> elements (already HTML-escaped, truncated to iInsertLen).  The
// CC35x1 build fills it from its cached Wi-Fi scan; the TM4C build (wired
// Ethernet, no scan) writes an empty string.  Always NUL-terminates pcInsert.
//
extern void WebPlatformWifiScanOptions(char *pcInsert, int iInsertLen);

//
// Render the Settings-page sub-tab bar, emitted by the "wifitab" SSI tag in
// index.shtml.  The CC35x1 (Wi-Fi) build returns the MQTT/Wi-Fi sub-tab buttons
// so the Wi-Fi provisioning pane is reachable; the TM4C build (wired Ethernet)
// writes an empty string, so its Settings page keeps a single (untabbed) MQTT
// pane.  Always NUL-terminates.
//
extern void WebPlatformWifiTab(char *pcInsert, int iInsertLen);

#ifdef __cplusplus
}
#endif

#endif // WEBUI_H
