//*****************************************************************************
//
// net_wifi.c (CC35x1) - Wi-Fi STA/AP -> lwIP -> DHCP bring-up.
//
// Distilled, from the SDK network_terminal demo's network_lwip.c +
// WlanStackEventHandler.  The demo file is unusable as-is here: it pulls in the
// demo's app_CB (network_terminal.c), the CLI, dhcpserver, uart_term, and a
// mountain of example app headers.  This file keeps only the pieces the port
// needs - the lwIP<->Wi-Fi netif glue and the connect/DHCP orchestration - and
// logs through PalLog.
//
// Two roles are supported, mutually exclusive (only one active at a time):
//   STA - join a home AP (the normal operating mode), DHCP client.
//   AP  - an open "MQTT-IO-Setup" access point (192.168.4.1) with a DHCP server,
//         used by the SoftAP provisioning flow so a user can enter credentials.
// The NWP driver is started once (NetWifiDriverStart); roles are then brought up,
// torn down, and switched live (no reboot - a warm reboot wedges the NWP).
//
// Threading (NO_SYS=0, LWIP_TCPIP_CORE_LOCKING=1): the netif callbacks below run
// on the lwIP tcpip_thread (invoked from tcpip_input / the netif state machine)
// and so must NOT take the core lock themselves; the entry points called from
// our task or the Wlan event thread wrap their raw-API work in LOCK_TCPIP_CORE()/
// UNLOCK_TCPIP_CORE().
//
//*****************************************************************************

#include <stdint.h>
#include <string.h>

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"

#include "wlan_if.h"
#include "errors.h"      // WLAN_RET_OPER_IN_PROGRESS

#include "pal_log.h"
#include "net_wifi.h"

//
// DHCP server (vendored from the SDK demo, dhcpserver.c).  Forward-declared here
// rather than including dhcpserver.h, which drags in its own struct ip_addr /
// dhcps_msg / macro soup we do not need.
//
extern void dhcps_start(uint32_t addr, struct netif *apnetif);
extern void dhcps_stop(void);

//
// lwIP Ethernet framing sizes (mirror network_lwip.c).
//
#define ETH_MAX_PAYLOAD     1514
#define VLAN_TAG_SIZE       4U
#define ETHHDR_SIZE         14
#define ETH_FRAME_SIZE      (ETH_MAX_PAYLOAD + VLAN_TAG_SIZE)

//
// Setup access-point identity.  Open network; the AP is only up during
// provisioning / fallback.  192.168.4.1/24 with the gateway = the AP itself.
//
#define AP_SSID             "MQTT-IO-Setup"
#define AP_IP_B0            192
#define AP_IP_B1            168
#define AP_IP_B2            4
#define AP_IP_B3            1
#define AP_CHANNEL          6
#define AP_STA_LIMIT        4       // max clients on the setup AP

//
// The active role.  Routes the shared netif callbacks and guards the STA-only
// Wlan event handling.
//
typedef enum { ROLE_NONE = 0, ROLE_STA, ROLE_AP } tNetRole;
static tNetRole g_eRole = ROLE_NONE;
static bool     g_bDriverStarted = false;

//
// The STA and AP interfaces and the STA DHCP client state.
//
static struct netif g_sStaIf;
static struct netif g_sApIf;
static struct dhcp  g_sStaDhcp;
static volatile int g_iIpAcquired;

//
// Cached Wi-Fi scan results for the provisioning page.  A scan is issued (STA
// role) just before the setup AP comes up - scanning needs the station role,
// which is torn down before the AP starts - and the result event fills this
// cache, which the wifi.shtml SSID dropdown renders.  Entries are deduplicated
// by SSID (strongest RSSI wins) and kept sorted by RSSI descending, so the
// closest networks render first and the 800-byte SSI insert holds the best ones.
//
#define SCAN_CACHE_MAX      12
#define SCAN_WAIT_MS        8000    // bounded wait for the async scan-result event
typedef struct
{
    char   pcSsid[WLAN_SSID_MAX_LENGTH + 1];
    int8_t i8Rssi;
} tScanEntry;
static tScanEntry    g_sScanCache[SCAN_CACHE_MAX];
static int           g_iScanCount;
static volatile bool g_bScanDone;

//
// Connection diagnostics (intentionally non-static so they can be read over the
// debugger while the serial backchannel is unavailable): event counters plus
// the reason/initiator of the most recent disconnect.  ReasonCode is the
// 802.11 reason (e.g. 4 = inactivity, 15 = 4-way-handshake timeout); Initiator
// is non-zero when the station (us) initiated the disconnect vs the AP.
//
volatile int g_iNetConnects;
volatile int g_iNetDisconnects;
volatile int g_iLastDiscReason;
volatile int g_iLastDiscInitiator;

