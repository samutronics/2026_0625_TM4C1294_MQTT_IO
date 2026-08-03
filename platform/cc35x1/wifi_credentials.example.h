//*****************************************************************************
//
// wifi_credentials.example.h (CC35x1) - template for the git-ignored
// wifi_credentials.h.
//
// Copy this file to "wifi_credentials.h" in the same directory and set your
// station SSID/passphrase.  wifi_credentials.h is excluded from git (see the
// repo .gitignore) so real credentials are never committed; main.c includes it.
//
// (Runtime/provisioning-backed credentials are a later step; for now the station
// SSID/pass are compile-time.  The shared config carries the MQTT broker, not
// Wi-Fi, since the TM4C build is wired for Ethernet.)
//
//*****************************************************************************

#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

#define WIFI_SSID       "your-ssid"
#define WIFI_PASS       "your-passphrase"

#endif // WIFI_CREDENTIALS_H
