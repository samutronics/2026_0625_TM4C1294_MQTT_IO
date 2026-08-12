//*****************************************************************************
//
// webui_platform.c (CC35x1) - platform seams for the shared web UI (webui.c).
//
// Implements browser-push OTA on top of the SDK PSA Firmware Update (FWU)
// library.  The Tools page (fs/tools.shtml) uploads a signed vendor-image .bin
// in hex-encoded GET chunks over /fwchunk.cgi -- the SAME wire protocol the TM4C
// flash-programming handler uses (seq / last / data, idempotent retry) -- and
// this handler stages those bytes into the inactive A/B vendor-image slot via
// PSA FWU, installs it, and asks the bootloader to reboot into it in TRIAL mode.
//
// The uploaded file is the [ detached-manifest | signed image ] blob produced by
// the toolbox (the same primary_vendor_image.sign.bin flash.sh signs): its first
// TI_FWU_MANIFEST_SIZE bytes are the detached manifest fed to psa_fwu_start();
// the remainder streams to psa_fwu_write() at its absolute file offset.
//
// Rollback safety (the reason a DIN-rail box can be flashed blind): a staged
// image boots in TRIAL and is only committed once main.c confirms a healthy boot
// (Wi-Fi up) and calls WebPlatformOtaTrialAccept().  A firmware that cannot come
// up never accepts, so the bootloader reverts to the previous slot on the next
// power-cycle.
//
// All PSA FWU usage is confined to this file; main.c stays free of psa_fwu
// headers and drives OTA only through the seams declared in webui_platform.h.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "pal_log.h"
#include "pal_sys.h"
#include "webui.h"
#include "webui_platform.h"

#include "ti/utils/FWU/psa_fwu.h"   /* also pulls in fwu/1.0/psa/update.h + error.h */

#include "lwip/apps/httpd.h"         /* LWIP_HTTPD_SUPPORT_POST + POST callback prototypes */
#include "lwip/pbuf.h"               /* struct pbuf, pbuf_free (streaming POST body) */


//
// The two vendor-image A/B components (PSA_FWU_Component_ID_e).  We only ever
// OTA the application (vendor image); BL2 and wireless firmware are out of scope.
//
#define OTA_VENDOR_SLOT_1   ((psa_fwu_component_t)Vendor_Image_Slot_1)  /* 4 */
#define OTA_VENDOR_SLOT_2   ((psa_fwu_component_t)Vendor_Image_Slot_2)  /* 5 */

//
// Fallback staging cap (bytes) if the runtime max_size query is unavailable:
// the SysConfig MemoryConfigurator vendor-app slot size (2688 KB on the 8 MB
// IS25WJ064F).  Rendered by the "otamax" SSI tag so the browser rejects an
// oversized image up front.
//
#define OTA_MAX_BYTES_FALLBACK  (2688u * 1024u)

//
// Max binary bytes decoded from one GET chunk.  tools.shtml sends FWCHUNK=448
// binary bytes (896 hex chars); size the decode buffer with margin.
//
#define OTA_CHUNK_MAX   512u

//
// Image bytes are coalesced into flash-sector-sized blocks before being handed
// to psa_fwu_write().  The external flash sector is 4 KB (SysConfig
// MemoryConfigurator), and the FWU driver programs a whole sector per write --
// so writing the raw 448-byte GET chunks means ~9 read-modify-write cycles over
// the SAME sector, which is both slow (~1 s each -> a 1 MB image takes tens of
// minutes) and stall-prone.  Buffering to aligned 4 KB blocks makes it one
// program per sector: an order of magnitude fewer, faster, steadier flash ops.
//
#define OTA_WRITE_BLOCK 4096u

//
// Upload state machine.  Written from the CGI handler, which runs on the lwIP
// tcpip_thread; the upload is serialised (one chunk in flight) so no locking is
// needed between chunks.
//
static psa_fwu_component_t g_target;                 // inactive vendor slot
static uint8_t  g_pui8Manifest[TI_FWU_MANIFEST_SIZE];// detached manifest buffer
static size_t   g_szFileOffset;                      // bytes received (absolute)
static bool     g_bStarted;                          // psa_fwu_start() done
static bool     g_bAbort;                            // fatal error latched
static int32_t  g_i32NextSeq;                        // idempotent-retry guard
static bool     g_bOtaRebootPending;                 // last chunk installed OK

