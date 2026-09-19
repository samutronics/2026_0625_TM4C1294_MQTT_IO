//*****************************************************************************
//
// webui.c - config web UI: SSI + CGI handlers for the field-I/O gateway pages.
//
// These handlers and their SSI/CGI tables were extracted verbatim from the TM4C
// enet_io.c so both the TM4C (TivaWare lwIP 1.4.1) and CC35x1 (SimpleLink SDK
// lwIP 2.1.3) builds serve identical pages from one source.  The two httpd
// versions share the tCGIHandler/tSSIHandler typedefs, so the bodies are
// unchanged; the only portability shims are the three #defines below mapping
// TivaWare's ustdlib/UARTStdio calls onto the PAL (identical signatures), and
// the platform seams declared in webui.h (OTA chunk handler, the reset/MQTT
// request flags, g_ui32IPAddress and the live input snapshot).
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

// Foundation<->product contract (Plan 11).  product_web.h is included after
// httpd.h so the web hook types (tCGI) resolve; it pulls in product_api.h.
// Only prototypes today, wired as the cleave lands.
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
//
#define UARTprintf   PalLog
#define usnprintf    PalSnprintf
#define ustrtoul     PalStrToUl
#define ustrncpy     strncpy       // TivaWare ustrncpy == C strncpy (copy n, NUL-pad)
#define ustrlen      strlen        // TivaWare ustrlen  == C strlen

// ---- Foundation SSI tag indices (base tabs: Status/Settings/Wi-Fi/OTA) ----
// The product's SSI tags (I/O Config + Control) are appended AFTER these by
// product_web_register(); their product-relative indices are the PSSI_INDEX_*
// values in the PRODUCT section at the bottom of this file (Plan 11).
#define SSI_INDEX_HOST      0
#define SSI_INDEX_PORT      1
#define SSI_INDEX_CLIENT    2
#define SSI_INDEX_USER      3
#define SSI_INDEX_TOPIC     4
#define SSI_INDEX_AUTH      5
#define SSI_INDEX_STATUS    6
#define SSI_INDEX_IP        7
#define SSI_INDEX_FWVER     8
#define SSI_INDEX_NTPTIME   9
#define SSI_INDEX_NTPSVR    10
#define SSI_INDEX_NTPTZ     11
#define SSI_INDEX_MQPASS    12
#define SSI_INDEX_OTAMAX    13
#define SSI_INDEX_OTAPOST   14
#define SSI_INDEX_OTAERROR  15
#define SSI_INDEX_WIFIOPTS  16
#define SSI_INDEX_WIFITAB   17
#define SSI_INDEX_TEMP      18
#define SSI_INDEX_SHOWREBOOT 19
#define SSI_INDEX_WSSID1    20
#define SSI_INDEX_WPASS1    21
#define SSI_INDEX_WSSID2    22
#define SSI_INDEX_WPASS2    23

static const char *g_pcConfigSSITags[] =
{
    "mqhost",        // SSI_INDEX_HOST
    "mqport",        // SSI_INDEX_PORT
    "mqclient",      // SSI_INDEX_CLIENT
    "mquser",        // SSI_INDEX_USER
    "mqtopic",       // SSI_INDEX_TOPIC
    "mqauth",        // SSI_INDEX_AUTH
    "mqstatus",      // SSI_INDEX_STATUS
    "ipaddr",        // SSI_INDEX_IP
    "fwver",         // SSI_INDEX_FWVER  — build timestamp YYYYMMDDHHMM
    "ntptime",       // SSI_INDEX_NTPTIME — current time HH:MM:SS
    "ntpsvr",        // SSI_INDEX_NTPSVR  — NTP server hostname
    "ntptz",         // SSI_INDEX_NTPTZ   — UTC offset (signed integer)
    "mqpass",        // SSI_INDEX_MQPASS  — MQTT password (for backup page)
    "otamax",        // SSI_INDEX_OTAMAX    — max OTA image size (bytes), per platform
    "otapost",       // SSI_INDEX_OTAPOST   — 1: upload via streaming POST, 0: hex-GET
    "otaerror",      // SSI_INDEX_OTAERROR  — last OTA error message (CC35x1), empty string if none
    "wifiopts",      // SSI_INDEX_WIFIOPTS  — setup-page SSID dropdown <option>s (CC35x1)
    "wifitab",       // SSI_INDEX_WIFITAB   — Settings MQTT/Wi-Fi sub-tab bar (CC35x1 only)
    "temp",          // SSI_INDEX_TEMP      — on-board temperature reading (CC35x1)
    "showreboot",    // SSI_INDEX_SHOWREBOOT — 1 (TM4C), 0 (CC35x1 - reboot disabled due to NWP wedge)
    "wssid1",        // SSI_INDEX_WSSID1 — saved primary SSID prefill (CC35x1)
    "wpass1",        // SSI_INDEX_WPASS1 — saved primary passphrase prefill (CC35x1)
    "wssid2",        // SSI_INDEX_WSSID2 — saved backup SSID prefill (CC35x1)
    "wpass2"         // SSI_INDEX_WPASS2 — saved backup passphrase prefill (CC35x1)
};

//*****************************************************************************
//
// The number of individual SSI tags that the HTTPD server can expect to
// find in our configuration pages.
//
//*****************************************************************************
#define NUM_CONFIG_SSI_TAGS     (sizeof(g_pcConfigSSITags) / sizeof (char *))

// Upper bound on product SSI tags, for the combined-table scratch buffer in
// WebUIRegister().  The product currently registers 16.
#define WEBUI_MAX_PRODUCT_SSI   24

// ---- CGI handler prototypes (OTA handler is the platform seam) ----
static char *MQTTConfigCGIHandler(int32_t iIndex, int32_t i32NumParams,
                                  char *pcParam[], char *pcValue[]);
