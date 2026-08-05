//*****************************************************************************
//
// net_wifi.c (CC35x1) - Wi-Fi STA -> lwIP -> DHCP bring-up.
//
// Distilled, STA-only, from the SDK network_terminal demo's network_lwip.c +
// WlanStackEventHandler.  The demo file is unusable as-is here: it pulls in the
// demo's app_CB (network_terminal.c), the CLI, dhcpserver, uart_term, and a
// mountain of example app headers.  This file keeps only the pieces the port
// needs - the lwIP<->Wi-Fi netif glue and the connect/DHCP orchestration - and
// logs through PalLog.
//
// Threading (NO_SYS=0, LWIP_TCPIP_CORE_LOCKING=1): the netif callbacks below run
// on the lwIP tcpip_thread (invoked from tcpip_input / the netif state machine)
// and so must NOT take the core lock themselves; the entry points called from
// our task or the Wlan event thread (network_set_up, network_stack_add_if_sta)
// wrap their raw-API work in LOCK_TCPIP_CORE()/UNLOCK_TCPIP_CORE().
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
#include "netif/ethernet.h"

#include "wlan_if.h"
#include "errors.h"      // WLAN_RET_OPER_IN_PROGRESS

#include "pal_log.h"
#include "net_wifi.h"

//
// lwIP Ethernet framing sizes (mirror network_lwip.c).
//
#define ETH_MAX_PAYLOAD     1514
#define VLAN_TAG_SIZE       4U
#define ETHHDR_SIZE         14
#define ETH_FRAME_SIZE      (ETH_MAX_PAYLOAD + VLAN_TAG_SIZE)

//
// The single STA interface and its DHCP/state.
//
static struct netif g_sStaIf;
static struct dhcp  g_sStaDhcp;
static volatile int g_iIpAcquired;

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
//
static uint8_t g_pui8TxBuf[ETH_FRAME_SIZE];

//*****************************************************************************
//
// network_recv - Wlan driver RX callback: wrap the frame in a pbuf and hand it
// to lwIP.  Registered per-role once the link is up.
//
//*****************************************************************************
static void
network_recv(WlanRole_e eRole, uint8_t *pui8In, uint32_t ui32Len)
{
    struct pbuf *psBuf;

    (void)eRole;

    psBuf = pbuf_alloc(PBUF_RAW, (u16_t)ui32Len, PBUF_POOL);
    if(psBuf == NULL)
    {
        return;
    }

    memcpy(psBuf->payload, pui8In, ui32Len);
    psBuf->len = (u16_t)ui32Len;
    psBuf->tot_len = (u16_t)ui32Len;

    if(tcpip_input(psBuf, &g_sStaIf) != ERR_OK)
    {
        pbuf_free(psBuf);
    }
}

//*****************************************************************************
//
// network_send - lwIP linkoutput: flatten the pbuf chain and push it to the
// Wlan driver.  Called on the tcpip_thread.
//
//*****************************************************************************
static err_t
network_send(struct netif *psNetIf, struct pbuf *psBuf)
{
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

    Wlan_EtherPacketSend(WLAN_ROLE_STA, g_pui8TxBuf, psBuf->tot_len, 0);

    return ERR_OK;
}

//*****************************************************************************
//
// status_callback - netif status changed.  When the interface comes up with a
// non-zero IPv4 address, DHCP has completed: latch the MAC and flag readiness.
//
//*****************************************************************************
static void
status_callback(struct netif *psNetIf)
{
    const ip4_addr_t *psIP;

    if(!netif_is_up(psNetIf))
    {
        return;
    }

    //
    // Latch the hardware address into the netif for ARP.
    //
    {
        WlanMacAddress_t sMac;

        memset(&sMac, 0, sizeof(sMac));
        sMac.roleType = WLAN_ROLE_STA;
        Wlan_Get(WLAN_GET_MACADDRESS, (void *)&sMac);
        memcpy(psNetIf->hwaddr, sMac.pMacAddress, 6);
        psNetIf->hwaddr_len = 6;
    }

    psIP = netif_ip4_addr(psNetIf);
    if(psIP->addr != 0)
    {
        g_iIpAcquired = 1;
        PalLog("net: IP %s\n", ip4addr_ntoa(psIP));
    }
}