//
// tcpip_thread-only TX staging buffer: linkoutput is always called on the
// tcpip_thread, so a single static buffer avoids a per-frame heap allocation.
// Shared by both roles (both TX on the tcpip_thread, so calls are serialised).
//
static uint8_t g_pui8TxBuf[ETH_FRAME_SIZE];

//
// WPS parameters are mandatory for AP mode on CC35xx even though WPS itself is
// disabled here.  Values copied from the SDK network_terminal demo (wlan_cmd.c).
//
static char          g_pcApSsid[]      = AP_SSID;
static const char    g_pcWpsMethods[]  = "virtual_display virtual_push_button keypad";
static const char    g_pcManufacturer[] = "TI";
static const char    g_pcModelName[]   = "MQTT-IO CC35X1";
static const char    g_pcModelNumber[] = "2025";
static const char    g_pcSerialNumber[] = "20252025";
static const uint8_t g_pui8Uuid[16+1]  = "0123456789123456";
static const uint8_t g_pui8DevType[8+1] =
    { 0x00, 0x06, 0x00, 0x00, 0xf2, 0x04, 0x00, 0x01, 0x00 };

//*****************************************************************************
//
// network_recv - Wlan driver RX callback: wrap the frame in a pbuf and hand it
// to lwIP, routed to the netif matching the role it arrived on.
//
//*****************************************************************************
static void
network_recv(WlanRole_e eRole, uint8_t *pui8In, uint32_t ui32Len)
{
    struct pbuf  *psBuf;
    struct netif *psIf;

    psIf = (eRole == WLAN_ROLE_AP) ? &g_sApIf : &g_sStaIf;

    psBuf = pbuf_alloc(PBUF_RAW, (u16_t)ui32Len, PBUF_POOL);
    if(psBuf == NULL)
    {
        return;
    }

    memcpy(psBuf->payload, pui8In, ui32Len);
    psBuf->len = (u16_t)ui32Len;
    psBuf->tot_len = (u16_t)ui32Len;

    if(tcpip_input(psBuf, psIf) != ERR_OK)
    {
        pbuf_free(psBuf);
    }
}

//*****************************************************************************
//
// network_send - lwIP linkoutput: flatten the pbuf chain and push it to the
// Wlan driver on the role that owns this netif.  Called on the tcpip_thread.
//
//*****************************************************************************
static err_t
network_send(struct netif *psNetIf, struct pbuf *psBuf)
{
    WlanRole_e eRole = (psNetIf == &g_sApIf) ? WLAN_ROLE_AP : WLAN_ROLE_STA;

    if(!netif_is_up(psNetIf))
    {
        return ERR_IF;
    }

    if(psBuf->tot_len > sizeof(g_pui8TxBuf))
    {
        return ERR_MEM;
    }

    //
    // Copy the (possibly chained) pbuf into the contiguous TX buffer.
    //
    pbuf_copy_partial(psBuf, g_pui8TxBuf, psBuf->tot_len, 0);

    Wlan_EtherPacketSend(eRole, g_pui8TxBuf, psBuf->tot_len, 0);

    return ERR_OK;
}

//*****************************************************************************
//
// status_callback - netif status changed.  Latch the MAC (for ARP) whenever an
// interface comes up; for the STA, also flag DHCP completion once it holds a
// non-zero IPv4 address.
//
//*****************************************************************************
static void
status_callback(struct netif *psNetIf)
{
    WlanMacAddress_t  sMac;
    const ip4_addr_t *psIP;

    if(!netif_is_up(psNetIf))
    {
        return;
    }

    //
    // Latch the hardware address into the netif for ARP (role-specific MAC).
    //
    memset(&sMac, 0, sizeof(sMac));
    sMac.roleType = (psNetIf == &g_sApIf) ? WLAN_ROLE_AP : WLAN_ROLE_STA;
    Wlan_Get(WLAN_GET_MACADDRESS, (void *)&sMac);
    memcpy(psNetIf->hwaddr, sMac.pMacAddress, 6);
    psNetIf->hwaddr_len = 6;

    if(psNetIf == &g_sStaIf)
    {
        psIP = netif_ip4_addr(psNetIf);
        if(psIP->addr != 0)
        {
            g_iIpAcquired = 1;
            PalLog("net: IP %s\n", ip4addr_ntoa(psIP));
        }
    }
}

