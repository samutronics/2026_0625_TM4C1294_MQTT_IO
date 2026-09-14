//*****************************************************************************
//
// product_api.h - the foundation<->product contract (Plan 11).
//
// The foundation (connectivity core: MQTT client, Wi-Fi/SoftAP, OTA, web shell,
// config store, SNTP/NetBIOS, PAL, per-MCU platform ports) owns main() and the
// super-loop.  A product (e.g. products/home_auto) implements the hooks below;
// the foundation calls them at fixed points.  A no-op implementation of every
// hook lets the foundation build + run standalone (foundation_demo_* projects).
//
// Design + rationale: docs/FOUNDATION_PRODUCT_API_DESIGN.md
//
// NOTE: the web hooks reference lwIP's tCGI type.  The httpd header path is
// platform-conditional (lwip/apps/httpd.h on CC35x1, httpserver_raw/httpd.h on
// TM4C), so THIS header does not include it: the translation unit that uses the
// web hooks must include the correct httpd.h before this header.
//
//*****************************************************************************

#ifndef PRODUCT_API_H
#define PRODUCT_API_H

#include <stdint.h>
#include <stdbool.h>

#include "product_config.h"

#ifdef __cplusplus
extern "C" {
#endif

//*****************************************************************************
// Lifecycle - called by the foundation's main()/super-loop.
//*****************************************************************************

// One-time product bring-up, after the config store is initialised and before
// the network starts.  Register app state, load product config records.
void product_init(void);

// Super-loop tick.  elapsed_ms is the time since the previous call.
// CC35x1: the foundation calls this UNDER LOCK_TCPIP_CORE, because a tick may
// MQTT-publish (see the cc35x1-corelock-publish rule).
void product_poll(uint32_t elapsed_ms);

//*****************************************************************************
// MQTT - foundation owns the client, connect/edge handling, availability/LWT
// and the command subscription; the product owns application semantics.
//*****************************************************************************

// The MQTT session has (re)connected: publish discovery + retained state and
// subscribe to product command topics.
void product_on_connect(void);

// An incoming MQTT message on a subscribed topic.
void product_on_mqtt(const char *topic, const uint8_t *msg, uint16_t len);

//*****************************************************************************
// Web - foundation owns httpd + the base tabs (Status/Settings/Wi-Fi/OTA); the
// product contributes its own CGI handlers and SSI tags, merged by the
// foundation at httpd registration.
//*****************************************************************************

typedef struct
{
    const tCGI  *cgis;         // product CGI handlers (e.g. /iocfg.cgi, /control.cgi)
    int          num_cgis;
    const char **ssi_tags;     // product SSI tag names, appended after the foundation's
    int          num_ssi_tags;
} product_web_reg_t;

// Foundation calls this at httpd init so the product can publish its web tables.
// A no-op product zeroes *reg.
void product_web_register(product_web_reg_t *reg);

// Foundation SSI dispatch routes any tag index >= the foundation's tag count to
// this handler, passing a product-relative index (0 == the product's first tag).
u16_t product_ssi_handler(int product_tag_index, char *pcInsert, int iInsertLen);

#ifdef __cplusplus
}
#endif

#endif // PRODUCT_API_H
