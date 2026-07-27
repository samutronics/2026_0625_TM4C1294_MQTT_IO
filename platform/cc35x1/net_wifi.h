//*****************************************************************************
//
// net_wifi.h (CC35x1) - minimal Wi-Fi STA -> lwIP -> DHCP bring-up seam.
//
// A self-contained, STA-only distillation of the SDK network_terminal demo's
// network_lwip.c netif glue (network_send/recv, the _role_sta_up init, the link
// and status netif callbacks) plus its WlanStackEventHandler, with the demo's
// app_CB / CLI / dhcpserver / uart_term coupling removed.  Logging goes through
// PalLog.  This is the CC35x1 analogue of the TM4C's lwIPInit()+DHCP path.
//
//*****************************************************************************

#ifndef NET_WIFI_H
#define NET_WIFI_H

#include <stdint.h>

//
// Bring up the lwIP TCP/IP thread (tcpip_init) and block until it is ready.
// Call once, before NetWifiConnect().
//
void NetWifiInit(void);

//
// Register the STA netif, start the NWP, and issue an asynchronous connect to
// the given open/WPA2 AP.  A NULL or empty password selects an open network.
// Returns 0 if the connect request was issued.
//
int NetWifiConnect(const char *pcSsid, const char *pcPass);

//
// Non-zero once DHCP has assigned the STA interface an IPv4 address.
//
int NetWifiIsIpAcquired(void);

//
// Read the device MAC (6 bytes) into pui8Mac.  Valid after the NWP has started.
//
void NetWifiGetMac(uint8_t *pui8Mac);

#endif // NET_WIFI_H