//*****************************************************************************
//
// link_callback - link up/down.  On up, register the RX callback and start the
// DHCP client (STA) or DHCP server (AP); on down, stop it.
//
//*****************************************************************************
static void
link_callback(struct netif *psNetIf)
{
    bool       bAp   = (psNetIf == &g_sApIf);
    WlanRole_e eRole = bAp ? WLAN_ROLE_AP : WLAN_ROLE_STA;

    if(netif_is_link_up(psNetIf))
    {
        Wlan_EtherPacketRecvRegisterCallback(eRole, network_recv);
        if(bAp)
        {
            PalLog("net: AP link up, starting DHCP server\n");
            dhcps_start(psNetIf->ip_addr.addr, psNetIf);
        }
        else if(!g_iIpAcquired)
        {
            PalLog("net: link up, starting DHCP\n");
            dhcp_start(psNetIf);
        }
    }
    else
    {
        Wlan_EtherPacketRecvRegisterCallback(eRole, NULL);
        if(bAp)
        {
            dhcps_stop();
            PalLog("net: AP link down\n");
        }
        else
        {
            dhcp_stop(psNetIf);
            g_iIpAcquired = 0;
            PalLog("net: link down\n");
        }
    }
}

//*****************************************************************************
//
// sta_netif_init - STA netif init function (netif_add): install the driver hooks.
//
//*****************************************************************************
static err_t
sta_netif_init(struct netif *psNetIf)
{
    netif_set_status_callback(psNetIf, status_callback);
    netif_set_link_callback(psNetIf, link_callback);
    dhcp_set_struct(psNetIf, &g_sStaDhcp);

    psNetIf->name[0] = 's';
    psNetIf->name[1] = 't';
    psNetIf->mtu = ETH_FRAME_SIZE - ETHHDR_SIZE - VLAN_TAG_SIZE;
    psNetIf->output = etharp_output;
    psNetIf->linkoutput = network_send;
    psNetIf->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP;

    return ERR_OK;
}

//*****************************************************************************
//
// ap_netif_init - AP netif init function.  No DHCP client (the AP runs a DHCP
// server); the static IP is supplied by netif_add.
//
//*****************************************************************************
static err_t
ap_netif_init(struct netif *psNetIf)
{
    netif_set_status_callback(psNetIf, status_callback);
    netif_set_link_callback(psNetIf, link_callback);

    psNetIf->name[0] = 'a';
    psNetIf->name[1] = 'p';
    psNetIf->mtu = ETH_FRAME_SIZE - ETHHDR_SIZE - VLAN_TAG_SIZE;
    psNetIf->output = etharp_output;
    psNetIf->linkoutput = network_send;
    psNetIf->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP;

    return ERR_OK;
}

//*****************************************************************************
//
// tcpip_init_done - tcpip_init completion signal.
//
//*****************************************************************************
static void
tcpip_init_done(void *pvArg)
{
    sys_sem_signal((sys_sem_t *)pvArg);
}

//*****************************************************************************
//
// NetWifiInit - start the lwIP TCP/IP thread and wait for it.
//
//*****************************************************************************
void
NetWifiInit(void)
{
    sys_sem_t sInitSem;

    sys_sem_new(&sInitSem, 0);
    tcpip_init(tcpip_init_done, &sInitSem);
    sys_sem_wait(&sInitSem);
    sys_sem_free(&sInitSem);
}