static uint8_t  g_pui8WrBuf[OTA_WRITE_BLOCK];        // sector-write coalesce buffer
static uint32_t g_ui32WrLen;                         // bytes buffered in g_pui8WrBuf
static size_t   g_szWrBase;                          // image offset of buffer[0]

static uint32_t g_ui32OtaMaxBytes = OTA_MAX_BYTES_FALLBACK;

#if LWIP_HTTPD_SUPPORT_POST
//
// Chunked binary-POST OTA.  A single large POST body stalls this prebuilt
// lwIP/Wi-Fi stack after exactly one TCP receive window (8760 B): during a bulk
// inbound body the device sends nothing back, so window updates never flush and
// the sender freezes.  The hex-GET path never hits this because it is
// request/response -- every chunk gets a tiny reply that flushes the window.  So
// we upload the image as many small BINARY POSTs (<= OTA_POST_CHUNK_MAX each) on
// one keep-alive connection: each POST fits inside a window and its per-chunk
// reply slides the window for the next -- same proven pattern as hex-GET, but
// with no 2x hex overhead and ~10x larger chunks.
//
#define OTA_POST_CHUNK_MAX  8192u            // reject a POST body larger than this (< TCP window)

static void    *g_pPostConn;                         // owning POST connection (this chunk)
static bool     g_bChunkDup;                         // this POST re-sends an already-applied seq
static bool     g_bChunkLast;                        // this POST carries the final image bytes
#endif

//*****************************************************************************
//
// Small self-contained helpers (keep this file free of TivaWare/cgifuncs deps).
//
//*****************************************************************************
static int
OtaFindParam(const char *pcName, char *pcParam[], int32_t i32NumParams)
{
    int32_t i;
    for(i = 0; i < i32NumParams; i++)
    {
        if((pcParam[i] != NULL) && (strcmp(pcParam[i], pcName) == 0))
        {
            return((int)i);
        }
    }
    return(-1);
}

static int
OtaHexNibble(char c)
{
    if((c >= '0') && (c <= '9')) { return(c - '0'); }
    if((c >= 'a') && (c <= 'f')) { return(c - 'a' + 10); }
    if((c >= 'A') && (c <= 'F')) { return(c - 'A' + 10); }
    return(-1);
}

static uint32_t
OtaStrToUl(const char *p)
{
    uint32_t v = 0;
    while((*p >= '0') && (*p <= '9'))
    {
        v = (v * 10u) + (uint32_t)(*p++ - '0');
    }
    return(v);
}

//*****************************************************************************
//
// OtaPrepareTarget - pick the inactive (non-primary) vendor slot and drive it
// to the READY state, ready for psa_fwu_start().  Mirrors the SDK OTA example's
// OTA_FWU_selectTargetSlot + OTA_FWU_prepareSlot.  Returns 0 on success.
//
//*****************************************************************************
static int
OtaPrepareTarget(psa_fwu_component_t *pTarget)
{
    psa_fwu_component_info_t sInfo1, sInfo2, sTgt;
    psa_fwu_component_t      target;

    if((psa_fwu_query(OTA_VENDOR_SLOT_1, &sInfo1) != PSA_SUCCESS) ||
       (psa_fwu_query(OTA_VENDOR_SLOT_2, &sInfo2) != PSA_SUCCESS))
    {
        PalLog("ota: query vendor slots failed\n");
        return(-1);
    }

    if(!sInfo1.impl.Primary && sInfo2.impl.Primary)
    {
        target = OTA_VENDOR_SLOT_1;
    }
    else if(!sInfo2.impl.Primary && sInfo1.impl.Primary)
    {
        target = OTA_VENDOR_SLOT_2;
    }
    else
    {
        PalLog("ota: no non-primary vendor slot to target\n");
        return(-1);
    }

    if(psa_fwu_query(target, &sTgt) != PSA_SUCCESS)
    {
        return(-1);
    }

    //
    // Transition whatever the target currently holds back to READY.
    //
    switch(sTgt.state)
    {
        case PSA_FWU_READY:
            break;
        case PSA_FWU_WRITING:
        case PSA_FWU_CANDIDATE:
            psa_fwu_cancel(target);
            psa_fwu_clean(target);
            break;
        case PSA_FWU_STAGED:
        case PSA_FWU_TRIAL:
            psa_fwu_reject(PSA_ERROR_NOT_PERMITTED);
            psa_fwu_clean(target);
            break;
        case PSA_FWU_FAILED:
        case PSA_FWU_REJECTED:
        case PSA_FWU_UPDATED:
            psa_fwu_clean(target);
            break;
        default:
            PalLog("ota: target in unexpected state %d\n", (int)sTgt.state);
            return(-1);
    }

    if((psa_fwu_query(target, &sTgt) != PSA_SUCCESS) ||
       (sTgt.state != PSA_FWU_READY))
    {
        PalLog("ota: target not READY (state %d)\n", (int)sTgt.state);
        return(-1);
    }

    *pTarget = target;
    return(0);
}

