//*****************************************************************************
//
// product_web.h - foundation<->product WEB hooks (Plan 11).
//
// Split out of product_api.h because these declarations reference lwIP's tCGI
// type, whose httpd header path is platform-conditional (lwip/apps/httpd.h on
// CC35x1, httpserver_raw/httpd.h on TM4C).  Keeping them here means the
// lifecycle/MQTT hooks in product_api.h carry NO lwIP dependency and can be
// included by any foundation translation unit (e.g. the entry/super-loop files).
//
// The including translation unit MUST include the correct httpd.h before this
// header so that tCGI is defined.
//
// Design + rationale: docs/FOUNDATION_PRODUCT_API_DESIGN.md
//
//*****************************************************************************

#ifndef PRODUCT_WEB_H
#define PRODUCT_WEB_H

#include "product_api.h"   // lifecycle/MQTT hooks (no lwIP dependency)

#ifdef __cplusplus
extern "C" {
#endif

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

#endif // PRODUCT_WEB_H
