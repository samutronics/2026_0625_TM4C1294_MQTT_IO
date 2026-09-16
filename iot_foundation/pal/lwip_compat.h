//*****************************************************************************
//
// lwip_compat.h - Normalise the small lwIP raw-API differences between the two
//                 stacks the portable code must build against.
//
//   TivaWare (TM4C)   lwIP 1.4.1
//   SimpleLink SDK    lwIP 2.1.x
//
// lwIP 2.x made the address argument of the udp_recv and dns_found receive
// callbacks 'const ip_addr_t *'; lwIP 1.4.1 uses a non-const pointer.  The same
// callback source must compile on both, so the callback declares its address
// parameter as PAL_LWIP_CADDR* and this header supplies the right qualification.
//
// Both stacks define LWIP_VERSION_MAJOR in lwip/init.h (1.4.1 == 1), so a value
// test — not #ifdef — selects the branch.
//
//*****************************************************************************

#ifndef __PAL_LWIP_COMPAT_H__
#define __PAL_LWIP_COMPAT_H__

#include "lwip/init.h"      // LWIP_VERSION_MAJOR

#if LWIP_VERSION_MAJOR >= 2
#define PAL_LWIP_CADDR      const ip_addr_t
#else
#define PAL_LWIP_CADDR      ip_addr_t
#endif

// IPv4 address of a netif as a u32 (network order), plus a pointer to it.
// lwIP 2.x may enable IPv6 (ip_addr_t becomes a union with no direct .addr
// member); the netif_ip4_* accessors return the IPv4 sub-address on both
// v4-only and dual-stack builds.  lwIP 1.4.1 has no such accessor and its
// ip_addr_t is a bare struct { u32_t addr; }.  (Expanded only at use sites,
// which include lwip/netif.h.)
#if LWIP_VERSION_MAJOR >= 2
#define PAL_NETIF_IP4_U32(nif)   (netif_ip4_addr(nif)->addr)
#define PAL_NETIF_IP4_PTR(nif)   (&netif_ip4_addr(nif)->addr)
#else
#define PAL_NETIF_IP4_U32(nif)   ((nif)->ip_addr.addr)
#define PAL_NETIF_IP4_PTR(nif)   (&(nif)->ip_addr.addr)
#endif

#endif // __PAL_LWIP_COMPAT_H__
