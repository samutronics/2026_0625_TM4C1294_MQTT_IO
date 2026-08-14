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
#include <stdbool.h>

//
// Bring up the lwIP TCP/IP thread (tcpip_init) and block until it is ready.
// Call once, before bringing any role up.
//
void NetWifiInit(void);

//
// Re-issue the association (no NWP/role re-init) after a failed/dropped connect.
// NetWifiStaUp() must have been called once first.  Returns 0 if the connect
// request was issued.
//
int NetWifiReconnect(const char *pcSsid, const char *pcPass);

//*****************************************************************************
//
// Role-aware bring-up used by the SoftAP provisioning flow.  The NWP driver is
// started once (NetWifiDriverStart); after that the station and access-point
// roles are brought up and torn down independently and can be switched live -
// no reboot - which is important because a warm SYSRESETREQ reboot wedges the
// NWP on this board (see the cc35x1-nwp-reset notes).  STA and AP are mutually
// exclusive: only one is active at a time.
//
//*****************************************************************************

//
// Start the NWP once: Wlan_Start, disable the auto/fast-connect policy, wipe any
// stored profiles, set always-active power.  Idempotent.  Returns 0 on success.
//
int NetWifiDriverStart(void);

//
// Bring the station role up (adding the STA netif if needed) and issue the first
// association to the given AP.  NetWifiDriverStart() must have run.  Returns 0 if
// the connect request was issued.
//
int NetWifiStaUp(const char *pcSsid, const char *pcPass);

//
// Tear the station role down: disconnect, role-down, remove the STA netif.
//
void NetWifiStaDown(void);

//
// Bring the open setup access point up (SSID "MQTT-IO-Setup", 192.168.4.1/24)
// and start its DHCP server so a client can join and load the provisioning page.
// Returns 0 on success.
//
int NetWifiApUp(void);

//
// Tear the access point down: stop DHCP server, role-down, remove the AP netif.
//
void NetWifiApDown(void);

//
// Live switch from the setup AP to the station role: NetWifiApDown() followed by
// NetWifiStaUp().  Returns 0 if the STA connect request was issued.
//
int NetWifiSwitchToSta(const char *pcSsid, const char *pcPass);

//
// Non-zero while the setup access point is the active role.
//
int NetWifiIsAp(void);

//
// Provisioning scan support.  NetWifiScanCache() refreshes the cached SSID list
// (brings the STA role up, scans, caches, drops the role) and must be called
// before NetWifiApUp(), since scanning needs the station role.  The web UI reads
// the cache back through NetWifiScanCount()/NetWifiScanGet() to populate the
// wifi.shtml SSID dropdown.  Entries are ordered strongest-signal first.
//
void NetWifiScanCache(void);
int  NetWifiScanCount(void);
bool NetWifiScanGet(int iIndex, char *pcSsid, int iSsidLen, int8_t *pi8Rssi);

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
