//*****************************************************************************
//
// product_web.c - home-auto product web layer (Plan 11 Scope C).
//
// Extracted from iot_foundation/common/webui.c. This TU owns the product's web
// surface: the I/O Config + Control tab CGI handlers (/iocfg, /relaypulse,
// /nameset, /outcfg, /cover, /relayset, /roomcfg), the product SSI tag table and
// its renderer (product_ssi_handler), and the registration hook
// (product_web_register) that hands the product's CGI/SSI tables to the
// foundation httpd via WebUIRegister().
//
// The foundation (webui.c) owns the httpd shell, the base tabs (Status/Settings/
// Wi-Fi/OTA) and their handlers, and calls the two hooks declared in
// product_web.h. Shared helpers reach across the seam one way (product ->
// foundation): HexNibble() and the WebUIRequest*/WebPlatform* accessors +
// g_pui8LiveInState/g_ui32IPAddress come from webui.h; the config/relay/output
// APIs from their own headers. UrlDecodeParam is product-only and lives here.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef CC35XX
#include "lwip/apps/httpd.h"
#include "lwip/tcpip.h"
#else
#include "httpserver_raw/httpd.h"
#define LOCK_TCPIP_CORE()
#define UNLOCK_TCPIP_CORE()
#endif

// Foundation<->product web contract (Plan 11).  Included after httpd.h so the
// web hook types (tCGI) resolve; it pulls in product_api.h.
#include "product_web.h"

#include "config.h"
#include "cgifuncs.h"
#include "mqtt_app.h"
#include "din_chain.h"
#include "io_scan.h"
#include "relay_chain.h"
#include "input_events.h"
#include "relay_pulse.h"
#include "output_ctrl.h"
#include "sntp_client.h"
#include "buildinfo.h"
#include "pal_log.h"
#include "pal_str.h"
#include "webui.h"

//
// Map the TivaWare formatting/log calls in the moved bodies onto the PAL
// (identical signatures), so this module is single-source across platforms.
// Mirrors the mapping in webui.c.
//
#define UARTprintf   PalLog
#define usnprintf    PalSnprintf
#define ustrtoul     PalStrToUl
#define ustrncpy     strncpy       // TivaWare ustrncpy == C strncpy (copy n, NUL-pad)
#define ustrlen      strlen        // TivaWare ustrlen  == C strlen

//
// Product CGI response pages (moved from webui.c; foundation keeps DEFAULT_ /
// PARAM_ERROR_RESPONSE).
//
#define IOCFG_CGI_RESPONSE      "/iocfg.shtml"
#define CONTROL_CGI_RESPONSE    "/control.shtml"

//*****************************************************************************
//
// Convert one ASCII hex character to its 4-bit value.  Product-local copy (the
// foundation webui.c and the TM4C enet_io.c OTA decoder each keep their own),
// used by the I/O-config binding parser and UrlDecodeParam below.
//
//*****************************************************************************
static uint8_t
HexNibble(char c)
{
    if((c >= '0') && (c <= '9')) { return((uint8_t)(c - '0')); }
    if((c >= 'a') && (c <= 'f')) { return((uint8_t)(c - 'a' + 10)); }
    if((c >= 'A') && (c <= 'F')) { return((uint8_t)(c - 'A' + 10)); }
    return(0);
}