//*****************************************************************************
//
// scan_cache_add - insert one scanned SSID into the RSSI-sorted cache, keeping
// it deduplicated (a repeated SSID keeps the stronger RSSI) and bounded to the
// SCAN_CACHE_MAX strongest.  Called from the scan-result event only.
//
//*****************************************************************************
static void
scan_cache_add(const char *pcSsid, int8_t i8Rssi)
{
    int i, iPos, iLast;

    //
    // Already cached?  Keep the stronger reading and we are done.  (Not re-sorted
    // on an RSSI bump - close enough for a one-shot provisioning scan.)
    //
    for(i = 0; i < g_iScanCount; i++)
    {
        if(strcmp(g_sScanCache[i].pcSsid, pcSsid) == 0)
        {
            if(i8Rssi > g_sScanCache[i].i8Rssi)
            {
                g_sScanCache[i].i8Rssi = i8Rssi;
            }
            return;
        }
    }

    //
    // New SSID: find its RSSI-sorted slot (descending).  If the cache is full and
    // this one is weaker than everything kept, drop it.
    //
    for(iPos = 0; iPos < g_iScanCount; iPos++)
    {
        if(i8Rssi > g_sScanCache[iPos].i8Rssi)
        {
            break;
        }
    }
    if(iPos >= SCAN_CACHE_MAX)
    {
        return;
    }

    //
    // Shift the tail down to open the slot, evicting the weakest when full.
    //
    iLast = (g_iScanCount < SCAN_CACHE_MAX) ? g_iScanCount : (SCAN_CACHE_MAX - 1);
    for(i = iLast; i > iPos; i--)
    {
        g_sScanCache[i] = g_sScanCache[i - 1];
    }

    strncpy(g_sScanCache[iPos].pcSsid, pcSsid, WLAN_SSID_MAX_LENGTH);
    g_sScanCache[iPos].pcSsid[WLAN_SSID_MAX_LENGTH] = '\0';
    g_sScanCache[iPos].i8Rssi = i8Rssi;

    if(g_iScanCount < SCAN_CACHE_MAX)
    {
        g_iScanCount++;
    }
}

//*****************************************************************************
//
// WlanStackEventHandler - Wlan driver event callback (required by Wlan_Start).
// On a successful STA connect, bring the STA netif up (which triggers DHCP via
// link_callback).  Guarded by g_eRole so AP-mode peer events never touch the STA
// interface.
//
//*****************************************************************************
void
WlanStackEventHandler(WlanEvent_t *psEvent)
{
    if(psEvent == NULL)
    {
        return;
    }

    switch(psEvent->Id)
    {
        case WLAN_EVENT_CONNECT:
        {
            if(g_eRole != ROLE_STA)
            {
                break;
            }
            if(psEvent->Data.Connect.Status < 0)
            {
                PalLog("net: connect failed\n");
                break;
            }
            g_iNetConnects++;
            PalLog("net: connected to AP\n");

            //
            // Bring the interface up on the tcpip_thread (raw-API -> lock).
            //
            LOCK_TCPIP_CORE();
            netif_set_up(&g_sStaIf);
            netif_set_link_up(&g_sStaIf);
            UNLOCK_TCPIP_CORE();
            break;
        }

        case WLAN_EVENT_DISCONNECT:
        {
            if(g_eRole != ROLE_STA)
            {
                break;
            }
            g_iNetDisconnects++;
            g_iLastDiscReason = (int)psEvent->Data.Disconnect.ReasonCode;
            g_iLastDiscInitiator =
                (int)psEvent->Data.Disconnect.IsStaIsDiscnctInitiator;
            PalLog("net: disconnected (reason %d, initiator %d)\n",
                   g_iLastDiscReason, g_iLastDiscInitiator);
            LOCK_TCPIP_CORE();
            netif_set_link_down(&g_sStaIf);
            netif_set_down(&g_sStaIf);
            UNLOCK_TCPIP_CORE();
            break;
        }

        case WLAN_EVENT_SCAN_RESULT:
        {
            //
            // Async result of the provisioning scan.  Rebuild the cache from the
            // reported networks (skip hidden/empty SSIDs) and signal the waiter.
            // Touches no netif, so it needs neither the role guard nor the lock.
            //
            WlanEventScanResult_t *psScan = &psEvent->Data.ScanResult;
            uint32_t ui32N = psScan->NetworkListResultLen;
            uint32_t i;

            if(ui32N > WLAN_MAX_SCAN_COUNT)
            {
                ui32N = WLAN_MAX_SCAN_COUNT;
            }

            g_iScanCount = 0;
            for(i = 0; i < ui32N; i++)
            {
                WlanNetworkEntry_t *psE = &psScan->NetworkListResult[i];
                char pcSsid[WLAN_SSID_MAX_LENGTH + 1];
                int  iLen = (int)psE->SsidLen;

                if((iLen <= 0) || (iLen > WLAN_SSID_MAX_LENGTH))
                {
                    continue;               // hidden or malformed
                }
                memcpy(pcSsid, psE->Ssid, (size_t)iLen);
                pcSsid[iLen] = '\0';
                scan_cache_add(pcSsid, psE->Rssi);
            }

            g_bScanDone = true;
            PalLog("net: scan found %d network(s)\n", g_iScanCount);
            break;
        }

        default:
            break;
    }
}

