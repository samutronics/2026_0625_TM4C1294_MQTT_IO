//*****************************************************************************
//
// wifi_store.h (CC35x1) - persistent Wi-Fi STA credentials.
//
// A small credentials record (SSID + passphrase) stored in the PAL persistent
// store (the same NVOCMP-backed flash blob config.c uses), at the reserved
// address CFG_WIFI_EEPROM_ADDR.  Survives power-cycle and OTA A/B swap.  Used by
// the SoftAP provisioning flow: the user enters their home-network credentials
// over the setup AP, they are saved here, and the device connects as a station
// on this and every subsequent boot.
//
//*****************************************************************************

#ifndef WIFI_STORE_H
#define WIFI_STORE_H

#include <stdbool.h>

//
// Maximum stored lengths, excluding the NUL terminator: an 802.11 SSID is at
// most 32 octets; a WPA2 passphrase is 8..63 characters.  Buffers passed to
// WifiStoreLoad() must hold at least these many characters plus a NUL.
//
#define WIFI_SSID_MAX   32
#define WIFI_PASS_MAX   63

//
// Load the stored credentials.  pcSsid must have room for >= WIFI_SSID_MAX+1
// bytes and pcPass (if non-NULL) >= WIFI_PASS_MAX+1.  Returns true if a valid
// (magic + CRC) record with a non-empty SSID was found; otherwise returns false
// and sets the buffers to empty strings.
//
bool WifiStoreLoad(char *pcSsid, char *pcPass);

//
// Persist the given credentials (truncated to the max lengths).  A NULL or empty
// password stores an open-network entry.  Returns true on success.
//
bool WifiStoreSave(const char *pcSsid, const char *pcPass);

//
// Invalidate the stored record (zeroes its magic word) so WifiStoreLoad()
// returns false and the device falls back to AP provisioning on the next boot.
//
void WifiStoreClear(void);

//
// True if a valid credentials record is currently stored.
//
bool WifiStoreHas(void);

#endif // WIFI_STORE_H