//*****************************************************************************
//
// CGI handler for /iocfg.cgi.  Parses device counts and per-input type
// bitmask from the I/O configuration form, persists them to EEPROM and
// triggers a re-publish of HA discovery topics.
//
//*****************************************************************************
static char *
IOConfigCGIHandler(int32_t iIndex, int32_t i32NumParams, char *pcParam[],
                   char *pcValue[])
{
    int32_t i32Idx;

    (void)iIndex;

    //
    // Device counts.
    //
    i32Idx = FindCGIParameter("din", pcParam, i32NumParams);
    if(i32Idx != -1)
    {
        ConfigSetDinDevices((uint8_t)ustrtoul(pcValue[i32Idx], 0, 10));
    }
    i32Idx = FindCGIParameter("relay", pcParam, i32NumParams);
    if(i32Idx != -1)
    {
        ConfigSetRelayDevices((uint8_t)ustrtoul(pcValue[i32Idx], 0, 10));
    }

    //
    // Per-input type bitmask ("types" = hex string, 2 chars per byte of 8
    // inputs; bit n of byte b = input b*8+n is pushbutton when set).
    //
    i32Idx = FindCGIParameter("types", pcParam, i32NumParams);
    if(i32Idx != -1)
    {
        const char *pcT = pcValue[i32Idx];
        int iByte = 0;
        while((pcT[0] != '\0') && (pcT[1] != '\0') && (iByte < 15))
        {
            uint8_t ui8Val = (uint8_t)((HexNibble(pcT[0]) << 4) |
                                       HexNibble(pcT[1]));
            int iBit;
            for(iBit = 0; iBit < 8; iBit++)
            {
                ConfigSetInputPushbutton(iByte * 8 + iBit,
                                        (ui8Val & (1u << iBit)) != 0);
            }
            pcT += 2;
            iByte++;
        }
    }

    //
    // Per-input binding table: "binds" = 3 hex chars per slot, 4 slots per
    // input, for up to D*8 inputs.  12-bit encoding: output[6:0]|act[4:3]|trig[2:0].
    // "000" = unused slot.  (Parsing also stops at the end of the sent string.)
    //
    i32Idx = FindCGIParameter("binds", pcParam, i32NumParams);
    if(i32Idx != -1)
    {
        const char *pcB = pcValue[i32Idx];
        int iLen = (int)strlen(pcB);
        int iMaxIn = (int)IOInputCount();
        int iInput, iSlot, iPos = 0;
        if(iMaxIn > CFG_MAX_INPUTS) { iMaxIn = CFG_MAX_INPUTS; }
        for(iInput = 0; (iInput < iMaxIn) && ((iPos + 2) < iLen); iInput++)
        {
            for(iSlot = 0; (iSlot < CFG_BIND_SLOTS) &&
                           ((iPos + 2) < iLen); iSlot++, iPos += 3)
            {
                uint16_t ui16V = (uint16_t)(((uint16_t)HexNibble(pcB[iPos]) << 8) |
                                            ((uint16_t)HexNibble(pcB[iPos+1]) << 4) |
                                             (uint16_t)HexNibble(pcB[iPos+2]));
                uint8_t ui8TrigAct = (uint8_t)(ui16V & 0x1Fu);
                uint8_t ui8Out     = (uint8_t)((ui16V >> 5) & 0x7Fu);
                if((ui8TrigAct & 0x07u) == 0)
                {
                    ui8TrigAct = 0;
                    ui8Out = BIND_OUTPUT_NONE;
                }
                ConfigBindingSet(iInput, iSlot, ui8TrigAct, ui8Out);
            }
        }
    }

    //
    // Persist all three records and request chain + discovery update.
    // Release the tcpip_thread lock around config saves so PalLog can run safely.
    //
    UNLOCK_TCPIP_CORE();
    ConfigSave();
    ConfigIOSave();
    ConfigBindingSave();
    LOCK_TCPIP_CORE();
    WebUIRequestMqttApply();
    WebUIRequestMqttRepublish();

    return(IOCFG_CGI_RESPONSE);
}

//*****************************************************************************
//
// UrlDecodeParam - decode a CGI parameter value that may contain + (space)
// or %XX sequences produced by JS encodeURIComponent / form encoding.
//
//*****************************************************************************
static void
UrlDecodeParam(const char *pcIn, char *pcOut, int iMax)
{
    int iIn = 0, iOut = 0;
    while(pcIn[iIn] && iOut < iMax - 1)
    {
        if(pcIn[iIn] == '+')
        {
            pcOut[iOut++] = ' ';
            iIn++;
        }
        else if(pcIn[iIn] == '%' && pcIn[iIn + 1] && pcIn[iIn + 2])
        {
            pcOut[iOut++] = (char)((HexNibble(pcIn[iIn + 1]) << 4) |
                                    HexNibble(pcIn[iIn + 2]));
            iIn += 3;
        }
        else
        {
            pcOut[iOut++] = pcIn[iIn++];
        }
    }
    pcOut[iOut] = '\0';
}

//*****************************************************************************
//
// NameSetCGIHandler - save a custom name for one input or output channel.
//
// URL: /nameset.cgi?type=in|out&idx=N&name=<URL-encoded string, max 11 chars>
//
//*****************************************************************************
static char *
NameSetCGIHandler(int32_t iIndex, int32_t i32NumParams,
                  char *pcParam[], char *pcValue[])
{
    int32_t iTypeIdx, iIdxIdx, iNameIdx;
    bool    bInput;
    int     iIdx;
    char    acName[CFG_NAME_LEN];

    iTypeIdx = FindCGIParameter("type", pcParam, i32NumParams);
    iIdxIdx  = FindCGIParameter("idx",  pcParam, i32NumParams);
    iNameIdx = FindCGIParameter("name", pcParam, i32NumParams);

    if(iTypeIdx < 0 || iIdxIdx < 0 || iNameIdx < 0)
    {
        return(IOCFG_CGI_RESPONSE);
    }

    bInput = (pcValue[iTypeIdx][0] == 'i');
    iIdx   = (int)ustrtoul(pcValue[iIdxIdx], NULL, 10);
    UrlDecodeParam(pcValue[iNameIdx], acName, CFG_NAME_LEN);
    ConfigNameSet(bInput, iIdx, acName);
    UARTprintf("Name: %s[%d] = \"%s\"\n", bInput ? "in" : "out", iIdx, acName);
    return(IOCFG_CGI_RESPONSE);
}