#define PSA_FWU_WRITE(off, buf, len)  psa_fwu_write(g_target, (off), (buf), (len))

//*****************************************************************************
//
// OtaStageImage - coalesce image bytes into flash-sector-aligned writes.  Bytes
// accumulate in g_pui8WrBuf and are handed to psa_fwu_write() only when the
// buffer reaches the next OTA_WRITE_BLOCK-aligned image offset, so each write
// programs a whole sector once instead of read-modify-writing it per GET chunk.
// Returns PSA_SUCCESS, or the failing psa_fwu_write() status.
//
//*****************************************************************************
static psa_status_t
OtaStageImage(const uint8_t *pData, uint32_t ui32Len)
{
    while(ui32Len > 0u)
    {
        uint32_t ui32Here = (uint32_t)(g_szWrBase + g_ui32WrLen);
        uint32_t ui32Next = ((ui32Here / OTA_WRITE_BLOCK) + 1u) * OTA_WRITE_BLOCK;
        uint32_t ui32Room = ui32Next - ui32Here;     // bytes to the next boundary
        uint32_t ui32Take = (ui32Len < ui32Room) ? ui32Len : ui32Room;

        memcpy(&g_pui8WrBuf[g_ui32WrLen], pData, ui32Take);
        g_ui32WrLen += ui32Take;
        pData       += ui32Take;
        ui32Len     -= ui32Take;

        if((size_t)(g_szWrBase + g_ui32WrLen) == (size_t)ui32Next)
        {
            psa_status_t st = PSA_FWU_WRITE(g_szWrBase, g_pui8WrBuf, g_ui32WrLen);
            if(st != PSA_SUCCESS)
            {
                return(st);
            }
            g_szWrBase  += g_ui32WrLen;
            g_ui32WrLen  = 0u;
        }
    }
    return(PSA_SUCCESS);
}

//*****************************************************************************
//
// OtaFlushImage - write the buffered tail (final partial sector) at end of image.
//
//*****************************************************************************
static psa_status_t
OtaFlushImage(void)
{
    psa_status_t st;

    if(g_ui32WrLen == 0u)
    {
        return(PSA_SUCCESS);
    }
    st = PSA_FWU_WRITE(g_szWrBase, g_pui8WrBuf, g_ui32WrLen);
    if(st == PSA_SUCCESS)
    {
        g_szWrBase += g_ui32WrLen;
        g_ui32WrLen = 0u;
    }
    return(st);
}