static char *FactoryResetCGIHandler(int32_t iIndex, int32_t i32NumParams,
                                    char *pcParam[], char *pcValue[]);
static char *NtpCfgCGIHandler(int32_t iIndex, int32_t i32NumParams,
                               char *pcParam[], char *pcValue[]);
static char *CfgRestoreCGIHandler(int32_t iIndex, int32_t i32NumParams,
                                  char *pcParam[], char *pcValue[]);
static char *RebootCGIHandler(int32_t iIndex, int32_t i32NumParams,
                               char *pcParam[], char *pcValue[]);

//*****************************************************************************
//
// Prototype for the main handler used to process server-side-includes for the
// application's web-based configuration screens.
//
//*****************************************************************************
static int32_t SSIHandler(int32_t iIndex, char *pcInsert, int32_t iInsertLen);

static char *WifiCfgCGIHandler(int32_t iIndex, int32_t i32NumParams,
                               char *pcParam[], char *pcValue[]);
static char *WifiForgetCGIHandler(int32_t iIndex, int32_t i32NumParams,
                                  char *pcParam[], char *pcValue[]);

#define CGI_INDEX_MQTTCFG       0
#define CGI_INDEX_IOCFG         1
#define CGI_INDEX_FWCHUNK       2
#define CGI_INDEX_FACTORYRESET  3
#define CGI_INDEX_NTPCFG        4
#define CGI_INDEX_CFGRESTORE    5
#define CGI_INDEX_RELAYPULSE    6
#define CGI_INDEX_REBOOT        7
#define CGI_INDEX_NAMESET       8
#define CGI_INDEX_OUTCFG        9
#define CGI_INDEX_COVER         10
#define CGI_INDEX_RELAYSET      11
#define CGI_INDEX_ROOMCFG       12
#define CGI_INDEX_WIFICFG       13
#define CGI_INDEX_WIFIFORGET    14

// Foundation CGI handlers (base tabs: Settings/OTA/Wi-Fi + factory/reboot).  The
// product's CGI handlers (/iocfg, /relaypulse, /nameset, /outcfg, /cover,
// /relayset, /roomcfg) live in the PRODUCT section at the bottom of this file
// and reach the httpd via product_web_register() (Plan 11).
static const tCGI g_psConfigCGIURIs[] =
{
    { "/mqttcfg.cgi",      (tCGIHandler)MQTTConfigCGIHandler   }, // CGI_INDEX_MQTTCFG
    { "/fwchunk.cgi",      (tCGIHandler)WebPlatformOtaChunkCGI }, // CGI_INDEX_FWCHUNK (platform seam)
    { "/factoryreset.cgi", (tCGIHandler)FactoryResetCGIHandler }, // CGI_INDEX_FACTORYRESET
    { "/ntpcfg.cgi",       (tCGIHandler)NtpCfgCGIHandler       }, // CGI_INDEX_NTPCFG
    { "/cfgrestore.cgi",   (tCGIHandler)CfgRestoreCGIHandler   }, // CGI_INDEX_CFGRESTORE
#ifndef CC35XX
    { "/reboot.cgi",       (tCGIHandler)RebootCGIHandler       }, // CGI_INDEX_REBOOT (TM4C only)
#endif
    { "/wificfg.cgi",      (tCGIHandler)WifiCfgCGIHandler      }, // CGI_INDEX_WIFICFG
    { "/wififorget.cgi",   (tCGIHandler)WifiForgetCGIHandler   }  // CGI_INDEX_WIFIFORGET
};

//*****************************************************************************
//
// The number of foundation CGI URIs.  The product appends its own via
// product_web_register(); WebUIRegister() concatenates the two.
//
//*****************************************************************************
#define NUM_CONFIG_CGI_URIS     (sizeof(g_psConfigCGIURIs) / sizeof(tCGI))

// Upper bound on product CGI handlers, for the combined-table scratch buffer in
// WebUIRegister().  The product currently registers 7.
#define WEBUI_MAX_PRODUCT_CGI   12

#define DEFAULT_CGI_RESPONSE    "/index.shtml"
// IOCFG_CGI_RESPONSE / CONTROL_CGI_RESPONSE moved with the product web handlers
// to products/home_auto/web/product_web.c (Plan 11 Scope C).

//*****************************************************************************
//
// The file sent back to the browser in cases where a parameter error is
// detected by one of the CGI handlers.  This should only happen if someone
// tries to access the CGI directly via the broswer command line and doesn't
// enter all the required parameters alongside the URI.
//
//*****************************************************************************
#define PARAM_ERROR_RESPONSE    "/perror.htm"

#define JAVASCRIPT_HEADER                                                     \
    "<script type='text/javascript' language='JavaScript'><!--\n"
#define JAVASCRIPT_FOOTER                                                     \
    "//--></script>\n"

//*****************************************************************************
//
// Web -> main-loop request state, plus values the SSI handler renders that each
// platform keeps current (the raw IPv4 word and the live input snapshot).
//
//*****************************************************************************
uint32_t g_ui32IPAddress;
uint8_t  g_pui8LiveInState[IO_MAX_BYTES];

static volatile bool g_bStartMQTT;
static volatile bool g_bRepublishMQTT;
static volatile bool g_bOTAReset;

void WebUIRequestReset(void)         { g_bOTAReset = true; }
bool WebUIResetPending(void)         { bool b = g_bOTAReset;     g_bOTAReset = false;     return(b); }
void WebUIRequestMqttApply(void)     { g_bStartMQTT = true; }
bool WebUIMqttApplyPending(void)     { bool b = g_bStartMQTT;    g_bStartMQTT = false;    return(b); }
void WebUIRequestMqttRepublish(void) { g_bRepublishMQTT = true; }
bool WebUIMqttRepublishPending(void) { bool b = g_bRepublishMQTT; g_bRepublishMQTT = false; return(b); }