//*****************************************************************************
//
// RelayPulseCGIHandler - Trigger a timed relay pulse from the web UI.
//
// URL: /relaypulse.cgi?relay=N&ms=D
//   relay = 0-based relay index
//   ms    = pulse duration in milliseconds (1..3 600 000)
//
//*****************************************************************************
static char *
RelayPulseCGIHandler(int32_t iIndex, int32_t i32NumParams,
                     char *pcParam[], char *pcValue[])
{
    int32_t  iRelayIdx, iMsIdx;
    uint32_t ui32Relay, ui32Ms;

    iRelayIdx = FindCGIParameter("relay", pcParam, i32NumParams);
    iMsIdx    = FindCGIParameter("ms",    pcParam, i32NumParams);

    if(iRelayIdx < 0)
    {
        return(IOCFG_CGI_RESPONSE);
    }

    ui32Relay = ustrtoul(pcValue[iRelayIdx], NULL, 10);
    ui32Ms    = (iMsIdx >= 0) ? ustrtoul(pcValue[iMsIdx], NULL, 10) : 0;

    if(ui32Ms == 0)
    {
        ui32Ms = 1000;   // implicit default: missing/0 duration pulses for 1 s
    }

    if(ui32Ms > 3600000u || ui32Relay >= (uint32_t)RelayChainCount())
    {
        return(IOCFG_CGI_RESPONSE);
    }

    UARTprintf("Pulse: relay %u for %u ms (web UI)\n", ui32Relay, ui32Ms);
    RelayPulseStart((int)ui32Relay, ui32Ms);
    return(IOCFG_CGI_RESPONSE);
}

//*****************************************************************************
//
// OutCfgCGIHandler - Save per-output modes, timed durations, and the shutter
// table.  Query parameters (all optional):
//   modes = one char per output, '1' = Timed, else Standard.
//   tmo   = comma-separated timed auto-OFF durations (ms), one per output.
//   sh    = "up:down:travel;up:down:travel;..." shutter definitions (rebuilds
//           the whole table; invalid pairs are skipped).
//
//*****************************************************************************
static char *
OutCfgCGIHandler(int32_t iIndex, int32_t i32NumParams, char *pcParam[],
                 char *pcValue[])
{
    int32_t idx;
    int     i, iSlot;

    (void)iIndex;

    //
    // Per-output mode.
    //
    idx = FindCGIParameter("modes", pcParam, i32NumParams);
    if(idx >= 0)
    {
        const char *p = pcValue[idx];
        for(i = 0; (p[i] != '\0') && (i < CFG_MAX_OUTPUTS); i++)
        {
            ConfigSetOutMode(i, (p[i] == '1') ? OUT_MODE_TIMED
                                              : OUT_MODE_STANDARD);
        }
    }

    //
    // Per-output timed auto-OFF durations.
    //
    idx = FindCGIParameter("tmo", pcParam, i32NumParams);
    if(idx >= 0)
    {
        const char *p = pcValue[idx];
        i = 0;
        while((*p != '\0') && (i < CFG_MAX_OUTPUTS))
        {
            uint32_t v = 0;
            while((*p >= '0') && (*p <= '9')) { v = (v * 10u) + (uint32_t)(*p - '0'); p++; }
            ConfigSetOutTimedMs(i, v ? v : 1000u);
            i++;
            if(*p == ',') { p++; } else { break; }
        }
    }

    //
    // Shutter table — always rebuilt from scratch.
    //
    for(iSlot = 0; iSlot < CFG_MAX_SHUTTERS; iSlot++)
    {
        ConfigShutterClear(iSlot);
    }
    idx = FindCGIParameter("sh", pcParam, i32NumParams);
    if(idx >= 0)
    {
        const char *p = pcValue[idx];
        iSlot = 0;
        while((*p != '\0') && (iSlot < CFG_MAX_SHUTTERS))
        {
            uint32_t up = 0, down = 0, travel = 0;
            char     acName[CFG_NAME_LEN];
            acName[0] = '\0';
            while((*p >= '0') && (*p <= '9')) { up = (up * 10u) + (uint32_t)(*p - '0'); p++; }
            if(*p != ':') { break; }
            p++;
            while((*p >= '0') && (*p <= '9')) { down = (down * 10u) + (uint32_t)(*p - '0'); p++; }
            if(*p != ':') { break; }
            p++;
            while((*p >= '0') && (*p <= '9')) { travel = (travel * 10u) + (uint32_t)(*p - '0'); p++; }

            //
            // Optional 4th field ":name" (URL-encoded, runs to ';' or end).
            //
            if(*p == ':')
            {
                char acEnc[3 * CFG_NAME_LEN];
                int  iEnc = 0;
                p++;
                while((*p != '\0') && (*p != ';'))
                {
                    if(iEnc < (int)sizeof(acEnc) - 1) { acEnc[iEnc++] = *p; }
                    p++;
                }
                acEnc[iEnc] = '\0';
                UrlDecodeParam(acEnc, acName, CFG_NAME_LEN);
            }

            if((up != down) && (up < RelayChainCount()) && (down < RelayChainCount()))
            {
                ConfigShutterSet(iSlot, (uint8_t)up, (uint8_t)down,
                                 travel ? travel : 20000u);
                ConfigShutterNameSet(iSlot, acName);
                iSlot++;
            }
            if(*p == ';') { p++; } else { break; }
        }
    }

    UNLOCK_TCPIP_CORE();
    ConfigOutputSave();
    LOCK_TCPIP_CORE();
    OutputCtrlReload();
    WebUIRequestMqttRepublish();   // refresh HA discovery (covers vs switches)
    UARTprintf("Output config saved via web UI.\n");
    return(IOCFG_CGI_RESPONSE);
}