//*****************************************************************************
//
// OtaFeed - append the next contiguous run of image bytes to the staged image,
// regardless of how the transport framed them.  The first TI_FWU_MANIFEST_SIZE
// bytes of the file are buffered as the detached manifest and handed to
// psa_fwu_start(); every later byte is coalesced into aligned flash blocks by
// OtaStageImage().  g_szFileOffset tracks the absolute byte count consumed.
//
// This is the single staging core shared by both upload transports: the legacy
// hex-GET chunk CGI (WebPlatformOtaChunkCGI) and the streaming binary POST
// callbacks feed identical bytes here.  It does NOT finish/install the image
// (the caller does that once the last byte is in).  Returns PSA_SUCCESS or the
// failing PSA status.
//
//*****************************************************************************
static psa_status_t
OtaFeed(const uint8_t *pData, uint32_t ui32Len)
{
    psa_status_t st;
    uint32_t     ui32Off = 0;

    //
    // Manifest phase: fill g_pui8Manifest, then psa_fwu_start() once complete.
    //
    if(g_szFileOffset < TI_FWU_MANIFEST_SIZE)
    {
        uint32_t ui32Need = (uint32_t)TI_FWU_MANIFEST_SIZE -
                            (uint32_t)g_szFileOffset;
        uint32_t ui32Take = (ui32Len < ui32Need) ? ui32Len : ui32Need;

        memcpy(&g_pui8Manifest[g_szFileOffset], &pData[0], ui32Take);
        g_szFileOffset += ui32Take;
        ui32Off = ui32Take;

        if((g_szFileOffset == TI_FWU_MANIFEST_SIZE) && !g_bStarted)
        {
            st = psa_fwu_start(g_target, g_pui8Manifest, TI_FWU_MANIFEST_SIZE);
            if(st != PSA_SUCCESS)
            {
                return(st);
            }
            g_bStarted  = true;
            g_szWrBase  = TI_FWU_MANIFEST_SIZE;  // image data starts after manifest
            g_ui32WrLen = 0;
            PalLog("ota: manifest accepted, streaming image\n");
        }
    }

    //
    // Image phase: everything after the manifest is written at its file offset.
    //
    if(ui32Off < ui32Len)
    {
        if(!g_bStarted)
        {
            // A transport unit shorter than the manifest would land here.
            return(PSA_ERROR_BAD_STATE);
        }
        st = OtaStageImage(&pData[ui32Off], ui32Len - ui32Off);
        if(st != PSA_SUCCESS)
        {
            return(st);
        }
        g_szFileOffset += (ui32Len - ui32Off);
    }

    return(PSA_SUCCESS);
}

//*****************************************************************************
//
// OtaFinalize - flush the buffered tail, mark the image complete (psa_fwu_finish),
// stage it for the bootloader (psa_fwu_install) and arm the swap-reboot.  Shared
// by both upload transports' end-of-image handling.  Returns true on success;
// the caller cancels + aborts on false.
//
//*****************************************************************************
static bool
OtaFinalize(void)
{
    psa_status_t st;

    st = OtaFlushImage();
    if(st != PSA_SUCCESS)
    {
        PalLog("ota: final psa_fwu_write failed (%d)\n", (int)st);
        return(false);
    }
    st = psa_fwu_finish(g_target);
    if(st != PSA_SUCCESS)
    {
        PalLog("ota: psa_fwu_finish failed (%d)\n", (int)st);
        return(false);
    }
    st = psa_fwu_install();
    if(st != PSA_SUCCESS_REBOOT)
    {
        PalLog("ota: psa_fwu_install failed (%d)\n", (int)st);
        return(false);
    }

    g_bOtaRebootPending = true;
    WebUIRequestReset();   // let the OK page flush; main.c reboots into TRIAL
    PalLog("ota: %u bytes staged, rebooting into trial\n",
           (unsigned)g_szFileOffset);
    return(true);
}