//
// Wi-Fi provisioning request state (CC35x1 SoftAP flow).  Buffers sized locally:
// webui.c is shared and cannot include the CC35x1 wifi_store.h.  32 SSID octets
// + NUL, 63 WPA2 passphrase chars + NUL.
//
#define WEBUI_WIFI_SSID_LEN  33
#define WEBUI_WIFI_PASS_LEN  64
static volatile bool g_bWifiProvision;
static volatile bool g_bWifiForget;
static volatile int  g_iWifiSlot;    // target credential slot: 0 = primary, 1 = backup
static char          g_pcWifiSsid[WEBUI_WIFI_SSID_LEN];
static char          g_pcWifiPass[WEBUI_WIFI_PASS_LEN];

void
WebUIRequestWifiProvision(int iSlot, const char *pcSsid, const char *pcPass)
{
    g_iWifiSlot = (iSlot == 1) ? 1 : 0;
    strncpy(g_pcWifiSsid, (pcSsid != NULL) ? pcSsid : "", WEBUI_WIFI_SSID_LEN - 1);
    g_pcWifiSsid[WEBUI_WIFI_SSID_LEN - 1] = '\0';
    strncpy(g_pcWifiPass, (pcPass != NULL) ? pcPass : "", WEBUI_WIFI_PASS_LEN - 1);
    g_pcWifiPass[WEBUI_WIFI_PASS_LEN - 1] = '\0';
    g_bWifiProvision = true;
}

bool
WebUIWifiProvisionPending(int *piSlot, char *pcSsid, int iSsidLen,
                          char *pcPass, int iPassLen)
{
    if(!g_bWifiProvision)
    {
        return(false);
    }
    if(piSlot != NULL)
    {
        *piSlot = g_iWifiSlot;
    }
    if((pcSsid != NULL) && (iSsidLen > 0))
    {
        strncpy(pcSsid, g_pcWifiSsid, (size_t)iSsidLen - 1);
        pcSsid[iSsidLen - 1] = '\0';
    }
    if((pcPass != NULL) && (iPassLen > 0))
    {
        strncpy(pcPass, g_pcWifiPass, (size_t)iPassLen - 1);
        pcPass[iPassLen - 1] = '\0';
    }
    g_bWifiProvision = false;
    return(true);
}

void WebUIRequestWifiForget(void) { g_bWifiForget = true; }
bool WebUIWifiForgetPending(void) { bool b = g_bWifiForget; g_bWifiForget = false; return(b); }

// ---- handlers block 1: GetStringParam, MQTTConfig, HexNibble, IOConfig ----
//*****************************************************************************
//
// Helper: copy a (possibly URL-encoded) CGI parameter into a fixed buffer.
//
//*****************************************************************************
static void
GetStringParam(const char *pcName, char *pcParam[], char *pcValue[],
               int32_t i32NumParams, char *pcDest, int32_t i32DestLen)
{
    int32_t i32Idx = FindCGIParameter(pcName, pcParam, i32NumParams);
    if(i32Idx != -1)
    {
        DecodeFormString(pcValue[i32Idx], pcDest, i32DestLen);
    }
}

//*****************************************************************************
//
// CGI handler for /mqttcfg.cgi.  Parses the configuration form, stores the
// new settings in EEPROM and requests a reconnect.
//
//*****************************************************************************
static char *
MQTTConfigCGIHandler(int32_t iIndex, int32_t i32NumParams, char *pcParam[],
                     char *pcValue[])
{
    tMQTTConfig *psCfg = ConfigGet();
    int32_t i32Idx;

    (void)iIndex;

    //
    // String fields.  Empty values are accepted (e.g. to clear the host).
    //
    psCfg->pcHost[0] = '\0';
    GetStringParam("host", pcParam, pcValue, i32NumParams, psCfg->pcHost,
                   CFG_HOST_LEN);
    GetStringParam("client", pcParam, pcValue, i32NumParams, psCfg->pcClientID,
                   CFG_CLIENTID_LEN);
    GetStringParam("user", pcParam, pcValue, i32NumParams, psCfg->pcUser,
                   CFG_USER_LEN);
    GetStringParam("topic", pcParam, pcValue, i32NumParams, psCfg->pcTopicBase,
                   CFG_TOPIC_LEN);

    //
    // Password: only overwrite the stored value when a non-empty value is
    // submitted (the form never echoes the current password back).
    //
    i32Idx = FindCGIParameter("pass", pcParam, i32NumParams);
    if((i32Idx != -1) && (pcValue[i32Idx][0] != '\0'))
    {
        DecodeFormString(pcValue[i32Idx], psCfg->pcPass, CFG_PASS_LEN);
    }

    //
    // Port number.
    //
    i32Idx = FindCGIParameter("port", pcParam, i32NumParams);
    if(i32Idx != -1)
    {
        uint32_t ui32Port = ustrtoul(pcValue[i32Idx], 0, 10);
        psCfg->ui16Port = (ui32Port && (ui32Port <= 65535)) ?
                          (uint16_t)ui32Port : 1883;
    }

    //
    // Authentication checkbox (present in the form only when ticked).
    //
    psCfg->ui8UseAuth =
        (FindCGIParameter("auth", pcParam, i32NumParams) != -1) ? 1 : 0;

    //
    // Persist and ask the main loop to apply the new settings.
    // Release the tcpip_thread lock around config save so PalLog can run safely.
    //
    UNLOCK_TCPIP_CORE();
    ConfigSave();
    LOCK_TCPIP_CORE();
    g_bStartMQTT = true;

    return(DEFAULT_CGI_RESPONSE);
}

