# TM4C1294 MQTT IO

Firmware for the **TI EK-TM4C1294XL Connected LaunchPad** (TM4C1294NCPDTI, Cortex-M4F with on-chip
10/100 Ethernet) that:

- Obtains an IP address via **DHCP**.
- Serves a **web page** to configure a remote **MQTT broker** (host, port, credentials, topic base),
  with settings persisted in the on-chip **EEPROM**.
- Connects to the broker over **plain MQTT (port 1883)** and **exposes board features over MQTT** —
  v1 publishes the two user pushbuttons (SW1/SW2). The MQTT layer is built to also support
  subscribe/control (LEDs, telemetry) as later additions.

## Architecture

Bare-metal **lwIP 1.4.1** (raw/callback API), based on TivaWare's `enet_io` example. No RTOS, no SysConfig.

| Layer | Detail |
|-------|--------|
| TCP/IP | lwIP 1.4.1, DHCP + DNS |
| Web server | lwIP httpd with SSI tags (status) + CGI handler (config form) |
| Config storage | TM4C on-chip EEPROM (magic + CRC validated) |
| MQTT | lwIP 2.x `apps/mqtt` backported onto 1.4.1's raw TCP API |
| I/O | Buttons SW1=PJ0, SW2=PJ1 (publish); LEDs D1=PN1 (IP), D2=PN0 (MQTT) as status |

### MQTT topics (base topic configurable)

- `<base>/button/sw1` → `PRESSED` / `RELEASED`
- `<base>/button/sw2` → `PRESSED` / `RELEASED`
- `<base>/status` → `online` (retained) / Last-Will `offline`

## Build prerequisites

- **CCStudio (CCS) 2100** at `C:/ti/ccs2100`.
- **TivaWare for C Series 2.2.0.295** (SW-TM4C) installed at `C:/ti/TivaWare_C_Series-2.2.0.295`.

## Status

Early scaffolding. See the implementation plan for the build-out sequence.