//*****************************************************************************
//
// WebPlatformOtaChunkCGI - receive one hex-encoded chunk of a signed vendor
// image and stage it via PSA FWU.  /fwchunk.cgi?seq=N&last=0|1&data=HEX...
//
// seq==0 selects+prepares the inactive slot.  The first TI_FWU_MANIFEST_SIZE
// bytes are buffered as the detached manifest and passed to psa_fwu_start();
// every later byte is written contiguously with psa_fwu_write().  last==1 marks
// the image (psa_fwu_finish), stages it (psa_fwu_install) and requests a reboot.
//
// Returns the URI the httpd then serves.  Per-chunk ACKs return the tiny
// "/otaack.txt" (a few bytes, non-SSI) rather than the full SSI-rendered
// "/tools.shtml": a ~1.2 MB image is ~2700 rapid connection-close GETs, and
// re-rendering + sending the ~15 KB tools page for every one exhausts the
// prebuilt lwIP heap (MEM_SIZE=60000) and wedges the httpd after ~180 chunks.
// The final chunk returns "/fwupdate_ok.shtml" (its body holds the FWUPDATE_OK
// token the browser/uploader keys success off); a failure keeps the tiny ACK, so
// the last chunk lacks FWUPDATE_OK and the client reports failure.
//
//*****************************************************************************
char *
WebPlatformOtaChunkCGI(int32_t iIndex, int32_t i32NumParams,
                       char *pcParam[], char *pcValue[])
{
    int          iSeq, iData, iLast;
    int32_t      i32SeqNum;
    bool         bLast;
    const char  *pcData;
    uint32_t     ui32HexLen, i, ui32Len;
    uint8_t      pui8Buf[OTA_CHUNK_MAX];
    psa_status_t st;

    (void)iIndex;

    iSeq  = OtaFindParam("seq",  pcParam, i32NumParams);
    iData = OtaFindParam("data", pcParam, i32NumParams);
    iLast = OtaFindParam("last", pcParam, i32NumParams);
    if((iSeq < 0) || (iData < 0))
    {
        return("/otaack.txt");
    }

    i32SeqNum = (int32_t)OtaStrToUl(pcValue[iSeq]);
    bLast     = (iLast >= 0) && (pcValue[iLast][0] == '1');
    pcData    = pcValue[iData];

    //
    // First chunk: reset state and prepare the inactive vendor slot.
    //
    if(i32SeqNum == 0)
    {
        g_szFileOffset      = 0;
        g_bStarted          = false;
        g_bAbort            = false;
        g_i32NextSeq        = 0;
        g_bOtaRebootPending = false;
        g_ui32WrLen         = 0;
        memset(g_pui8Manifest, 0, sizeof(g_pui8Manifest));

        if(OtaPrepareTarget(&g_target) != 0)
        {
            g_bAbort = true;
            PalLog("ota: cannot prepare a target slot, aborting\n");
            return("/otaack.txt");
        }
        PalLog("ota: upload started -> vendor component %d\n", (int)g_target);
    }

    if(g_bAbort)
    {
        return("/otaack.txt");
    }

    //
    // Ordering / idempotency guard (same semantics as the TM4C path): a resent
    // chunk (seq < next) is ACKed without re-writing; a gap (seq > next) aborts.
    //
    if(i32SeqNum < g_i32NextSeq)
    {
        return(bLast ? "/fwupdate_ok.shtml" : "/otaack.txt");
    }
    if(i32SeqNum > g_i32NextSeq)
    {
        g_bAbort = true;
        PalLog("ota: out-of-order chunk (got %d, want %d), aborting\n",
               (int)i32SeqNum, (int)g_i32NextSeq);
        return("/otaack.txt");
    }

    //
    // Decode this chunk's hex payload.
    //
    ui32HexLen = 0;
    while(pcData[ui32HexLen] != '\0') { ui32HexLen++; }
    ui32HexLen &= ~1u;                      // whole byte pairs only
    if((ui32HexLen / 2u) > OTA_CHUNK_MAX)
    {
        g_bAbort = true;
        PalLog("ota: chunk too large (%u bytes), aborting\n",
               (unsigned)(ui32HexLen / 2u));
        return("/otaack.txt");
    }
    ui32Len = 0;
    for(i = 0; i < ui32HexLen; i += 2)
    {
        int hi = OtaHexNibble(pcData[i]);
        int lo = OtaHexNibble(pcData[i + 1]);
        if((hi < 0) || (lo < 0))
        {
            g_bAbort = true;
            PalLog("ota: bad hex in chunk, aborting\n");
            return("/otaack.txt");
        }
        pui8Buf[ui32Len++] = (uint8_t)((hi << 4) | lo);
    }

    //
    // Stage this chunk's bytes (manifest split + coalesced flash writes) through
    // the shared feeder.  On any PSA failure, cancel and abort the upload.
    //
    st = OtaFeed(pui8Buf, ui32Len);
    if(st != PSA_SUCCESS)
    {
        g_bAbort = true;
        psa_fwu_cancel(g_target);
        PalLog("ota: staging @%u failed (%d), aborting\n",
               (unsigned)g_szFileOffset, (int)st);
        return("/otaack.txt");
    }

    //
    // Chunk fully applied; a resend of this seq is now a duplicate ACK.
    //
    g_i32NextSeq++;

    //
    // Last chunk: finish -> install -> request reboot into TRIAL.
    //
    if(bLast)
    {
        if(!OtaFinalize())
        {
            g_bAbort = true;
            psa_fwu_cancel(g_target);
            return("/otaack.txt");
        }
        return("/fwupdate_ok.shtml");
    }

    return("/tools.shtml");
}