//*****************************************************************************
//
// CGI handler for /wificfg.cgi (CC35x1 SoftAP provisioning).  Reads the SSID and
// passphrase entered on the setup page and raises the provisioning request; the
// main tick persists them and switches from the setup AP to station mode.  The
// actual switch is deferred to the tick so this response flushes first (the AP -
// and this connection - disappears once the device joins the target network).
//
//*****************************************************************************
static char *
WifiCfgCGIHandler(int32_t iIndex, int32_t i32NumParams, char *pcParam[],
                  char *pcValue[])
{
    char pcSsid[WEBUI_WIFI_SSID_LEN];
    char pcPass[WEBUI_WIFI_PASS_LEN];
    char pcSlot[4];
    int  iSlot = 0;

    (void)iIndex;

    pcSsid[0] = '\0';
    pcPass[0] = '\0';
    pcSlot[0] = '\0';
    GetStringParam("ssid", pcParam, pcValue, i32NumParams, pcSsid,
                   WEBUI_WIFI_SSID_LEN);
    GetStringParam("pass", pcParam, pcValue, i32NumParams, pcPass,
                   WEBUI_WIFI_PASS_LEN);

    //
    // Optional "slot" param selects the credential slot: absent or "0" = primary
    // (triggers the live AP->STA switch), "1" = backup (saved only, no switch).
    //
    GetStringParam("slot", pcParam, pcValue, i32NumParams, pcSlot,
                   (int32_t)sizeof(pcSlot));
    if(pcSlot[0] == '1')
    {
        iSlot = 1;
    }

    if(pcSsid[0] == '\0')
    {
        return(PARAM_ERROR_RESPONSE);
    }

    WebUIRequestWifiProvision(iSlot, pcSsid, pcPass);
    return("/wifi_ok.html");
}

//*****************************************************************************
//
// CGI handler for /wififorget.cgi (CC35x1).  Clears the stored credentials and
// returns the device to the setup AP so it can be re-provisioned.  Deferred to
// the tick for the same flush-first reason as /wificfg.cgi.
//
//*****************************************************************************
static char *
WifiForgetCGIHandler(int32_t iIndex, int32_t i32NumParams, char *pcParam[],
                     char *pcValue[])
{
    (void)iIndex;
    (void)i32NumParams;
    (void)pcParam;
    (void)pcValue;

    WebUIRequestWifiForget();
    return("/wifi_forget.html");
}

//*****************************************************************************
//
// Convert one ASCII hex character to its 4-bit value.
//
//*****************************************************************************
static uint8_t
HexNibble(char c)
{
    if((c >= '0') && (c <= '9'))
    {
        return((uint8_t)(c - '0'));
    }
    if((c >= 'a') && (c <= 'f'))
    {
        return((uint8_t)(c - 'a' + 10));
    }
    if((c >= 'A') && (c <= 'F'))
    {
        return((uint8_t)(c - 'A' + 10));
    }
    return(0);
}



// ---- handlers block 2: FactoryReset, Reboot, UrlDecodeParam, NameSet ----
//*****************************************************************************
//
// FactoryResetCGIHandler - Web-triggered factory reset.
//
// Invalidates all EEPROM records, then schedules a system reset via the same
// g_bOTAReset flag used by the OTA handler so the HTTP response is sent first.
//
//*****************************************************************************
static char *
FactoryResetCGIHandler(int32_t iIndex, int32_t i32NumParams,
                       char *pcParam[], char *pcValue[])
{
    UARTprintf("Factory reset triggered via web UI.\n");
    MQTTAppStop();
    ConfigFactoryReset();
    g_bOTAReset = true;
    return("/factoryreset_ok.shtml");
}

//*****************************************************************************
//
// RebootCGIHandler - Graceful reboot from the web UI.
//
//*****************************************************************************
static char *
RebootCGIHandler(int32_t iIndex, int32_t i32NumParams,
                 char *pcParam[], char *pcValue[])
{
    UARTprintf("Reboot triggered via web UI.\n");
    MQTTAppStop();
    g_bOTAReset = true;
    return("/fwupdate_ok.shtml");
}



// ---- handlers block 3: NtpCfg, CfgJson*, CfgRestore, RelayPulse, OutCfg, Cover, RelaySet, RoomCfg, SSIHandler ----
//*****************************************************************************
//
// NtpCfgCGIHandler - Save NTP server + TZ offset then restart SNTP.
//
//*****************************************************************************
static char *
NtpCfgCGIHandler(int32_t iIndex, int32_t i32NumParams,
                 char *pcParam[], char *pcValue[])
{
    int32_t iIdx;
    char    acServer[CFG_NTP_SERVER_LEN];
    int32_t i32Tz = 0;

    iIdx = FindCGIParameter("ntp", pcParam, i32NumParams);
    if(iIdx >= 0)
    {
        GetStringParam("ntp", pcParam, pcValue, i32NumParams,
                       acServer, CFG_NTP_SERVER_LEN);
        ConfigNtpSetServer(acServer);
    }

    iIdx = FindCGIParameter("tz", pcParam, i32NumParams);
    if(iIdx >= 0)
    {
        i32Tz = (int32_t)ustrtoul(pcValue[iIdx], NULL, 10);
        if(pcValue[iIdx][0] == '-') { i32Tz = -i32Tz; }
        if(i32Tz < -12) { i32Tz = -12; }
        if(i32Tz >  14) { i32Tz =  14; }
        ConfigNtpSetTz((int8_t)i32Tz);
    }

    UNLOCK_TCPIP_CORE();
    ConfigNtpSave();
    SntpInit();
    LOCK_TCPIP_CORE();
    UARTprintf("NTP: config updated, re-syncing.\n");
    return(DEFAULT_CGI_RESPONSE);
}

