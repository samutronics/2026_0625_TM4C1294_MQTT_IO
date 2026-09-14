# Foundation ↔ Product API design (Plan 11, Phase 0/1 gate)

Companion to [`docs/plans/PLAN_foundation_product_split.md`](plans/PLAN_foundation_product_split.md).
Grounded in a code read of the actual seams (2026-09-14). This is the contract that must exist
**before** the mechanical `mqtt_io_common → iot_foundation` rename, because the rename is trivial
and the real work is cleaving three files (`webui.c`, `mqtt_app.c`, `config.c`) onto these hooks.

## Corrected file classification (supersedes the coarse table in the plan)

The initial per-file table treated whole files as foundation-or-product. The code read shows three
files are **internally mixed** and must be split, and one (`mqtt_app.c`) is *mostly product*:

| File | Reality | Cleave |
|---|---|---|
| `mqtt_client.c` | pure foundation (0 product refs) | move as-is → `iot_foundation/core/` |
| `common/cgifuncs.c` | pure foundation (0 product refs) | move as-is → `iot_foundation/core/` (NOT split) |
| `common/sntp_client.c`, `common/netbiosns.c`, `buildinfo.*` | pure foundation | move as-is |
| `common/mqtt_app.c` | **mostly PRODUCT** — HA discovery + state publish + command dispatch for relays/covers/inputs/temp | thin connect/LWT/subscribe glue → foundation; the rest → `products/home_auto/app/ha_mqtt.c` behind `product_on_connect()` + `product_on_mqtt()` |
| `common/webui.c` | **mixed** — httpd shell + base tabs (foundation) vs. product SSI tags + `/iocfg.cgi`/control CGI | shell + base SSI/CGI → foundation; product SSI/CGI registered via `product_web_register()` |
| `config.c` | **mixed** — EEPROM/CRC/record mechanics (foundation) vs. bindings/modes/shutters/rooms schema (product) | store engine → `iot_foundation/config/`; schema records → product blob (decision 15) |
| `din_chain`, `relay_chain`, `common/{io_scan, output_ctrl, relay_pulse, input_events}` | pure product | move → `products/home_auto/app/` |

## The contract: `iot_foundation/product_api.h`

```c
#ifndef PRODUCT_API_H
#define PRODUCT_API_H
#include <stdint.h>
#include <stdbool.h>
#include "lwip/apps/httpd.h"   /* tCGI, tSSIHandler typedefs */
#include "product_config.h"    /* PRODUCT_NAME, MQTT_BASE_TOPIC_DEFAULT, FW_ID, feature flags */

/* ---- lifecycle: foundation main()/super-loop calls these ---- */
void product_init(void);        /* one-time app bring-up (after config load, before net) */
void product_poll(void);        /* super-loop tick. CC35x1: foundation calls this UNDER
                                   LOCK_TCPIP_CORE — see [[cc35x1-corelock-publish]] */

/* ---- MQTT: foundation owns client/connect/LWT; product owns app semantics ---- */
void product_on_connect(void);  /* (re)connected: publish discovery + retained state, subscribe */
void product_on_mqtt(const char *topic, const uint8_t *msg, uint16_t len); /* incoming command */

/* ---- Web: foundation owns httpd + base tabs; product contributes its tags ---- */
typedef struct {
    const tCGI  *cgis;        int num_cgis;      /* product CGI handlers (/iocfg.cgi, /control.cgi) */
    const char **ssi_tags;    int num_ssi_tags;  /* product SSI tag names, appended after foundation's */
} product_web_reg_t;
void product_web_register(product_web_reg_t *reg);      /* foundation calls at httpd init */
/* Foundation SSI dispatch routes tag indices >= foundation_count here (index is product-relative) */
u16_t product_ssi_handler(int product_tag_index, char *pcInsert, int iInsertLen);

#endif /* PRODUCT_API_H */
```