//*****************************************************************************
//
// net_issue_connect - (re)issue the Wlan_Connect association request.  Split out
// so the one-time NWP/role setup is not repeated on a retry.
//
//*****************************************************************************
static int
net_issue_connect(const char *pcSsid, const char *pcPass)
{
    int  iPassLen;
    char cSecType;
    int  iRet;

    iPassLen = (pcPass != NULL) ? (int)strlen(pcPass) : 0;

    //
    // Security type must match the AP's advertised AKM (see the SDK CME AKM
    // table, cme.c):
    //   WLAN_SEC_TYPE_WPA_WPA2 (2)  -> WPA_KEY_MGMT_PSK        (standard WPA2-PSK)
    //   WLAN_SEC_TYPE_WPA2_PLUS(11) -> WPA_KEY_MGMT_PSK_SHA256 (PMF/802.11w only)
    // A plain WPA2-PSK(AES) router advertises only PSK, so WPA2_PLUS - which
    // negotiates *only* PSK-SHA256 - mismatches the AP's AKM and the 4-way
    // handshake is deauthed (CME SUPPLICANT_MANAGED_STATE -> PEER_DISCONNECT,
    // 802.11 reason 15).  Plain WPA_WPA2 matches a standard WPA2-PSK AP.  (For a
    // WPA2/WPA3-mixed or WPA3-only AP use WLAN_SEC_TYPE_WPA2_WPA3 or
    // WLAN_SEC_TYPE_WPA3.)
    //
    cSecType = (iPassLen > 0) ? WLAN_SEC_TYPE_WPA_WPA2 : WLAN_SEC_TYPE_OPEN;

    iRet = Wlan_Connect((const signed char *)pcSsid, (int)strlen(pcSsid), NULL,
                        cSecType, pcPass, (char)iPassLen, 0);
    if(iRet < 0)
    {
        PalLog("net: Wlan_Connect failed (%d)\n", iRet);
        return(-1);
    }

    PalLog("net: connecting to %s (sec %d)\n", pcSsid, (int)cSecType);
    return(0);
}

//*****************************************************************************
//
// NetWifiDriverStart - one-time NWP bring-up: Wlan_Start, disable the automatic
// connection manager, wipe stored profiles, keep the radio always active.
// Idempotent.
//
//*****************************************************************************
int
NetWifiDriverStart(void)
{
    if(g_bDriverStarted)
    {
        return(0);
    }

    //
    // Start the network processor with our event handler.
    //
    // NOTE: a JTAG reload resets the M33 but NOT the Wi-Fi network processor, so
    // across reflashes without a USB power-cycle the NWP can retain stale
    // supplicant/PMF state that makes the WPA2 4-way handshake time out (802.11
    // reason 15).  Calling Wlan_Stop() here to force-clean it makes the driver
    // transport layer assert on an already-wedged NWP, so the correct recovery
    // is a physical power-cycle rather than a software stop/start.
    //
    if(Wlan_Start(WlanStackEventHandler) != 0)
    {
        PalLog("net: Wlan_Start failed\n");
        return(-1);
    }

    //
    // We drive explicit connections, so disable the NWP's automatic connection
    // manager and wipe any stored profiles.  Otherwise a leftover profile +
    // auto/fast-connect policy (this board was used with the SDK provisioning
    // demos, which persist both in NVS) spawns a second, competing connection
    // owner: our Wlan_Connect takes the STA flow as CME_STA_WLAN_CONNECT_USER,
    // the background fast-connect then requests ownership too, and the CME
    // rejects the transition and tears the link down.  Clearing them here leaves
    // our explicit connect as the sole owner.
    //
    {
        WlanPolicySetGet_t sPolicy;
        int                iRc;

        memset(&sPolicy, 0, sizeof(sPolicy)); // auto=0, fast=0, fastPersistant=0
        while((iRc = Wlan_Set(WLAN_SET_CONNECTION_POLICY, &sPolicy)) ==
              WLAN_RET_OPER_IN_PROGRESS)
        {
        }
        if(iRc != 0)
        {
            PalLog("net: disable conn policy failed (%d)\n", iRc);
        }

        // 0xFF == WLAN_DEL_ALL_PROFILES (internal SDK constant).
        Wlan_ProfileDel(0xFF);
    }

    //
    // Keep the radio always active.  Phone hotspots (and many APs) aggressively
    // deauth a station that drops into Wi-Fi power save, which shows up as
    // "associate, get DHCP, then drop a few seconds later".  ALWAYS_ACTIVE
    // trades power for a stable link - correct for a mains-powered gateway.
    //
    {
        uint32_t ui32PwrMgmt = (uint32_t)POWER_MANAGEMENT_ALWAYS_ACTIVE_MODE;
        int      iRc;

        while((iRc = Wlan_Set(WLAN_SET_POWER_MANAGEMENT, &ui32PwrMgmt)) ==
              WLAN_RET_OPER_IN_PROGRESS)
        {
        }
        if(iRc != 0)
        {
            PalLog("net: set power mgmt failed (%d)\n", iRc);
        }
    }

    g_bDriverStarted = true;
    return(0);
}

