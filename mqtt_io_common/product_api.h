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
// Config store - the foundation owns the store engine (EEPROM/CRC and the
// foundation records: broker, NTP, OTA flag).  The product owns its own schema
// records and loads / resets them through these hooks, so the foundation store
// never enumerates the product schema (decision 15).
//*****************************************************************************

// Load (and default / migrate) the product's config records.  The foundation's
// ConfigInit() calls this AFTER its own records are loaded and BEFORE
// product_init().  Implementations use the foundation store primitives
// (ConfigCRC32 + the PAL storage layer) to persist a product-owned region.
void product_config_load(void);

// Invalidate the product's config records (zero their magic words) so the next
// boot falls back to product defaults.  Called by ConfigFactoryReset().
void product_config_factory_reset(void);

//*****************************************************************************
// MQTT - foundation owns the client, connect/edge handling, availability/LWT
// and the command subscription; the product owns application semantics.
//*****************************************************************************

// The MQTT session has (re)connected: publish discovery + retained state and
// subscribe to product command topics.
void product_on_connect(void);

// An incoming MQTT message on a subscribed topic.  The topic is length-delimited
// (topic_len), NOT NUL-terminated: the foundation's MQTT client delivers it as a
// slice of the RX buffer immediately followed by the payload (see mqtt_client.c).
void product_on_mqtt(const char *topic, uint16_t topic_len,
                     const uint8_t *msg, uint16_t msg_len);

// NOTE: the web hooks (product_web_register / product_ssi_handler) live in
// product_web.h, because they reference lwIP's tCGI type.  Keeping them out of
// this header lets non-web foundation files (entry/super-loop) include the
// lifecycle/MQTT hooks without pulling in the platform-conditional httpd.h.

#ifdef __cplusplus
}
#endif

#endif // PRODUCT_API_H