//*****************************************************************************
//
// CfgRestoreCGIHandler - Restore broker + NTP config from a JSON chunk stream.
//
// URL: /cfgrestore.cgi?seq=N&last=0|1&data=HEXHEX...
//
// Hex-decoded bytes accumulate in g_acCfgRestoreBuf.  On last=1 the buffer
// is parsed as JSON and the broker + NTP EEPROM records are updated.
//
//*****************************************************************************
#define CFG_RESTORE_BUF_SIZE 2048

static char    g_acCfgRestoreBuf[CFG_RESTORE_BUF_SIZE];
static uint32_t g_ui32CfgRestoreLen = 0;

//
// Minimal JSON value extractor using strstr.
//
// Skip JSON whitespace (space, tab, CR, LF).
#define SKIP_WS(p) while(*(p)==' '||*(p)=='\t'||*(p)=='\r'||*(p)=='\n'){(p)++;}

static bool
CfgJsonGetStr(const char *pcJson, const char *pcKey,
              char *pcVal, int iMax)
{
    char acSearch[48];
    const char *p;
    int i = 0;
    // Search for "key": — no trailing quote so it matches both "k":"v" and "k": "v"
    usnprintf(acSearch, sizeof(acSearch), "\"%s\":", pcKey);
    p = strstr(pcJson, acSearch);
    if(!p) { return(false); }
    p += ustrlen(acSearch);
    SKIP_WS(p);
    if(*p != '"') { return(false); }
    p++;  // skip opening quote
    while(*p && *p != '"' && i < iMax - 1) { pcVal[i++] = *p++; }
    pcVal[i] = '\0';
    return(true);
}

static int32_t
CfgJsonGetInt(const char *pcJson, const char *pcKey)
{
    char acSearch[48];
    const char *p;
    usnprintf(acSearch, sizeof(acSearch), "\"%s\":", pcKey);
    p = strstr(pcJson, acSearch);
    if(!p) { return(-1); }
    p += ustrlen(acSearch);
    SKIP_WS(p);
    if(*p == '-') { return(-(int32_t)ustrtoul(p + 1, NULL, 10)); }
    return((int32_t)ustrtoul(p, NULL, 10));
}