#if LWIP_HTTPD_SUPPORT_POST
//*****************************************************************************
//
// Streaming binary-POST OTA (CC35x1's fast path).  The Tools page uploads the
// signed vendor image as ONE raw POST body to /fwupload; the lwIP httpd hands us
// the body pbuf-by-pbuf, in order, as it arrives off TCP -- no hex expansion and
// no per-chunk request/response round trips, so the image streams at line rate
// straight into psa_fwu_write() (vs ~2700 hex GETs before).  We reuse the same
// OtaPrepareTarget / OtaFeed / OtaFinalize staging core as the legacy chunk CGI.
//
// The three callbacks below are the app hooks the httpd calls (they are global
// symbols, not CGI-table entries).  Only /fwupload is accepted; a single upload
// is in flight at a time (state is file-global), tracked by g_pPostConn.
//
//*****************************************************************************

//
// OtaUriQueryU32 - read an unsigned decimal query parameter (e.g. "seq") from a
// request URI like "/fwupload?seq=5&last=0".  Returns true and *pui32Out on hit.
//
static bool
OtaUriQueryU32(const char *pcUri, const char *pcKey, uint32_t *pui32Out)
{
    size_t      szKey = strlen(pcKey);
    const char *p     = pcUri;

    // Match the key only where it starts a real query parameter (right after '?'
    // or '&'), so "seq=" can't be captured from inside e.g. "myseq=".
    while((p = strstr(p, pcKey)) != NULL)
    {
        if((p != pcUri) && ((p[-1] == '?') || (p[-1] == '&')))
        {
            *pui32Out = OtaStrToUl(p + szKey);
            return(true);
        }
        p += szKey;
    }
    return(false);
}

//
// httpd_post_begin - one chunk of the chunked binary upload arrived.  The image
// is POSTed as many small binary bodies to /fwupload?seq=N&last=L over a single
// keep-alive connection; each POST is <= one TCP window and its reply flushes the
// window for the next (the pattern hex-GET uses, which this stack needs -- a
// single large body stalls at one window).  seq==0 resets state and prepares the
// slot; each body then feeds OtaFeed inline (small enough that tcpip_thread is
// free again before the next chunk).  On denial we name the tiny ACK page.
//
err_t
httpd_post_begin(void *connection, const char *uri, const char *http_request,
                 u16_t http_request_len, int content_len, char *response_uri,
                 u16_t response_uri_len, u8_t *post_auto_wnd)
{
    uint32_t ui32Seq = 0, ui32Last = 0;

    (void)http_request;
    (void)http_request_len;

    // Match exactly "/fwupload" plus an optional ?query -- NOT a longer path like
    // "/fwuploadX" (a stray POST there with seq=0 would reset staging + churn the
    // PSA slot).  uri[9] is safe to read: strncmp confirmed 9 chars, so [9] is at
    // worst the terminating '\0'.
    if((uri == NULL) || (strncmp(uri, "/fwupload", 9) != 0) ||
       ((uri[9] != '\0') && (uri[9] != '?')))
    {
        return(ERR_VAL);    // not our endpoint -> httpd handles it normally
    }

    // Automatic receive window: each chunk fits one window, and the per-chunk
    // response flushes the window update for the next chunk.
    *post_auto_wnd = 1;

    if((content_len <= 0) || ((uint32_t)content_len > OTA_POST_CHUNK_MAX))
    {
        PalLog("ota(post): bad chunk length %d (max %u)\n",
               content_len, (unsigned)OTA_POST_CHUNK_MAX);
        strncpy(response_uri, "/otaack.txt", response_uri_len);
        response_uri[response_uri_len - 1] = '\0';
        return(ERR_VAL);
    }

    if(!OtaUriQueryU32(uri, "seq=", &ui32Seq))
    {
        strncpy(response_uri, "/otaack.txt", response_uri_len);
        response_uri[response_uri_len - 1] = '\0';
        return(ERR_VAL);
    }
    (void)OtaUriQueryU32(uri, "last=", &ui32Last);

    //
    // First chunk: reset staging state and prepare the inactive vendor slot.
    //
    if(ui32Seq == 0u)
    {
        g_szFileOffset      = 0;
        g_bStarted          = false;
        g_bAbort            = false;
        g_i32NextSeq        = 0;
        g_bOtaRebootPending = false;
        g_ui32WrLen         = 0;
        memset(g_pui8Manifest, 0, sizeof(g_pui8Manifest));

        if(OtaPrepareTarget(&g_target) != 0)
        {
            g_bAbort = true;
            PalLog("ota(post): cannot prepare a target slot\n");
            strncpy(response_uri, "/otaack.txt", response_uri_len);
            response_uri[response_uri_len - 1] = '\0';
            return(ERR_VAL);
        }
        PalLog("ota(post): upload started -> vendor component %d\n",
               (int)g_target);
    }

    if(g_bAbort)
    {
        strncpy(response_uri, "/otaack.txt", response_uri_len);
        response_uri[response_uri_len - 1] = '\0';
        return(ERR_VAL);
    }

    //
    // Ordering / idempotency (same semantics as the hex-GET path): a resent chunk
    // (seq < next) is accepted but its body discarded; a gap (seq > next) aborts.
    //
    if((int32_t)ui32Seq < g_i32NextSeq)
    {
        g_bChunkDup = true;
    }
    else if((int32_t)ui32Seq > g_i32NextSeq)
    {
        g_bAbort = true;
        PalLog("ota(post): out-of-order chunk (got %d, want %d), aborting\n",
               (int)ui32Seq, (int)g_i32NextSeq);
        strncpy(response_uri, "/otaack.txt", response_uri_len);
        response_uri[response_uri_len - 1] = '\0';
        return(ERR_VAL);
    }
    else
    {
        g_bChunkDup = false;
    }

    //
    // Cumulative image-size cap (a fresh chunk only): the accumulated file must
    // fit the vendor slot.  The hex-GET path relied on psa_fwu_write to fail past
    // the slot end; refuse up front here instead of streaming into a doomed write.
    //
    if(!g_bChunkDup &&
       (((uint64_t)g_szFileOffset + (uint32_t)content_len) > g_ui32OtaMaxBytes))
    {
        g_bAbort = true;
        PalLog("ota(post): image over slot cap (%u+%d > %u), aborting\n",
               (unsigned)g_szFileOffset, content_len, (unsigned)g_ui32OtaMaxBytes);
        strncpy(response_uri, "/otaack.txt", response_uri_len);
        response_uri[response_uri_len - 1] = '\0';
        return(ERR_VAL);
    }

    g_bChunkLast = (ui32Last != 0u);
    g_pPostConn  = connection;
    return(ERR_OK);
}

