//*****************************************************************************
//
// netbiosns.h - Minimal NetBIOS Name Service (NBNS) responder.
//
// Answers NetBIOS name queries for this device's client ID so that a Windows
// host on the same LAN can reach the board by name, e.g. "http://m35/",
// without knowing its DHCP-assigned IP address.
//
//*****************************************************************************

#ifndef __NETBIOSNS_H__
#define __NETBIOSNS_H__

//*****************************************************************************
//
//! Initializes the NetBIOS name responder.
//!
//! Snapshots the configured client ID as this node's NetBIOS name (uppercased,
//! truncated to 15 characters) and opens a UDP listener on port 137.  Call once
//! after lwIP is initialized (the IP address is read at query time, so DHCP need
//! not have bound yet).
//
//*****************************************************************************
extern void NetbiosnsInit(void);

#endif // __NETBIOSNS_H__