//*****************************************************************************
//
// net_sta_role_up - add the STA netif and activate the station role on the NWP
// (no association yet).  Idempotent while the STA role is already up.
//
//*****************************************************************************
static int
net_sta_role_up(void)
{
    ip4_addr_t     sZero;
    RoleUpStaCmd_t sRoleUp;
    int            iRet;

    if(g_eRole == ROLE_STA)
    {
        return(0);
    }

    //
    // Register the STA interface with a zeroed address (DHCP fills it in).
    //
    ip4_addr_set_zero(&sZero);
    LOCK_TCPIP_CORE();
    netif_add(&g_sStaIf, &sZero, &sZero, &sZero, NULL, sta_netif_init,
              tcpip_input);
    netif_set_default(&g_sStaIf);
    UNLOCK_TCPIP_CORE();

    //
    // Activate the STA role on the NWP.  This blocking call is what actually
    // brings the station interface up on the network processor; without it
    // Wlan_Connect has no active role and never associates.  "00" selects the
    // worldwide regulatory domain, matching the SDK demos.
    //
    memset(&sRoleUp, 0, sizeof(sRoleUp));
    sRoleUp.countryDomain[0] = '0';
    sRoleUp.countryDomain[1] = '0';
    iRet = Wlan_RoleUp(WLAN_ROLE_STA, &sRoleUp, WLAN_WAIT_FOREVER);
    if(iRet < 0)
    {
        PalLog("net: Wlan_RoleUp(STA) failed (%d)\n", iRet);
        LOCK_TCPIP_CORE();
        netif_remove(&g_sStaIf);
        UNLOCK_TCPIP_CORE();
        return(-1);
    }

    g_eRole = ROLE_STA;
    return(0);
}

//*****************************************************************************
//
// NetWifiStaUp - bring the station role up and issue the first association.
//
//*****************************************************************************
int
NetWifiStaUp(const char *pcSsid, const char *pcPass)
{
    if(net_sta_role_up() != 0)
    {
        return(-1);
    }
    return(net_issue_connect(pcSsid, pcPass));
}

//*****************************************************************************
//
// NetWifiStaDown - disconnect, role-down and remove the STA netif.
//
//*****************************************************************************
void
NetWifiStaDown(void)
{
    if(g_eRole != ROLE_STA)
    {
        return;
    }

    //
    // Clear the role BEFORE tearing the interface down.  WlanStackEventHandler
    // (which runs on the Wlan event thread) gates its netif_set_up/down of
    // g_sStaIf on g_eRole == ROLE_STA; a late/queued WLAN_EVENT_CONNECT or
    // DISCONNECT provoked by the Wlan_Disconnect below could otherwise pass that
    // guard and touch g_sStaIf while - or after - we netif_remove it.  Dropping
    // the role first makes the guard close this window.
    //
    g_eRole = ROLE_NONE;
    g_iIpAcquired = 0;

    Wlan_Disconnect(WLAN_ROLE_STA, NULL);

    LOCK_TCPIP_CORE();
    netif_set_link_down(&g_sStaIf);
    netif_set_down(&g_sStaIf);
    UNLOCK_TCPIP_CORE();

    Wlan_RoleDown(WLAN_ROLE_STA, WLAN_WAIT_FOREVER);

    LOCK_TCPIP_CORE();
    netif_remove(&g_sStaIf);
    UNLOCK_TCPIP_CORE();

    PalLog("net: STA down\n");
}

