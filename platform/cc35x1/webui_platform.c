//*****************************************************************************
//
// webui_platform.c (CC35x1) - platform seams for the shared web UI (webui.c).
//
// The shared CGI table routes /fwchunk.cgi to WebPlatformOtaChunkCGI.  On the
// TM4C that programs internal flash; the CC35x1 has no such in-place OTA path
// (firmware update there is PSA FWU, out of scope for this slice), so the stub
// reports "unsupported" and bounces the browser back to the tools page.
//
//*****************************************************************************

#include <stdint.h>
#include "pal_log.h"
#include "webui.h"

char *
WebPlatformOtaChunkCGI(int32_t iIndex, int32_t i32NumParams,
                       char *pcParam[], char *pcValue[])
{
    (void)iIndex;
    (void)i32NumParams;
    (void)pcParam;
    (void)pcValue;

    PalLog("web: OTA over /fwchunk.cgi is not supported on CC35x1 "
           "(PSA FWU deferred)\n");
    return("/tools.shtml");
}
