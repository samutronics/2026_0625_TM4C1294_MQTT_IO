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
            PalLog("net: disconnected\n");
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
// NetWifiConnect - add the STA netif, start the NWP, and issue the connect.
//
//*****************************************************************************
int
NetWifiConnect(const char *pcSsid, const char *pcPass)
{
    ip4_addr_t sZero;
    int        iPassLen;
    char       cSecType;

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
    if(Wlan_Start(WlanStackEventHandler) != 0)
    {
        PalLog("net: Wlan_Start failed\n");
        return -1;
    }

    iPassLen = (pcPass != NULL) ? (int)strlen(pcPass) : 0;
    cSecType = (iPassLen > 0) ? WLAN_SEC_TYPE_WPA_WPA2 : WLAN_SEC_TYPE_OPEN;

    if(Wlan_Connect((const signed char *)pcSsid, (int)strlen(pcSsid), NULL,
                    cSecType, pcPass, (char)iPassLen, 0) < 0)
    {
        PalLog("net: Wlan_Connect failed\n");
        return -1;
    }

    return 0;
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