//*****************************************************************************
//
// NetWifiApUp - bring the open setup AP up and start its DHCP server.
//
//*****************************************************************************
int
NetWifiApUp(void)
{
    ip4_addr_t    sIp, sMask, sGw;
    RoleUpApCmd_t sAp;
    int           iRet;

    if(g_eRole == ROLE_AP)
    {
        return(0);
    }

    IP4_ADDR(&sIp,   AP_IP_B0, AP_IP_B1, AP_IP_B2, AP_IP_B3);
    IP4_ADDR(&sMask, 255, 255, 255, 0);
    sGw = sIp;

    LOCK_TCPIP_CORE();
    netif_add(&g_sApIf, &sIp, &sMask, &sGw, NULL, ap_netif_init, tcpip_input);
    //
    // Give lwIP a valid default netif while the AP is up (netif_remove of the STA
    // interface reset it to NULL).  net_sta_role_up restores the STA as default
    // when we switch back.
    //
    netif_set_default(&g_sApIf);
    UNLOCK_TCPIP_CORE();

    //
    // Activate the AP role on the NWP.  WPS parameters are mandatory on CC35xx
    // even with WPS disabled (see the SDK AGENTS.md), so they are always filled.
    //
    memset(&sAp, 0, sizeof(sAp));
    sAp.ssid      = (uint8_t *)g_pcApSsid;
    sAp.hidden    = 0;
    sAp.channel   = AP_CHANNEL;
    sAp.sta_limit = AP_STA_LIMIT;
    sAp.secParams.Type   = WLAN_SEC_TYPE_OPEN;
    sAp.secParams.Key    = (int8_t *)"";
    sAp.secParams.KeyLen = 0;
    sAp.countryDomain[0] = '0';
    sAp.countryDomain[1] = '0';
    sAp.wpsDisabled = TRUE;
    sAp.wpsParams.deviceName    = (char *)g_pcModelName;
    sAp.wpsParams.configMethods = (char *)g_pcWpsMethods;
    sAp.wpsParams.manufacturer  = (char *)g_pcManufacturer;
    sAp.wpsParams.modelName     = (char *)g_pcModelName;
    sAp.wpsParams.modelNumber   = (char *)g_pcModelNumber;
    sAp.wpsParams.serialNumber  = (char *)g_pcSerialNumber;
    sAp.wpsParams.uuid          = (uint8_t *)g_pui8Uuid;
    sAp.wpsParams.deviceType    = (uint8_t *)g_pui8DevType;

    iRet = Wlan_RoleUp(WLAN_ROLE_AP, &sAp, WLAN_WAIT_FOREVER);
    if(iRet < 0)
    {
        PalLog("net: Wlan_RoleUp(AP) failed (%d)\n", iRet);
        LOCK_TCPIP_CORE();
        netif_remove(&g_sApIf);
        UNLOCK_TCPIP_CORE();
        return(-1);
    }

    g_eRole = ROLE_AP;

    //
    // Bring the interface up: this triggers link_callback, which starts the DHCP
    // server so joining clients get a 192.168.4.x lease.
    //
    LOCK_TCPIP_CORE();
    netif_set_up(&g_sApIf);
    netif_set_link_up(&g_sApIf);
    UNLOCK_TCPIP_CORE();

    PalLog("net: AP '%s' up at %u.%u.%u.%u\n", g_pcApSsid,
           AP_IP_B0, AP_IP_B1, AP_IP_B2, AP_IP_B3);
    return(0);
}

//*****************************************************************************
//
// NetWifiApDown - stop the DHCP server, role-down and remove the AP netif.
//
//*****************************************************************************
void
NetWifiApDown(void)
{
    if(g_eRole != ROLE_AP)
    {
        return;
    }

    //
    // Take the link down first (link_callback stops the DHCP server), then
    // remove the netif before rolling the AP down (mirrors the SDK ordering).
    //
    LOCK_TCPIP_CORE();
    netif_set_link_down(&g_sApIf);
    netif_set_down(&g_sApIf);
    netif_remove(&g_sApIf);
    UNLOCK_TCPIP_CORE();

    Wlan_RoleDown(WLAN_ROLE_AP, WLAN_WAIT_FOREVER);

    g_eRole = ROLE_NONE;
    PalLog("net: AP down\n");
}

//*****************************************************************************
//
// NetWifiSwitchToSta - live AP -> STA transition (no reboot).
//
//*****************************************************************************
int
NetWifiSwitchToSta(const char *pcSsid, const char *pcPass)
{
    NetWifiApDown();
    return(NetWifiStaUp(pcSsid, pcPass));
}