//*****************************************************************************
//
// link_callback - link up/down.  On up, register the RX callback and start
// DHCP; on down, stop DHCP.
//
//*****************************************************************************
static void
link_callback(struct netif *psNetIf)
{
    if(netif_is_link_up(psNetIf))
    {
        Wlan_EtherPacketRecvRegisterCallback(WLAN_ROLE_STA, network_recv);
        if(!g_iIpAcquired)
        {
            PalLog("net: link up, starting DHCP\n");
            dhcp_start(psNetIf);
        }
    }
    else
    {
        Wlan_EtherPacketRecvRegisterCallback(WLAN_ROLE_STA, NULL);
        dhcp_stop(psNetIf);
        g_iIpAcquired = 0;
        PalLog("net: link down\n");
    }
}

//*****************************************************************************
//
// sta_netif_init - netif init function (netif_add): install the driver hooks.
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
// WlanStackEventHandler - Wlan driver event callback (required by Wlan_Start).
// On a successful connect, bring the netif up (which triggers DHCP via
// link_callback).
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

        default:
            break;
    }
}

//*****************************************************************************
//
// net_issue_connect - (re)issue the Wlan_Connect association request.  Split out
// so the one-time NWP/role setup in NetWifiConnect is not repeated on a retry.
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
        return -1;
    }

    PalLog("net: connecting to %s (sec %d)\n", pcSsid, (int)cSecType);
    return 0;
}

//*****************************************************************************
//
// NetWifiConnect - add the STA netif, start the NWP, and issue the first
// connect.  Performs the one-time setup, then the initial association.
//
//*****************************************************************************
int
NetWifiConnect(const char *pcSsid, const char *pcPass)
{
    ip4_addr_t     sZero;
    RoleUpStaCmd_t sRoleUp;
    int            iRet;

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
    // Start the network processor with our event handler.
    //
    // NOTE: a JTAG reload resets the M33 but NOT the Wi-Fi network processor, so
    // across reflashes without a USB power-cycle the NWP can retain stale
    // supplicant/PMF state that makes the WPA2 4-way handshake time out (802.11
    // reason 15).  A calling Wlan_Stop() here to force-clean it made the driver
    // transport layer assert on an already-wedged NWP, so the correct recovery
    // is a physical power-cycle rather than a software stop/start.
    //
    if(Wlan_Start(WlanStackEventHandler) != 0)
    {
        PalLog("net: Wlan_Start failed\n");
        return -1;
    }

    //
    // We drive a single explicit connection (NetWifiConnect below), so disable
    // the NWP's automatic connection manager and wipe any stored profiles.
    // Otherwise a leftover profile + auto/fast-connect policy (this board was
    // used with the SDK provisioning demos, which persist both in NVS) spawns a
    // second, competing connection owner: our Wlan_Connect takes the STA flow
    // as CME_STA_WLAN_CONNECT_USER, the background fast-connect then requests
    // ownership too, and the CME rejects the transition and tears the link down
    // ("New User owner request" / disconnect).  Clearing them here leaves our
    // explicit connect as the sole owner.
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

    //
    // Activate the STA role on the NWP.  This blocking call is what actually
    // brings the station interface up on the network processor; without it
    // Wlan_Connect has no active role and never associates (no
    // WLAN_EVENT_CONNECT).  "00" selects the worldwide regulatory domain,
    // matching the SDK demos.
    //
    memset(&sRoleUp, 0, sizeof(sRoleUp));
    sRoleUp.countryDomain[0] = '0';
    sRoleUp.countryDomain[1] = '0';
    iRet = Wlan_RoleUp(WLAN_ROLE_STA, &sRoleUp, WLAN_WAIT_FOREVER);
    if(iRet < 0)
    {
        PalLog("net: Wlan_RoleUp failed (%d)\n", iRet);
        return -1;
    }

    //
    // Issue the first association; NetWifiReconnect re-issues it on retry.
    //
    return net_issue_connect(pcSsid, pcPass);
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
    return net_issue_connect(pcSsid, pcPass);
}

//*****************************************************************************
//
// NetWifiIsIpAcquired / NetWifiGetMac - status + MAC accessors.
//
//*****************************************************************************
int
NetWifiIsIpAcquired(void)
{
    return g_iIpAcquired;
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
