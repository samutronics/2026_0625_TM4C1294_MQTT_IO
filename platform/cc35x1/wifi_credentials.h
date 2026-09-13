//*****************************************************************************
//
// wifi_credentials.h (CC35x1) - compile-time seed credentials.
//
// TRACKED (committed) with a deliberately NON-EXISTENT network so the default
// build never silently joins a real bench AP: an empty credential store seeds
// "dummyAP", which cannot associate, so the device falls through to the
// "MQTT-IO-Setup" access point for provisioning.  Real credentials come from
// SoftAP provisioning (wifi_store) at runtime -- do NOT put a real SSID/pass
// here (this file is committed).  Also handy for the F1 cascade test (bogus
// primary -> real backup).
//
//*****************************************************************************

#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

#define WIFI_SSID       "dummyAP"
#define WIFI_PASS       "dummyPSW"

#endif // WIFI_CREDENTIALS_H