//*****************************************************************************
//
// NetWifiScanCache - refresh the cached SSID list for the provisioning page.
// Scanning needs the station role, which is torn down before the setup AP comes
// up, so this brings the STA role up (if not already), issues an asynchronous
// Wlan_Scan, waits (bounded by SCAN_WAIT_MS) for WLAN_EVENT_SCAN_RESULT to fill
// the cache, then drops the STA role so the caller can NetWifiApUp().  Best
// effort: on any failure the cache is left as-is and the page falls back to
// manual SSID entry.  Call from the app task (it blocks via sys_msleep).
//
//*****************************************************************************
void
NetWifiScanCache(void)
{
    scanCommon_t sScan;
    int          iWaited;
    int          iRc;

    if(net_sta_role_up() != 0)
    {
        PalLog("net: scan skipped (STA role up failed)\n");
        return;
    }

    g_bScanDone = false;

    memset(&sScan, 0, sizeof(sScan));
    sScan.Band = BAND_SEL_BOTH;             // list 2.4 GHz and 5 GHz networks

    iRc = Wlan_Scan(WLAN_ROLE_STA, &sScan, (unsigned char)WLAN_MAX_SCAN_COUNT);
    if(iRc < 0)
    {
        PalLog("net: Wlan_Scan failed (%d)\n", iRc);
        NetWifiStaDown();
        return;
    }

    for(iWaited = 0; !g_bScanDone && (iWaited < SCAN_WAIT_MS); iWaited += 100)
    {
        sys_msleep(100);
    }
    if(!g_bScanDone)
    {
        PalLog("net: scan timed out\n");
    }

    //
    // Drop the STA role we brought up (or that the failed connect left up); the
    // caller brings the AP up next, and the two roles are mutually exclusive.
    //
    NetWifiStaDown();
}

//*****************************************************************************
//
// NetWifiScanCount / NetWifiScanGet - read back the cached provisioning scan.
// NetWifiScanGet copies entry iIndex's SSID (NUL-terminated, truncated to
// iSsidLen) and, if pi8Rssi is non-NULL, its RSSI.  Returns false for an
// out-of-range index.  Entries are ordered strongest-RSSI first.
//
//*****************************************************************************
int
NetWifiScanCount(void)
{
    return(g_iScanCount);
}

bool
NetWifiScanGet(int iIndex, char *pcSsid, int iSsidLen, int8_t *pi8Rssi)
{
    if((iIndex < 0) || (iIndex >= g_iScanCount) ||
       (pcSsid == NULL) || (iSsidLen <= 0))
    {
        return(false);
    }

    strncpy(pcSsid, g_sScanCache[iIndex].pcSsid, (size_t)(iSsidLen - 1));
    pcSsid[iSsidLen - 1] = '\0';
    if(pi8Rssi != NULL)
    {
        *pi8Rssi = g_sScanCache[iIndex].i8Rssi;
    }
    return(true);
}

//*****************************************************************************
//
// NetWifiReconnect - re-issue the association without re-initialising the NWP
// or STA role.  Used by the connect-retry loop when DHCP does not complete.
//
//*****************************************************************************
int
NetWifiReconnect(const char *pcSsid, const char *pcPass)
{
    if(g_eRole != ROLE_STA)
    {
        return(-1);
    }
    return(net_issue_connect(pcSsid, pcPass));
}

//*****************************************************************************
//
// NetWifiIsAp - non-zero while the setup access point is the active role.
//
//*****************************************************************************
int
NetWifiIsAp(void)
{
    return(g_eRole == ROLE_AP);
}

//*****************************************************************************
//
// NetWifiIsIpAcquired / NetWifiGetMac / IP accessors.
//
//*****************************************************************************
int
NetWifiIsIpAcquired(void)
{
    return(g_iIpAcquired);
}

void
NetWifiGetMac(uint8_t *pui8Mac)
{
    WlanMacAddress_t sMac;

    memset(&sMac, 0, sizeof(sMac));
    sMac.roleType = WLAN_ROLE_STA;
    Wlan_Get(WLAN_GET_MACADDRESS, (void *)&sMac);
    memcpy(pui8Mac, sMac.pMacAddress, 6);
}

void
NetWifiGetIp(char *pcBuf, int iLen)
{
    if((pcBuf == NULL) || (iLen <= 0))
    {
        return;
    }

    //
    // netif_ip4_addr reads a single 32-bit field; ip4addr_ntoa_r formats into
    // the caller's buffer (the plain ip4addr_ntoa uses a shared static buffer
    // and is not reentrant, so avoid it here).  Reads 0.0.0.0 before a lease.
    //
    ip4addr_ntoa_r(netif_ip4_addr(&g_sStaIf), pcBuf, iLen);
}

uint32_t
NetWifiGetIp4(void)
{
    //
    // Raw IPv4 address word (lwIP network order; 0 before a lease).  Feeds the
    // shared web UI's "ipaddr" SSI tag via g_ui32IPAddress.
    //
    return(netif_ip4_addr(&g_sStaIf)->addr);
}