//*****************************************************************************
//
// CoverCGIHandler - Web UP/DOWN/STOP buttons for a shutter.
//   /cover.cgi?sh=N&cmd=up|down|stop
//
//*****************************************************************************
static char *
CoverCGIHandler(int32_t iIndex, int32_t i32NumParams, char *pcParam[],
                char *pcValue[])
{
    int32_t  iShIdx, iCmdIdx;
    uint32_t ui32Sh;
    tShCmd   eCmd;

    (void)iIndex;

    iShIdx  = FindCGIParameter("sh",  pcParam, i32NumParams);
    iCmdIdx = FindCGIParameter("cmd", pcParam, i32NumParams);
    if((iShIdx < 0) || (iCmdIdx < 0))
    {
        return(IOCFG_CGI_RESPONSE);
    }

    ui32Sh = ustrtoul(pcValue[iShIdx], NULL, 10);
    switch(pcValue[iCmdIdx][0])
    {
        case 'u': eCmd = SH_CMD_UP;     break;   // momentary: idle→open, moving→stop
        case 'd': eCmd = SH_CMD_DOWN;   break;   // momentary: idle→close, moving→stop
        case 's': eCmd = SH_CMD_STOP;   break;
        case 'c': eCmd = SH_CMD_TOGGLE; break;   // single-button cycle
        default:  return(IOCFG_CGI_RESPONSE);
    }

    UARTprintf("Cover: shutter %u cmd %d (web UI)\n", ui32Sh, (int)eCmd);
    OutputCtrlShutter((int)ui32Sh, eCmd);
    return(IOCFG_CGI_RESPONSE);
}

//*****************************************************************************
//
// RelaySetCGIHandler - direct ON/OFF/Toggle for a light (Control dashboard).
//   /relayset.cgi?relay=N&cmd=on|off|toggle
// Routed through OutputCtrlCommand so the output's mode (Standard/Timed) and
// any shutter-member translation are honored, and the state is published.
//
//*****************************************************************************
static char *
RelaySetCGIHandler(int32_t iIndex, int32_t i32NumParams, char *pcParam[],
                   char *pcValue[])
{
    int32_t  iRelIdx, iCmdIdx;
    uint32_t ui32Relay;
    tOutCmd  eCmd;

    (void)iIndex;

    iRelIdx = FindCGIParameter("relay", pcParam, i32NumParams);
    iCmdIdx = FindCGIParameter("cmd",   pcParam, i32NumParams);
    if((iRelIdx < 0) || (iCmdIdx < 0))
    {
        return(CONTROL_CGI_RESPONSE);
    }

    ui32Relay = ustrtoul(pcValue[iRelIdx], NULL, 10);
    if(ui32Relay >= RelayChainCount())
    {
        return(CONTROL_CGI_RESPONSE);
    }

    //
    // cmd = "on" | "off" | "toggle".  "on"/"off" share a first char, so
    // disambiguate on the second; anything else is a toggle.
    //
    if(pcValue[iCmdIdx][0] == 'o')
    {
        eCmd = (pcValue[iCmdIdx][1] == 'n') ? OUT_CMD_ON : OUT_CMD_OFF;
    }
    else
    {
        eCmd = OUT_CMD_TOGGLE;
    }

    UARTprintf("RelaySet: relay %u cmd %d (web UI)\n", ui32Relay, (int)eCmd);
    OutputCtrlCommand((int)ui32Relay, eCmd);
    return(CONTROL_CGI_RESPONSE);
}