static char *
CfgRestoreCGIHandler(int32_t iIndex, int32_t i32NumParams,
                     char *pcParam[], char *pcValue[])
{
    int32_t iSeqIdx, iLastIdx, iDataIdx;
    bool    bLast;
    const char *pcData;
    uint32_t ui32DataHexLen, i;

    iSeqIdx  = FindCGIParameter("seq",  pcParam, i32NumParams);
    iLastIdx = FindCGIParameter("last", pcParam, i32NumParams);
    iDataIdx = FindCGIParameter("data", pcParam, i32NumParams);

    if(iSeqIdx < 0 || iDataIdx < 0) { return(DEFAULT_CGI_RESPONSE); }

    bLast  = (iLastIdx >= 0) && (pcValue[iLastIdx][0] == '1');
    pcData = pcValue[iDataIdx];
    ui32DataHexLen = ustrlen(pcData) & ~1u;

    if(ustrtoul(pcValue[iSeqIdx], NULL, 10) == 0)
    {
        g_ui32CfgRestoreLen = 0;
    }

    for(i = 0; i < ui32DataHexLen && g_ui32CfgRestoreLen < CFG_RESTORE_BUF_SIZE - 1u; i += 2)
    {
        g_acCfgRestoreBuf[g_ui32CfgRestoreLen++] =
            (char)((HexNibble(pcData[i]) << 4) | HexNibble(pcData[i + 1]));
    }

    if(!bLast) { return(DEFAULT_CGI_RESPONSE); }

    g_acCfgRestoreBuf[g_ui32CfgRestoreLen] = '\0';
    UARTprintf("CfgRestore: %u bytes JSON received.\n", g_ui32CfgRestoreLen);

    //
    // Parse and apply broker config.
    //
    {
        tMQTTConfig *psCfg = ConfigGet();
        char        acTmp[64];
        int32_t     i32Val;

        if(CfgJsonGetStr(g_acCfgRestoreBuf, "host", acTmp, CFG_HOST_LEN))
        { ustrncpy(psCfg->pcHost, acTmp, CFG_HOST_LEN - 1); }
        if(CfgJsonGetStr(g_acCfgRestoreBuf, "client", acTmp, CFG_CLIENTID_LEN))
        { ustrncpy(psCfg->pcClientID, acTmp, CFG_CLIENTID_LEN - 1); }
        if(CfgJsonGetStr(g_acCfgRestoreBuf, "user", acTmp, CFG_USER_LEN))
        { ustrncpy(psCfg->pcUser, acTmp, CFG_USER_LEN - 1); }
        if(CfgJsonGetStr(g_acCfgRestoreBuf, "pass", acTmp, CFG_PASS_LEN))
        { ustrncpy(psCfg->pcPass, acTmp, CFG_PASS_LEN - 1); }
        if(CfgJsonGetStr(g_acCfgRestoreBuf, "topic", acTmp, CFG_TOPIC_LEN))
        { ustrncpy(psCfg->pcTopicBase, acTmp, CFG_TOPIC_LEN - 1); }
        i32Val = CfgJsonGetInt(g_acCfgRestoreBuf, "port");
        if(i32Val > 0 && i32Val <= 65535) { psCfg->ui16Port = (uint16_t)i32Val; }
        i32Val = CfgJsonGetInt(g_acCfgRestoreBuf, "auth");
        if(i32Val >= 0) { psCfg->ui8UseAuth = (uint8_t)(i32Val != 0); }
        i32Val = CfgJsonGetInt(g_acCfgRestoreBuf, "din");
        if(i32Val >= 0) { ConfigSetDinDevices((uint8_t)i32Val); }
        i32Val = CfgJsonGetInt(g_acCfgRestoreBuf, "relay");
        if(i32Val >= 0) { ConfigSetRelayDevices((uint8_t)i32Val); }
        UNLOCK_TCPIP_CORE();
        ConfigSave();
        LOCK_TCPIP_CORE();
    }

    //
    // Parse and apply NTP config.
    //
    {
        char    acTmp[CFG_NTP_SERVER_LEN];
        int32_t i32Val;
        if(CfgJsonGetStr(g_acCfgRestoreBuf, "ntp", acTmp, CFG_NTP_SERVER_LEN))
        { ConfigNtpSetServer(acTmp); }
        i32Val = CfgJsonGetInt(g_acCfgRestoreBuf, "tz");
        if(i32Val >= -12 && i32Val <= 14) { ConfigNtpSetTz((int8_t)i32Val); }
        UNLOCK_TCPIP_CORE();
        ConfigNtpSave();
        LOCK_TCPIP_CORE();
    }

    //
    // Restore per-input types (Switch / Pushbutton) from hex string.
    // Format: 2 hex chars per SN65HVS882 device (1 bit per input channel).
    //
    {
        char acTypes[64];
        if(CfgJsonGetStr(g_acCfgRestoreBuf, "types", acTypes, sizeof(acTypes)))
        {
            const char *p = acTypes;
            int b, bit;
            for(b = 0; b < CFG_DIN_MAX_DEVICES && p[0] && p[1]; b++, p += 2)
            {
                uint8_t ui8Val = (uint8_t)((HexNibble(p[0]) << 4) |
                                            HexNibble(p[1]));
                for(bit = 0; bit < 8; bit++)
                {
                    ConfigSetInputPushbutton(b * 8 + bit,
                                             (ui8Val >> bit) & 1 ? true : false);
                }
            }
            UNLOCK_TCPIP_CORE();
            ConfigIOSave();
            LOCK_TCPIP_CORE();
            UARTprintf("CfgRestore: input types restored.\n");
        }
    }

    //
    // Restore input-to-relay bindings from hex string.
    // Format: 3 hex chars per slot, 4 slots per input, up to 16 inputs.
    // Encoding: bits 11:5 = output, bits 4:3 = action, bits 2:0 = trigger.
    //
    {
        char acBinds[256];
        if(CfgJsonGetStr(g_acCfgRestoreBuf, "binds", acBinds, sizeof(acBinds)))
        {
            const char *p    = acBinds;
            int         iLen = (int)ustrlen(acBinds);
            int         iIn, iSlot;
            int         iMaxIn = (int)ConfigGetDinDevices() * 8;
            if(iMaxIn > 16) { iMaxIn = 16; }
            for(iIn = 0; iIn < iMaxIn; iIn++)
            {
                for(iSlot = 0; iSlot < CFG_BIND_SLOTS; iSlot++)
                {
                    uint16_t v;
                    uint8_t  ui8TrigAct, ui8Out;
                    if((p - acBinds) + 3 > iLen) { goto done_binds; }
                    v = (uint16_t)(((uint16_t)HexNibble(p[0]) << 8) |
                                   ((uint16_t)HexNibble(p[1]) << 4) |
                                    (uint16_t)HexNibble(p[2]));
                    p += 3;
                    //
                    // Low 5 bits of v encode trig (2:0) and act (4:3),
                    // matching the ui8TrigAct field layout in tIOBindings.
                    //
                    ui8TrigAct = (uint8_t)(v & 0x1Fu);
                    ui8Out = ((v & 7u) == 0) ? BIND_OUTPUT_NONE
                                              : (uint8_t)((v >> 5) & 0x7Fu);
                    ConfigBindingSet(iIn, iSlot, ui8TrigAct, ui8Out);
                }
            }
            done_binds:
            UNLOCK_TCPIP_CORE();
            ConfigBindingSave();
            LOCK_TCPIP_CORE();
            UARTprintf("CfgRestore: bindings restored.\n");
        }
    }

    //
    // Restore channel names from packed 12-char-per-slot strings.
    // Trailing spaces are stripped; empty names are skipped (keep existing).
    //
    {
        char acPacked[256];
        int  iMax, i;

        if(CfgJsonGetStr(g_acCfgRestoreBuf, "innames", acPacked, sizeof(acPacked)))
        {
            iMax = (int)ConfigGetDinDevices() * 8;
            if(iMax > CFG_NAMES_MAX_INPUTS) { iMax = CFG_NAMES_MAX_INPUTS; }
            for(i = 0; i < iMax && (i + 1) * CFG_NAME_LEN <= (int)ustrlen(acPacked); i++)
            {
                char acName[CFG_NAME_LEN];
                int  k = CFG_NAME_LEN - 2;
                memcpy(acName, acPacked + i * CFG_NAME_LEN, CFG_NAME_LEN - 1);
                acName[CFG_NAME_LEN - 1] = '\0';
                while(k >= 0 && acName[k] == ' ') { acName[k--] = '\0'; }
                if(acName[0]) { ConfigNameSet(true, i, acName); }
            }
            UARTprintf("CfgRestore: input names restored.\n");
        }

        if(CfgJsonGetStr(g_acCfgRestoreBuf, "outnames", acPacked, sizeof(acPacked)))
        {
            iMax = (int)ConfigGetRelayDevices() * 8;
            if(iMax > CFG_NAMES_MAX_OUTPUTS) { iMax = CFG_NAMES_MAX_OUTPUTS; }
            for(i = 0; i < iMax && (i + 1) * CFG_NAME_LEN <= (int)ustrlen(acPacked); i++)
            {
                char acName[CFG_NAME_LEN];
                int  k = CFG_NAME_LEN - 2;
                memcpy(acName, acPacked + i * CFG_NAME_LEN, CFG_NAME_LEN - 1);
                acName[CFG_NAME_LEN - 1] = '\0';
                while(k >= 0 && acName[k] == ' ') { acName[k--] = '\0'; }
                if(acName[0]) { ConfigNameSet(false, i, acName); }
            }
            UARTprintf("CfgRestore: output names restored.\n");
        }
    }

    UARTprintf("CfgRestore: settings applied. Rebooting...\n");
    g_bOTAReset = true;
    return("/cfgrestore_ok.shtml");
}