A **no-op product** (empty `product_init/poll/on_connect/on_mqtt`, `product_web_register` returning
zero entries, `product_ssi_handler` returning 0) lets the foundation compile + link + run standalone
— this is what `foundation_demo_*` uses (Phase 2), and the temporary stub in Phase 1.

## Web SSI/CGI cleave detail (the fiddly part)

lwIP httpd uses a **flat tag-name array + one handler switched on index**. Today `webui.c` has ~40
SSI tags interleaving foundation (`mqhost`, `mqstatus`, `fwver`, `ntp*`, `ota*`, `wifi*`, …) with
product (`mqdin`, `mqrelay`, `iotypes`, `innames`, `outstates`, `shutters`, `rmnames`, …).

Cleave rule:
1. Foundation owns tags `[0 .. F)` and its own SSI handler for that range.
2. At `product_web_register()`, foundation concatenates the product tag names after its own, so the
   registered array is `foundation_tags ++ product_tags`; product tags occupy `[F .. F+P)`.
3. Foundation's SSI handler dispatches any index `>= F` to `product_ssi_handler(index - F, …)`.
4. CGI: foundation registers its base handlers + the product's `reg->cgis` in one `http_set_cgi_handlers`
   call. `/iocfg.cgi` and control handlers move to `products/home_auto/web/product_web.c`.

## MQTT cleave detail

- **Foundation keeps:** MQTT client bring-up (`mqtt_client.c`, already), connect-edge detection,
  base-topic string build, availability/LWT publish, subscribe to the command wildcard, and routing
  each incoming message to `product_on_mqtt()`. Exposes `MQTTClientPublish()` (already foundation).
- **Product gets (from today's `mqtt_app.c` → `products/home_auto/app/ha_mqtt.c`):** all
  `MQTTAppPublish{Relay,Cover,Input,Temp}{State,Discovery}` + the topic parsers +
  the incoming command dispatch to `OutputCtrlCommand`/`OutputCtrlShutter`. The post-connect
  discovery+state sequencer becomes `product_on_connect()`; the incoming callback body becomes
  `product_on_mqtt()`.

## Config cleave detail (decision 15)

- **Foundation `config/` engine:** EEPROM/flash read+write, CRC, record framing, and the
  foundation records (Wi-Fi creds, MQTT host/port/user/pass/topic, device name, NTP, OTA).
- **Product blob:** the binding table, per-output modes/timeouts, shutter table, room/zone
  assignments, input/output/room names — owned by `products/home_auto/`, stored via a reserved
  namespaced blob the foundation engine persists opaquely. The one-time migration helpers
  (`ConfigOutputMigrateV1`, etc.) move with the product schema.

## Open item before Phase 1
- **Locate `main()`** (grep found no `int main` in `mqtt_io_tm4c1294/`/`platform/cc35x1/`) — confirm
  the real entry point (likely `enet_io.c` on TM4C, SDK `main_freertos.c` on CC35x1) so decision 13
  ("foundation owns `main()`") can be wired. The super-loop that will call `product_poll()` lives there.

## Recommended Phase ordering (revised from the spike)
1. **Design freeze:** this doc — `product_api.h` + the three cleaves. (now)
2. **Cleave in place (still `mqtt_io_common/`, CCS open OK):** introduce `product_api.h`, split
   `mqtt_app.c`/`webui.c`/`config.c` into foundation + product-hook halves, keeping both builds
   green — no dir rename yet, so no CCS file-lock problem. This is the bulk of the risk and can be
   done incrementally under live CCS.
3. **Mechanical rename (CCS CLOSED):** `git mv mqtt_io_common iot_foundation` + path find/replace
   across the ~8 files, reopen CCS, build. (Blocked under live CCS — see spike finding 1.)
4. Foundation demo projects, emeter stub, docs — per the plan.

This ordering front-loads the hard refactor while CCS can still build-verify it, and defers the
lock-sensitive rename to a single closed-CCS step.