//*****************************************************************************
//
// RoomCfgCGIHandler - save room names + per-output/per-shutter room assignment.
//   /roomcfg.cgi?rooms=<;-sep URL-encoded names>&outr=<csv>&shr=<csv>
// "255" (or any value >= CFG_MAX_ROOMS) means unassigned.
//
//*****************************************************************************
static char *
RoomCfgCGIHandler(int32_t iIndex, int32_t i32NumParams, char *pcParam[],
                  char *pcValue[])
{
    int32_t idx;
    int     i;

    (void)iIndex;

    //
    // Room names: ';'-separated, each URL-encoded (so ';'/'&' cannot appear
    // literally inside a name), index 0..CFG_MAX_ROOMS-1.
    //
    idx = FindCGIParameter("rooms", pcParam, i32NumParams);
    if(idx >= 0)
    {
        const char *p = pcValue[idx];
        int iRoom = 0;
        while(iRoom < CFG_MAX_ROOMS)
        {
            char acEnc[3 * CFG_NAME_LEN];
            char acName[CFG_NAME_LEN];
            int  iEnc = 0;
            while((*p != '\0') && (*p != ';'))
            {
                if(iEnc < (int)sizeof(acEnc) - 1) { acEnc[iEnc++] = *p; }
                p++;
            }
            acEnc[iEnc] = '\0';
            UrlDecodeParam(acEnc, acName, CFG_NAME_LEN);
            ConfigRoomNameSet(iRoom, acName);
            iRoom++;
            if(*p == ';') { p++; } else { break; }
        }
        for(; iRoom < CFG_MAX_ROOMS; iRoom++) { ConfigRoomNameSet(iRoom, ""); }
    }

    //
    // Per-output room index (comma list, one per output).
    //
    idx = FindCGIParameter("outr", pcParam, i32NumParams);
    if(idx >= 0)
    {
        const char *p = pcValue[idx];
        i = 0;
        while((*p != '\0') && (i < CFG_MAX_OUTPUTS))
        {
            uint32_t v = 0;
            while((*p >= '0') && (*p <= '9')) { v = (v * 10u) + (uint32_t)(*p - '0'); p++; }
            ConfigOutRoomSet(i, (v < CFG_MAX_ROOMS) ? (uint8_t)v : ROOM_NONE);
            i++;
            if(*p == ',') { p++; } else { break; }
        }
    }

    //
    // Per-shutter room index (comma list, defined shutters in slot order).
    //
    idx = FindCGIParameter("shr", pcParam, i32NumParams);
    if(idx >= 0)
    {
        const char *p = pcValue[idx];
        i = 0;
        while((*p != '\0') && (i < CFG_MAX_SHUTTERS))
        {
            uint32_t v = 0;
            while((*p >= '0') && (*p <= '9')) { v = (v * 10u) + (uint32_t)(*p - '0'); p++; }
            ConfigShRoomSet(i, (v < CFG_MAX_ROOMS) ? (uint8_t)v : ROOM_NONE);
            i++;
            if(*p == ',') { p++; } else { break; }
        }
    }

    UNLOCK_TCPIP_CORE();
    ConfigRoomSave();
    LOCK_TCPIP_CORE();
    UARTprintf("Room config saved via web UI.\n");
    return(IOCFG_CGI_RESPONSE);
}

//*****************************************************************************
//
// ===== PRODUCT (home-auto) web hooks — TEMPORARY HOME (Plan 11) =====
//
// This section relocates to products/home_auto/web/product_web.c at the rename.
// It publishes the product's web tables (CGI now; SSI tags in the next step) to
// the foundation httpd via product_web_register().  The product CGI *handler
// bodies* (IOConfigCGIHandler, RelayPulseCGIHandler, NameSetCGIHandler,
// OutCfgCGIHandler, CoverCGIHandler, RelaySetCGIHandler, RoomCfgCGIHandler)
// remain in place above for now; they move with this section at the rename.
//
//*****************************************************************************
static const tCGI g_psProductCGIURIs[] =
{
    { "/iocfg.cgi",      (tCGIHandler)IOConfigCGIHandler  },
    { "/relaypulse.cgi", (tCGIHandler)RelayPulseCGIHandler },
    { "/nameset.cgi",    (tCGIHandler)NameSetCGIHandler   },
    { "/outcfg.cgi",     (tCGIHandler)OutCfgCGIHandler    },
    { "/cover.cgi",      (tCGIHandler)CoverCGIHandler     },
    { "/relayset.cgi",   (tCGIHandler)RelaySetCGIHandler  },
    { "/roomcfg.cgi",    (tCGIHandler)RoomCfgCGIHandler   }
};
#define NUM_PRODUCT_CGI_URIS    (sizeof(g_psProductCGIURIs) / sizeof(tCGI))

// ---- Product SSI tag indices (I/O Config + Control tabs) ----
// These are PRODUCT-RELATIVE: the foundation registers its own tags first, then
// appends this array, and forwards any tag index >= the foundation count to
// product_ssi_handler() with the index rebased to 0 here (Plan 11).
#define PSSI_INDEX_DIN       0
#define PSSI_INDEX_RELAY     1
#define PSSI_INDEX_IOTYPES   2
#define PSSI_INDEX_IOBINDS   3
#define PSSI_INDEX_INNAMES   4
#define PSSI_INDEX_OUTNAMES  5
#define PSSI_INDEX_INSTATES  6
#define PSSI_INDEX_OUTSTATES 7
#define PSSI_INDEX_OUTMODES  8
#define PSSI_INDEX_OUTTMO    9
#define PSSI_INDEX_SHUTTERS  10
#define PSSI_INDEX_SHNAMES   11
#define PSSI_INDEX_RMNAMES   12
#define PSSI_INDEX_OUTROOMS  13
#define PSSI_INDEX_SHROOMS   14
#define PSSI_INDEX_NLOC      15