//
// httpd_post_receive_data - body bytes of the current chunk.  Feed them straight
// into the staging core (small chunk -> quick inline psa_fwu_write), then free
// the pbuf (httpd contract).  A duplicate/aborted chunk is drained and discarded.
//
err_t
httpd_post_receive_data(void *connection, struct pbuf *p)
{
    struct pbuf *q;

    if(connection != g_pPostConn)
    {
        if(p != NULL) { pbuf_free(p); }
        return(ERR_VAL);
    }
    if(g_bAbort || g_bChunkDup)
    {
        if(p != NULL) { pbuf_free(p); }     // already have these bytes (or dead)
        return(ERR_OK);
    }

    for(q = p; q != NULL; q = q->next)
    {
        if(q->len > 0u)
        {
            psa_status_t st = OtaFeed((const uint8_t *)q->payload,
                                      (uint32_t)q->len);
            if(st != PSA_SUCCESS)
            {
                g_bAbort = true;
                psa_fwu_cancel(g_target);
                PalLog("ota(post): staging @%u failed (%d), aborting\n",
                       (unsigned)g_szFileOffset, (int)st);
                pbuf_free(p);
                return(ERR_VAL);
            }
        }
    }

    pbuf_free(p);
    return(ERR_OK);
}

//
// httpd_post_finished - the current chunk's body is complete.  A fresh chunk
// advances the sequence and, on the last one, finalizes + installs the image.
// Every non-final chunk returns the tiny ACK page, whose transmission flushes the
// TCP window so the next chunk can flow.
//
void
httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len)
{
    const char *pcResp = "/otaack.txt";

    if(connection != g_pPostConn)
    {
        return;
    }

    if(g_bAbort)
    {
        pcResp = "/otaack.txt";
    }
    else if(g_bChunkDup)
    {
        // Already applied on the first delivery; echo the same verdict.
        pcResp = g_bChunkLast ? "/fwupdate_ok.shtml" : "/otaack.txt";
    }
    else
    {
        g_i32NextSeq++;                     // this chunk is now applied
        if(g_bChunkLast)
        {
            if(OtaFinalize())
            {
                pcResp = "/fwupdate_ok.shtml";
            }
            else
            {
                g_bAbort = true;
                psa_fwu_cancel(g_target);
                pcResp = "/otaack.txt";
            }
        }
        else
        {
            pcResp = "/otaack.txt";
        }
    }

    strncpy(response_uri, pcResp, response_uri_len);
    response_uri[response_uri_len - 1] = '\0';
}
#endif /* LWIP_HTTPD_SUPPORT_POST */

