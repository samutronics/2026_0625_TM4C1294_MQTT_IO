//*****************************************************************************
//
// product_config.h - compile-time product identity for the foundation.
//
// Plan 11 (foundation/product split): each product supplies this header to
// stamp the foundation with its name, MQTT base topic, firmware id and feature
// flags.  During the in-place cleave this lives in the shared tree next to the
// foundation; it moves to products/<name>/ when the tree is renamed to
// iot_foundation/ (see docs/FOUNDATION_PRODUCT_API_DESIGN.md).
//
// This is the home-auto product's identity (product #1).
//
//*****************************************************************************

#ifndef PRODUCT_CONFIG_H
#define PRODUCT_CONFIG_H

// Human-readable product name (used in web UI / discovery metadata).
#define PRODUCT_NAME            "Field I/O Gateway"

// Default MQTT base topic prefix.  The runtime value remains user-configurable
// via the web UI + config store; this is only the compile-time default.
#define PRODUCT_MQTT_BASE_DEFAULT   "fieldio"

// Feature flags (product opt-in).  Placeholders during the cleave; wired as the
// foundation/product seam is populated.
#define PRODUCT_HAS_SHUTTERS        1
#define PRODUCT_HAS_TEMP_SENSOR     1

#endif // PRODUCT_CONFIG_H
