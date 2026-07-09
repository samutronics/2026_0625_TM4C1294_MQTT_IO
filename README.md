# TM4C1294 MQTT IO

Firmware for the **TI EK-TM4C1294XL Connected LaunchPad** (TM4C1294NCPDTI, Cortex-M4F with on-chip
10/100 Ethernet) acting as the controller on a **field-I/O control board**. It:

- Obtains an IP address via **DHCP**.
- Serves a **web page** to configure a remote **MQTT broker** (host, port, credentials, topic base)
  and the **I/O hardware layout**, with settings persisted in the on-chip **EEPROM**.
- Connects to the broker over **plain MQTT (port 1883)** and exposes the control board's field I/O
  over MQTT (with Home Assistant auto-discovery).

## Hardware

The LaunchPad plugs into a **control board** (via the EV300E connector) that provides galvanic
isolation and drives two independent daisy chains:

- **Input boards** — 16 digital inputs each, read through cascaded **SN65HVS882** serializers over
  **SSI0**. The number of daisy-chained devices is **web-configurable** (each device = 8 inputs).
- **Output boards** — 16 relays each, driven through cascaded **DRV8860** low-side drivers over
  **SSI1**.

The full pin map, chain protocols and connector pinouts are documented in
**[HARDWARE.md](HARDWARE.md)** (traced from the schematics in [`pdf/`](pdf/)). The onboard
LaunchPad buttons (SW1/SW2) and LEDs (D1/D2) are **no longer used** — they were retired to free
their pins for the field interface.

## Architecture

Bare-metal **lwIP 1.4.1** (raw/callback API), based on TivaWare's `enet_io` example. No RTOS, no SysConfig.

| Layer | Detail |
|-------|--------|
| TCP/IP | lwIP 1.4.1, DHCP + DNS |
| Web server | lwIP httpd with SSI tags (status) + CGI handler (config form) |
| Config storage | TM4C on-chip EEPROM (magic + CRC validated) |
| MQTT | lwIP 2.x `apps/mqtt` backported onto 1.4.1's raw TCP API |
| Input chain | `din_chain.c` — SN65HVS882 read, configurable device count |
| Output chain | `relay_chain.c` — DRV8860 relay control, configurable device count |

### MQTT topics (base topic configurable)

- `<base>/status` → `online` (retained) / Last-Will `offline`
- `<base>/relay/<n>/set` ← `ON` / `OFF` (control; wildcard subscription)
- `<base>/relay/<n>/state` → `ON` / `OFF` (retained)

Relays are auto-discovered by Home Assistant as `switch` entities under one device.
*(Per-input topics + `binary_sensor` discovery are still to come.)*

## Build prerequisites

- **CCStudio (CCS) 2100** at `C:/ti/ccs2100`.
- **TivaWare for C Series 2.2.0.295** (SW-TM4C) installed at `C:/ti/TivaWare_C_Series-2.2.0.295`.

Build from the command line with the CCS gmake and TI Arm Clang toolchain:

```
cd mqtt_io/Debug
"C:/ti/ccs2100/ccs/utils/bin/gmake.exe" -j4
```

If the web form (`fs/index.shtml`) is changed, regenerate the compiled FS image:

```
"C:/ti/TivaWare_C_Series-2.2.0.295/tools/bin/makefsfile.exe" -i fs -o io_fsdata.h -r -h -q
```

## Status

Working on hardware: DHCP, the web config page with EEPROM persistence (survives reboot), and the
MQTT connection lifecycle (`status` online/offline). The **DRV8860 relay output chain** is
implemented — 16 relays are controllable over MQTT and auto-discovered by Home Assistant, with the
device count configurable. The **SN65HVS882 input chain** is scanned (device count configurable);
publishing inputs over MQTT is the next step. Both chain drivers are currently bit-banged (not yet
hardware SSI) and their channel-order mapping should be confirmed on hardware.

**MQTT is confirmed working against a local Mosquitto broker.** Against the public
`test.mosquitto.org` the connection is unreliable — it connects but is frequently aborted
(lwIP `ERR_ABRT`, logged as `connection error (-10)`) and then retries, which appears to be
public-broker rate limiting rather than a firmware fault. Point the config at a local broker
for stable operation.