//*****************************************************************************
static int32_t
SSIHandler(int32_t iIndex, char *pcInsert, int32_t iInsertLen)
{
    tMQTTConfig *psCfg = ConfigGet();

    //
    // Product SSI tags are registered after the foundation's, so any index at or
    // beyond the foundation tag count belongs to the product (Plan 11).  Forward
    // it to the product handler with a product-relative index.
    //
    if(iIndex >= (int32_t)NUM_CONFIG_SSI_TAGS)
    {
        return((int32_t)product_ssi_handler(
                   (int)(iIndex - (int32_t)NUM_CONFIG_SSI_TAGS),
                   pcInsert, iInsertLen));
    }

    //
    // Which SSI tag have we been passed?  (The password is intentionally never
    // echoed back to the browser.)
    //
    switch(iIndex)
    {
        case SSI_INDEX_HOST:
            usnprintf(pcInsert, iInsertLen, "%s", psCfg->pcHost);
            break;

        case SSI_INDEX_PORT:
            usnprintf(pcInsert, iInsertLen, "%d", psCfg->ui16Port);
            break;

        case SSI_INDEX_CLIENT:
            usnprintf(pcInsert, iInsertLen, "%s", psCfg->pcClientID);
            break;

        case SSI_INDEX_USER:
            usnprintf(pcInsert, iInsertLen, "%s", psCfg->pcUser);
            break;

        case SSI_INDEX_TOPIC:
            usnprintf(pcInsert, iInsertLen, "%s", psCfg->pcTopicBase);
            break;

        case SSI_INDEX_TEMP:
            //
            // On-board temperature reading, rendered by the platform (CC35x1
            // TMP1075; "n/a" on the TM4C).  Also served on its own at /temp.ssi
            // for the status bar's live poll.
            //
            WebPlatformTempStr(pcInsert, iInsertLen);
            break;

        case SSI_INDEX_SHOWREBOOT:
            //
            // Whether the Reboot button should be shown. CC35x1 disables it
            // because a warm SYSRESETREQ wedges the NWP (see cc35x1-web-reboot-freeze).
            // TM4C reboot works fine.
            //
#ifdef CC35XX
            usnprintf(pcInsert, iInsertLen, "0");
#else
            usnprintf(pcInsert, iInsertLen, "1");
#endif
            break;

        case SSI_INDEX_AUTH:
            usnprintf(pcInsert, iInsertLen, "%s",
                      psCfg->ui8UseAuth ? "checked" : "");
            break;

        case SSI_INDEX_STATUS:
            usnprintf(pcInsert, iInsertLen, "%s", MQTTAppStatusStr());
            break;

        case SSI_INDEX_IP:
            usnprintf(pcInsert, iInsertLen, "%d.%d.%d.%d",
                      g_ui32IPAddress & 0xff, (g_ui32IPAddress >> 8) & 0xff,
                      (g_ui32IPAddress >> 16) & 0xff,
                      (g_ui32IPAddress >> 24) & 0xff);
            break;

        case SSI_INDEX_FWVER:
        {
            //
            // Build timestamp formatted as YYYYMMDDHHMM from __DATE__ and
            // __TIME__ (both are compile-time string literals).
            //
            // __DATE__ = "Mon DD YYYY"  e.g. "Jul 11 2026"  (space-padded day)
            // __TIME__ = "HH:MM:SS"     e.g. "09:23:45"
            //
            static const char pcMons[] =
                "JanFebMarAprMayJunJulAugSepOctNovDec";
            const char *pd = g_pcBuildDate;
            const char *pt = g_pcBuildTime;
            int iMon, iDay, iYear, iHour, iMin;

            for(iMon = 0; iMon < 12; iMon++)
            {
                if((pd[0] == pcMons[iMon * 3]) &&
                   (pd[1] == pcMons[iMon * 3 + 1]) &&
                   (pd[2] == pcMons[iMon * 3 + 2]))
                {
                    break;
                }
            }
            iMon++;
            iDay  = ((pd[4] == ' ') ? 0 : (pd[4] - '0')) * 10 + (pd[5] - '0');
            iYear = (pd[7]-'0')*1000 + (pd[8]-'0')*100 +
                    (pd[9]-'0')*10   + (pd[10]-'0');
            iHour = (pt[0]-'0')*10 + (pt[1]-'0');
            iMin  = (pt[3]-'0')*10 + (pt[4]-'0');

            usnprintf(pcInsert, iInsertLen, "%04d%02d%02d%02d%02d",
                      iYear, iMon, iDay, iHour, iMin);
            break;
        }

        case SSI_INDEX_NTPTIME:
            SntpGetTimeStr(pcInsert, iInsertLen);
            break;

        case SSI_INDEX_NTPSVR:
            usnprintf(pcInsert, iInsertLen, "%s",
                      ConfigNtpGet()->pcServer);
            break;

        case SSI_INDEX_NTPTZ:
            usnprintf(pcInsert, iInsertLen, "%d",
                      (int)ConfigNtpGet()->i8TzOffset);
            break;

        case SSI_INDEX_MQPASS:
            usnprintf(pcInsert, iInsertLen, "%s", ConfigGet()->pcPass);
            break;

        case SSI_INDEX_OTAMAX:
            //
            // Largest image the firmware-upload page will offer to send.  The
            // value is platform-specific (TM4C flash staging region vs. CC35x1
            // PSA vendor-image slot), so it comes from the platform seam.
            //
            usnprintf(pcInsert, iInsertLen, "%u",
                      (unsigned)WebPlatformOtaMaxBytes());
            break;

        case SSI_INDEX_OTAPOST:
            //
            // Which upload transport the Tools page should use: 1 = stream the
            // image as a single binary POST (CC35x1's fast path), 0 = legacy
            // hex-GET chunk loop (TM4C).  Platform seam.
            //
            usnprintf(pcInsert, iInsertLen, "%u",
                      (unsigned)WebPlatformOtaUsePost());
            break;

        case SSI_INDEX_OTAERROR:
            //
            // Last OTA error message (if any) from a failed staging attempt,
            // e.g. "Staging failed: -133 (NOT_PERMITTED...) at byte 48".
            // Empty string if no error.  Platform seam (CC35x1 only).
            //
            WebPlatformOtaError(pcInsert, iInsertLen);
            break;

        case SSI_INDEX_WIFIOPTS:
            //
            // SSID dropdown for the Wi-Fi setup page.  The list of nearby networks
            // is platform-specific (CC35x1 renders its cached scan; TM4C has no
            // Wi-Fi and returns nothing), so it comes from the platform seam.
            //
            WebPlatformWifiScanOptions(pcInsert, iInsertLen);
            break;

        case SSI_INDEX_WIFITAB:
            //
            // Settings-page MQTT/Wi-Fi sub-tab bar, emitted only on the Wi-Fi
            // platform (CC35x1) so the Wi-Fi provisioning pane is reachable there;
            // the wired TM4C gets an empty string, leaving its Settings page with
            // no sub-tabs.  Platform seam.
            //
            WebPlatformWifiTab(pcInsert, iInsertLen);
            break;

        case SSI_INDEX_WSSID1:
        case SSI_INDEX_WPASS1:
        case SSI_INDEX_WSSID2:
        case SSI_INDEX_WPASS2:
            //
            // Saved Wi-Fi credential prefill for the Settings->Wi-Fi forms: the
            // primary (slot 0) and backup (slot 1) SSID/passphrase, HTML-escaped
            // for a double-quoted value="" attribute.  Platform seam (CC35x1 reads
            // the credential store; the TM4C build writes an empty string).
            //
            switch(iIndex)
            {
                case SSI_INDEX_WSSID1: WebPlatformWifiSsid(0, pcInsert, iInsertLen); break;
                case SSI_INDEX_WPASS1: WebPlatformWifiPass(0, pcInsert, iInsertLen); break;
                case SSI_INDEX_WSSID2: WebPlatformWifiSsid(1, pcInsert, iInsertLen); break;
                default:               WebPlatformWifiPass(1, pcInsert, iInsertLen); break;
            }
            break;

        default:
            usnprintf(pcInsert, iInsertLen, "??");
            break;
    }

    //
    // Tell the server how many characters our insert string contains.
    //
    return(strlen(pcInsert));
}