//*****************************************************************************
//
// WebPlatformOtaUsePost - value rendered by the "otapost" SSI tag: 1 tells the
// shared uploader (fs/tools.shtml) to stream the image as a single binary POST
// to /fwupload (this platform's fast path) instead of the hex-GET chunk loop.
//
//*****************************************************************************
uint32_t
WebPlatformOtaUsePost(void)
{
#if LWIP_HTTPD_SUPPORT_POST
    return(1u);
#else
    return(0u);
#endif
}

//*****************************************************************************
//
// WebPlatformOtaInit - one-time PSA FWU init.  Reads the boot report and sets up
// component states; also caches the vendor slot's max image size for "otamax".
// Call once at startup (before any query / before serving the web UI).
//
//*****************************************************************************
void
WebPlatformOtaInit(void)
{
    psa_fwu_component_info_t sInfo;

    psa_fwu_init();

    if((psa_fwu_query(OTA_VENDOR_SLOT_1, &sInfo) == PSA_SUCCESS) &&
       (sInfo.max_size != 0u))
    {
        g_ui32OtaMaxBytes = sInfo.max_size;
    }
    PalLog("ota: FWU init done, staging cap %u bytes\n",
           (unsigned)g_ui32OtaMaxBytes);
}

//*****************************************************************************
//
// WebPlatformOtaMaxBytes - value rendered by the "otamax" SSI tag: the largest
// image the browser should offer to upload (this platform's vendor slot size).
//
//*****************************************************************************
uint32_t
WebPlatformOtaMaxBytes(void)
{
    return(g_ui32OtaMaxBytes);
}

//*****************************************************************************
//
// WebPlatformOtaTrialAccept - if this boot is running a freshly-installed image
// in TRIAL, commit it.  Called by main.c once the board is healthy (Wi-Fi up),
// so an OTA that boots and reaches the network is made permanent; one that does
// not is rolled back by the bootloader on the next power-cycle.  Commit needs a
// reboot (TRIAL -> UPDATED), after which the new image boots normally.
//
//*****************************************************************************
void
WebPlatformOtaTrialAccept(void)
{
    psa_fwu_component_info_t sInfo;
    bool bTrial = false;

    if((psa_fwu_query(OTA_VENDOR_SLOT_1, &sInfo) == PSA_SUCCESS) &&
       (sInfo.state == PSA_FWU_TRIAL))
    {
        bTrial = true;
    }
    if((psa_fwu_query(OTA_VENDOR_SLOT_2, &sInfo) == PSA_SUCCESS) &&
       (sInfo.state == PSA_FWU_TRIAL))
    {
        bTrial = true;
    }
    if(!bTrial)
    {
        return;
    }

    PalLog("ota: healthy boot in TRIAL -> accepting update\n");
    if(psa_fwu_accept() == PSA_SUCCESS_REBOOT)
    {
        psa_fwu_request_reboot();   // commit; does not return
    }
}

//*****************************************************************************
//
// WebPlatformFinalizeReboot - the reset seam main.c calls when a web request
// asked for a reboot.  If an OTA image was just staged, reboot through PSA so
// the bootloader swaps to the new slot; otherwise a plain system reset.  Does
// not return.
//
//*****************************************************************************
void
WebPlatformFinalizeReboot(void)
{
    if(g_bOtaRebootPending)
    {
        PalLog("ota: rebooting via PSA to swap in the new image\n");
        psa_fwu_request_reboot();   // bootloader stages TRIAL; does not return
    }
    PalReboot();                    // normal reboot (/reboot.cgi, factory reset)
}
