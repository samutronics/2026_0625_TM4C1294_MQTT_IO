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
// Re-issue the association (no NWP/role re-init) after a failed/dropped connect.
// NetWifiConnect() must have been called once first.  Returns 0 if the connect
// request was issued.
//
int NetWifiReconnect(const char *pcSsid, const char *pcPass);

//
// Non-zero once DHCP has assigned the STA interface an IPv4 address.
//
int NetWifiIsIpAcquired(void);

//
// Read the device MAC (6 bytes) into pui8Mac.  Valid after the NWP has started.
//
void NetWifiGetMac(uint8_t *pui8Mac);

//
// Copy the current STA IPv4 address as a dotted-decimal string into pcBuf
// (needs room for "255.255.255.255" + NUL, i.e. >= 16 bytes).  Writes "0.0.0.0"
// when no lease is held.  Safe to call from the app task (reentrant ntoa).
//
void NetWifiGetIp(char *pcBuf, int iLen);

//
// Raw current STA IPv4 address as a 32-bit word (lwIP network order; 0 before a
// lease).  Feeds the web UI's "ipaddr" SSI tag (g_ui32IPAddress).
//
uint32_t NetWifiGetIp4(void);

//
// Connection diagnostics maintained by the Wlan event handler: cumulative
// connect/disconnect counts and the 802.11 reason/initiator of the most recent
// disconnect (reason 15 = 4-way-handshake timeout; initiator != 0 = we dropped
// it).  Exposed so the periodic heartbeat log can surface link churn over the
// serial backchannel.
//
extern volatile int g_iNetConnects;
extern volatile int g_iNetDisconnects;
extern volatile int g_iLastDiscReason;
extern volatile int g_iLastDiscInitiator;

#endif // NET_WIFI_H
