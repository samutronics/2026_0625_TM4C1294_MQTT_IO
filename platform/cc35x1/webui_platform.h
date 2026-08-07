//*****************************************************************************
//
// webui_platform.h (CC35x1) - OTA lifecycle seams the app tick (main.c) drives.
//
// The web-UI OTA-chunk CGI (WebPlatformOtaChunkCGI, declared in the shared
// webui.h) stages an uploaded image via PSA FWU.  These extra seams keep all
// psa_fwu usage inside webui_platform.c: main.c only calls the entry points
// below, so it never includes the SDK FWU headers.
//
//   WebPlatformOtaInit        - once at boot: psa_fwu_init() + cache staging cap.
//   WebPlatformOtaTrialAccept - once the board is healthy (Wi-Fi up): commit a
//                               TRIAL image, else no-op.
//   WebPlatformFinalizeReboot - the web-reset seam: PSA swap-reboot after an OTA
//                               stage, otherwise a plain PalReboot().
//
// (WebPlatformOtaMaxBytes, rendered by the shared "otamax" SSI tag, is declared
// in webui.h since the shared SSIHandler calls it on both platforms.)
//
//*****************************************************************************

#ifndef WEBUI_PLATFORM_H
#define WEBUI_PLATFORM_H

#ifdef __cplusplus
extern "C"
{
#endif

void WebPlatformOtaInit(void);
void WebPlatformOtaTrialAccept(void);
void WebPlatformFinalizeReboot(void);

#ifdef __cplusplus
}
#endif

#endif // WEBUI_PLATFORM_H