static const char *g_pcProductSSITags[] =
{
    "mqdin",         // PSSI_INDEX_DIN       — SN65HVS882 input device count
    "mqrelay",       // PSSI_INDEX_RELAY     — relay device count
    "iotypes",       // PSSI_INDEX_IOTYPES   — per-input type bitmask (hex)
    "iobinds",       // PSSI_INDEX_IOBINDS   — input->output binding table (hex)
    "innames",       // PSSI_INDEX_INNAMES   — packed input names (12 B each)
    "outnames",      // PSSI_INDEX_OUTNAMES  — packed output names (12 B each)
    "instates",      // PSSI_INDEX_INSTATES  — live input states as hex bytes
    "outstats",      // PSSI_INDEX_OUTSTATES — live relay states as hex bytes
    "outmodes",      // PSSI_INDEX_OUTMODES  — per-output mode hex (1 char each)
    "outtmo",        // PSSI_INDEX_OUTTMO    — timed durations, comma list
    "shutters",      // PSSI_INDEX_SHUTTERS  — packed "up:down:travel;" list
    "shnames",       // PSSI_INDEX_SHNAMES   — packed shutter names (12 B each)
    "rmnames",       // PSSI_INDEX_RMNAMES   — packed room names (12 B each)
    "outrooms",      // PSSI_INDEX_OUTROOMS  — room index per output (comma list)
    "shrooms",       // PSSI_INDEX_SHROOMS   — room index per defined shutter
    "nloc"           // PSSI_INDEX_NLOC      — count of platform-local inputs
};
#define NUM_PRODUCT_SSI_TAGS    (sizeof(g_pcProductSSITags) / sizeof(char *))

//*****************************************************************************
//
// product_web_register - Plan 11 product hook.  Hands the product's web tables
// to the foundation httpd (called once from WebUIRegister()).  A no-op product
// would zero *reg; here the home-auto product supplies its CGI handlers and SSI
// tags.
//
//*****************************************************************************
void
product_web_register(product_web_reg_t *reg)
{
    reg->cgis         = g_psProductCGIURIs;
    reg->num_cgis     = (int)NUM_PRODUCT_CGI_URIS;
    reg->ssi_tags     = g_pcProductSSITags;
    reg->num_ssi_tags = (int)NUM_PRODUCT_SSI_TAGS;
}

