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

#endif // __PAL_LWIP_COMPAT_H__