//*****************************************************************************
//
// WebUIRegister - hand the SSI + CGI tables to the lwIP httpd.  Casts absorb the
// legacy int32_t/char* handler prototypes into the httpd typedefs (same layout
// on both lwIP versions).  Call once after httpd_init() (core lock held on
// NO_SYS=0 builds).
//
//*****************************************************************************
void
WebUIRegister(void)
{
    //
    // Combined CGI table = foundation handlers ++ the product's (Plan 11).  The
    // product hands its table to the foundation via product_web_register(); we
    // concatenate into a static buffer and register once.
    //
    static tCGI        s_psCombinedCGI[NUM_CONFIG_CGI_URIS + WEBUI_MAX_PRODUCT_CGI];
    static const char *s_ppcCombinedSSI[NUM_CONFIG_SSI_TAGS + WEBUI_MAX_PRODUCT_SSI];
    product_web_reg_t  sReg;
    unsigned           uN = 0, uM = 0, i;

    memset(&sReg, 0, sizeof(sReg));
    product_web_register(&sReg);

    //
    // CGI: foundation handlers ++ product handlers, registered as one table.
    //
    for(i = 0; i < NUM_CONFIG_CGI_URIS; i++)
    {
        s_psCombinedCGI[uN++] = g_psConfigCGIURIs[i];
    }
    for(i = 0; (i < (unsigned)sReg.num_cgis) && (i < WEBUI_MAX_PRODUCT_CGI); i++)
    {
        s_psCombinedCGI[uN++] = sReg.cgis[i];
    }

    //
    // SSI: foundation tags first, product tags appended.  The registered index
    // order is what SSIHandler keys on: [0..NUM_CONFIG_SSI_TAGS) foundation,
    // the rest forwarded to product_ssi_handler() rebased to 0.
    //
    for(i = 0; i < NUM_CONFIG_SSI_TAGS; i++)
    {
        s_ppcCombinedSSI[uM++] = g_pcConfigSSITags[i];
    }
    for(i = 0; (i < (unsigned)sReg.num_ssi_tags) && (i < WEBUI_MAX_PRODUCT_SSI);
        i++)
    {
        s_ppcCombinedSSI[uM++] = sReg.ssi_tags[i];
    }

    http_set_ssi_handler((tSSIHandler)SSIHandler, s_ppcCombinedSSI, (int)uM);
    http_set_cgi_handlers(s_psCombinedCGI, (int)uN);
}