//*****************************************************************************
//
// product_ssi_handler - Plan 11 product hook.  Renders the product's SSI tags,
// keyed by the PRODUCT-RELATIVE index (0 == the product's first tag).  Bodies
// were moved verbatim out of the foundation SSIHandler; the only change is the
// switch key (product_tag_index / PSSI_INDEX_*).
//
//*****************************************************************************
u16_t
product_ssi_handler(int product_tag_index, char *pcInsert, int iInsertLen)
{
    switch(product_tag_index)
    {
        case PSSI_INDEX_DIN:
            usnprintf(pcInsert, iInsertLen, "%d", ConfigGetDinDevices());
            break;

        case PSSI_INDEX_RELAY:
            usnprintf(pcInsert, iInsertLen, "%d", ConfigGetRelayDevices());
            break;

        case PSSI_INDEX_NLOC:
            //
            // Count of platform-local inputs appended after the SPI chain (the
            // CC35x1 on-board buttons; 0 on the TM4C).  The I/O config page adds
            // this to D*8 to size its Inputs table.
            //
            usnprintf(pcInsert, iInsertLen, "%d", WebPlatformLocalInputCount());
            break;

        case PSSI_INDEX_IOTYPES:
        {
            //
            // Emit one byte (2 hex chars) per input byte, LSB = input 0 of that
            // byte.  Covers the whole logical input space (SPI chain + the
            // appended platform-local inputs), so the button byte is included.
            // The I/O config page uses this to seed each Type select.
            //
            static const char pcHex[] = "0123456789abcdef";
            int iBytes = ((int)IOInputCount() + 7) / 8;
            int iByte, iBit;
            int iPos = 0;
            for(iByte = 0; (iByte < iBytes) && ((iPos + 2) < iInsertLen);
                iByte++)
            {
                uint8_t ui8Val = 0;
                for(iBit = 0; iBit < 8; iBit++)
                {
                    if(ConfigInputIsPushbutton(iByte * 8 + iBit))
                    {
                        ui8Val |= (uint8_t)(1u << iBit);
                    }
                }
                pcInsert[iPos++] = pcHex[(ui8Val >> 4) & 0xF];
                pcInsert[iPos++] = pcHex[ui8Val & 0xF];
            }
            pcInsert[iPos] = '\0';
            break;
        }

        case PSSI_INDEX_IOBINDS:
        {
            //
            // Emit up to D*8 inputs x 4 slots x 3 hex chars (12 B/input).  The
            // loop guard below caps it at the SSI insert buffer
            // (LWIP_HTTPD_MAX_TAG_INSERT_LEN, 800 B = 66 inputs).  Per slot:
            // 12-bit value = (output<<5)|(action<<3)|trigger as "XYZ";
            // "000" = unused slot.
            //
            static const char pcHexB[] = "0123456789abcdef";
            int iMaxIn = (int)IOInputCount();
            int iInput, iSlot, iPos = 0;
            if(iMaxIn > CFG_MAX_INPUTS) { iMaxIn = CFG_MAX_INPUTS; }
            for(iInput = 0; (iInput < iMaxIn) && ((iPos + 3) < iInsertLen);
                iInput++)
            {
                for(iSlot = 0; (iSlot < CFG_BIND_SLOTS) &&
                               ((iPos + 3) < iInsertLen); iSlot++)
                {
                    uint8_t ta  = ConfigBindingGetTrigAct(iInput, iSlot);
                    uint8_t out = ConfigBindingGetOutput(iInput, iSlot);
                    uint16_t v  = 0;
                    if(((ta & 0x07u) != 0) && (out != BIND_OUTPUT_NONE))
                    {
                        v = (uint16_t)(((uint16_t)out << 5) | (ta & 0x1Fu));
                    }
                    pcInsert[iPos++] = pcHexB[(v >> 8) & 0xFu];
                    pcInsert[iPos++] = pcHexB[(v >> 4) & 0xFu];
                    pcInsert[iPos++] = pcHexB[v & 0xFu];
                }
            }
            pcInsert[iPos] = '\0';
            break;
        }

        case PSSI_INDEX_INNAMES:
        case PSSI_INDEX_OUTNAMES:
        {
            //
            // Emit one CFG_NAME_LEN (12) char block per channel, space-padded,
            // no NUL separators so JS can index by i*12.  Bounded by the names
            // record capacity (64) and by the SSI insert buffer
            // (LWIP_HTTPD_MAX_TAG_INSERT_LEN, 800 B = up to 66 blocks) via the
            // loop guard below.  Previously hard-capped at 16, so output/input
            // names past #16 never reached the browser.
            //
            bool bIn = (product_tag_index == PSSI_INDEX_INNAMES);
            int iMax = bIn ? CFG_NAMES_MAX_INPUTS : CFG_NAMES_MAX_OUTPUTS;
            int iCount = bIn ? (int)IOInputCount()
                             : (int)ConfigGetRelayDevices() * 8;
            int i, j, iPos = 0;
            if(iCount > iMax) { iCount = iMax; }
            for(i = 0; i < iCount && (iPos + CFG_NAME_LEN) <= iInsertLen; i++)
            {
                const char *pcN = bIn ? ConfigGetInputName(i)
                                      : ConfigGetOutputName(i);
                for(j = 0; j < CFG_NAME_LEN; j++)
                {
                    pcInsert[iPos++] = (j < CFG_NAME_LEN - 1 && pcN[j])
                                       ? pcN[j] : ' ';
                }
            }
            pcInsert[iPos] = '\0';
            break;
        }

        case PSSI_INDEX_INSTATES:
        case PSSI_INDEX_OUTSTATES:
        {
            //
            // Emit one hex byte per configured device (8 inputs or relays
            // per byte).  Bit b of byte d = channel d*8+b is active/ON.
            //
            static const char pcH[] = "0123456789abcdef";
            bool bIn   = (product_tag_index == PSSI_INDEX_INSTATES);
            int  nDev  = bIn ? (((int)IOInputCount() + 7) / 8)
                              : (int)ConfigGetRelayDevices();
            int  iPos = 0, d, b;
            if(nDev > 8) { nDev = 8; }  // cap: 8 devices = 64 channels
            for(d = 0; d < nDev && iPos + 2 <= iInsertLen; d++)
            {
                uint8_t ui8B = 0;
                for(b = 0; b < 8; b++)
                {
                    if(bIn)
                    {
                        if(g_pui8LiveInState[d] & (1u << b)) { ui8B |= (1u << b); }
                    }
                    else
                    {
                        if(RelayChainGet((uint16_t)(d * 8 + b))) { ui8B |= (1u << b); }
                    }
                }
                pcInsert[iPos++] = pcH[ui8B >> 4];
                pcInsert[iPos++] = pcH[ui8B & 0xFu];
            }
            pcInsert[iPos] = '\0';
            break;
        }

        case PSSI_INDEX_OUTMODES:
        {
            //
            // One character per output: '1' = Timed, '0' = Standard (up to 16).
            //
            int n = (int)ConfigGetRelayDevices() * 8, i, iPos = 0;
            if(n > 16) { n = 16; }
            for(i = 0; (i < n) && ((iPos + 1) < iInsertLen); i++)
            {
                pcInsert[iPos++] = (ConfigOutMode(i) == OUT_MODE_TIMED) ? '1' : '0';
            }
            pcInsert[iPos] = '\0';
            break;
        }

        case PSSI_INDEX_OUTTMO:
        {
            //
            // Comma-separated timed auto-OFF durations (ms), one per output.
            //
            int n = (int)ConfigGetRelayDevices() * 8, i, iPos = 0;
            if(n > 16) { n = 16; }
            for(i = 0; i < n; i++)
            {
                char tmp[12];
                int  L;
                usnprintf(tmp, sizeof(tmp), "%u", ConfigOutTimedMs(i));
                L = (int)strlen(tmp);
                if((iPos + L + 2) >= iInsertLen) { break; }
                if(i > 0) { pcInsert[iPos++] = ','; }
                memcpy(pcInsert + iPos, tmp, L);
                iPos += L;
            }
            pcInsert[iPos] = '\0';
            break;
        }

        case PSSI_INDEX_SHUTTERS:
        {
            //
            // Configured shutters as "up:down:travel;" entries.
            //
            int i, iPos = 0;
            for(i = 0; i < CFG_MAX_SHUTTERS; i++)
            {
                uint8_t  up, down;
                uint32_t travel;
                char     tmp[24];
                int      L;
                if(!ConfigShutterGet(i, &up, &down, &travel)) { continue; }
                usnprintf(tmp, sizeof(tmp), "%d:%d:%u;", up, down, travel);
                L = (int)strlen(tmp);
                if((iPos + L) >= iInsertLen) { break; }
                memcpy(pcInsert + iPos, tmp, L);
                iPos += L;
            }
            pcInsert[iPos] = '\0';
            break;
        }

        case PSSI_INDEX_SHNAMES:
        {
            //
            // One CFG_NAME_LEN (12) char space-padded block per DEFINED shutter,
            // in slot order - same skip condition and ordering as PSSI_INDEX_SHUTTERS
            // so block n aligns with shutter entry n on the client.  Up to
            // 32 x 12 = 384 B, within the SSI insert buffer.
            //
            int i, j, iPos = 0;
            for(i = 0; i < CFG_MAX_SHUTTERS &&
                       (iPos + CFG_NAME_LEN) <= iInsertLen; i++)
            {
                uint8_t  up, down;
                uint32_t travel;
                const char *pcN;
                if(!ConfigShutterGet(i, &up, &down, &travel)) { continue; }
                pcN = ConfigShutterName(i);
                for(j = 0; j < CFG_NAME_LEN; j++)
                {
                    pcInsert[iPos++] = (j < CFG_NAME_LEN - 1 && pcN[j])
                                       ? pcN[j] : ' ';
                }
            }
            pcInsert[iPos] = '\0';
            break;
        }

        case PSSI_INDEX_RMNAMES:
        {
            //
            // 12-char space-padded block for EVERY room (0..CFG_MAX_ROOMS-1) so
            // the client can index by room number.  16 x 12 = 192 B.
            //
            int i, j, iPos = 0;
            for(i = 0; i < CFG_MAX_ROOMS &&
                       (iPos + CFG_NAME_LEN) <= iInsertLen; i++)
            {
                const char *pcN = ConfigRoomName(i);
                for(j = 0; j < CFG_NAME_LEN; j++)
                {
                    pcInsert[iPos++] = (j < CFG_NAME_LEN - 1 && pcN[j])
                                       ? pcN[j] : ' ';
                }
            }
            pcInsert[iPos] = '\0';
            break;
        }

        case PSSI_INDEX_OUTROOMS:
        {
            //
            // Comma-separated room index per output (ROOM_NONE = 255).
            //
            int i, iCount = (int)ConfigGetRelayDevices() * 8;
            int iPos = 0;
            char tmp[8];
            for(i = 0; i < iCount; i++)
            {
                int L;
                usnprintf(tmp, sizeof(tmp), (i == 0) ? "%d" : ",%d",
                          (int)ConfigOutRoom(i));
                L = (int)strlen(tmp);
                if((iPos + L) >= iInsertLen) { break; }
                memcpy(pcInsert + iPos, tmp, L);
                iPos += L;
            }
            pcInsert[iPos] = '\0';
            break;
        }

        case PSSI_INDEX_SHROOMS:
        {
            //
            // Comma-separated room index per DEFINED shutter, slot order (same
            // skip as PSSI_INDEX_SHUTTERS so entry n aligns with client SH[n]).
            //
            int i, iPos = 0, iEmit = 0;
            char tmp[8];
            for(i = 0; i < CFG_MAX_SHUTTERS; i++)
            {
                uint8_t  up, down;
                uint32_t travel;
                int      L;
                if(!ConfigShutterGet(i, &up, &down, &travel)) { continue; }
                usnprintf(tmp, sizeof(tmp), (iEmit == 0) ? "%d" : ",%d",
                          (int)ConfigShRoom(i));
                L = (int)strlen(tmp);
                if((iPos + L) >= iInsertLen) { break; }
                memcpy(pcInsert + iPos, tmp, L);
                iPos += L;
                iEmit++;
            }
            pcInsert[iPos] = '\0';
            break;
        }

        default:
            pcInsert[0] = '\0';
            break;
    }

    return((u16_t)strlen(pcInsert));
}
