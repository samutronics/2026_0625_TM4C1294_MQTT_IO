# TM4C1294 MQTT IO

Firmware for the **TI EK-TM4C1294XL Connected LaunchPad** (TM4C1294NCPDTI, Cortex-M4F with on-chip
10/100 Ethernet) acting as the controller on a **field-I/O control board**. It:

- Obtains an IP address via **DHCP**, and is reachable **by name** — `http://<clientid>/`
  (e.g. `http://m35/`) — through a built-in **NetBIOS** responder, so you don't need to know the
  DHCP-assigned IP.
- Serves a **web UI** to configure a remote **MQTT broker** (host, port, credentials, topic base)
  and the **I/O layout** (inputs, outputs, per-output modes, shutters, rooms), with settings
  persisted in the on-chip **EEPROM**.
- Connects to the broker over **plain MQTT (port 1883)** and exposes the board's field I/O over
  MQTT with **Home Assistant auto-discovery** (relays as `switch`, shutters as `cover`).
- Can be **updated over the air** — firmware upload from the web Tools page, no JTAG/UART needed in
  the field.

## Web UI

Four pages served by the lwIP httpd (all responses are sent no-cache so a browser never serves a
stale page across an OTA update):

- **Control** — the landing page (`/`). Day-to-day operation of **lights** (named relays) and
  **shutters**, **grouped by room**, with live state from a 1 s poll.
- **I/O Config** — sub-tabs **Rooms / Outputs / Shutters / Inputs**. Name and assign outputs, pick a
  per-output **mode** (Standard / Timed / Shutter), define shutters (Up/Down relay pair + travel
  time), group everything into rooms, and bind physical inputs to outputs/shutters. A single
  **Save** persists all of it.
- **Settings** — MQTT broker, network and device settings.
- **Tools** — EEPROM backup/restore (JSON) and **OTA firmware update**.

## Reaching the device

- **By name (Windows / SMB LAN):** `http://<clientid>/` — the NetBIOS responder (`netbiosns.c`,
  UDP port 137) answers name queries with the board's current IP. Same subnet only.
- **By name (router DNS):** the DHCP hostname is set to the client ID (sanitized to DNS-legal
  chars), so a router that registers DHCP hostnames also resolves `http://<clientid>/`.
- **By IP:** whatever DHCP assigned.

The client ID is set on the **Settings** page; both the NetBIOS name and the DHCP hostname track it
(applied at boot).

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
| TCP/IP | lwIP 1.4.1, DHCP + DNS; NetBIOS name responder (`netbiosns.c`) + DHCP hostname = client ID |
| Web server | lwIP httpd, SSI tags (status) + CGI handlers (config / control / OTA); no-cache response headers |
| Config storage | TM4C on-chip EEPROM — multiple magic+CRC-validated records (broker/names, output modes + shutters, rooms) |
| OTA | Web firmware upload → staged in a separate flash region → CRC-checked → applied, with a brick-safety pending-flag handshake |
| Output control | `output_ctrl.c` — Standard / Timed / Shutter modes; shutter FSM with a 500 ms direction interlock + travel-time auto-stop |
| Input chain | `din_chain.c` — SN65HVS882 read, configurable device count; drives local input→output/shutter bindings |
| Output chain | `relay_chain.c` — DRV8860 relay control, configurable device count |
| MQTT | lwIP 2.x `apps/mqtt` backported onto 1.4.1's raw TCP API; Home Assistant auto-discovery |

### MQTT topics (base topic configurable)

- `<base>/status` → `online` (retained) / Last-Will `offline`
- `<base>/relay/<n>/set` ← `ON` / `OFF` (control; wildcard subscription)
- `<base>/relay/<n>/state` → `ON` / `OFF` (retained)
- `<base>/cover/<n>/set` ← `OPEN` / `CLOSE` / `STOP`
- `<base>/cover/<n>/state` → `opening` / `closing` / `open` / `closed` / `stopped`

Relays are auto-discovered by Home Assistant as `switch` entities and shutters as `cover` entities,
under one device; relays that make up a shutter are hidden.
*(Per-input `binary_sensor` discovery is still to come — inputs are scanned and usable for local
bindings, but not yet published over MQTT.)*

## Build prerequisites

- **CCStudio (CCS) 2100** at `C:/ti/ccs2100`.
- **TivaWare for C Series 2.2.0.295** (SW-TM4C) installed at `C:/ti/TivaWare_C_Series-2.2.0.295`.

Build from the command line with the CCS gmake and TI Arm Clang toolchain:

```
cd mqtt_io_tm4c1294/Debug
"C:/ti/ccs2100/ccs/utils/bin/gmake.exe" -j4
```

The post-build step writes a timestamped `mqtt_io_tm4c1294_YYYYMMDDHHMM.bin` (and copies it to
`mqtt_io_tm4c1294.bin`).

If the web UI (`fs/*.shtml`) is changed, regenerate the compiled FS image **first** (from
`mqtt_io_tm4c1294/`), then force a rebuild of the FS object so the new content is linked in:

```
"C:/ti/TivaWare_C_Series-2.2.0.295/tools/bin/makefsfile.exe" -i fs -o io_fsdata.h -r -h -q
rm -f Debug/io_fs.o
```

Flashing is **OTA over Ethernet** (Tools → firmware update); the device reboots into the new image
and EEPROM config is preserved. Keep the previous known-good `.bin` for rollback.

## Status

**Released / running on hardware.** Verified on hardware: DHCP, the web config UI with EEPROM
persistence (survives reboot), the MQTT lifecycle (`status` online/offline + LWT), the **DRV8860
relay output chain** (relays controllable over MQTT, auto-discovered as HA `switch` entities, device
count configurable), the **SN65HVS882 input chain** (scanned, device count configurable, driving
local input→output/shutter bindings), **per-output modes** (Standard / Timed / Shutter with a 500 ms
interlock), **shutters** (up to 32, named, exposed as HA `cover` entities), **rooms + the Control
dashboard**, **OTA firmware update** (round-trip tested), and **name resolution** — reach the board
at `http://<clientid>/` via the NetBIOS responder (and via router DNS through the DHCP hostname).
Both chain drivers are bit-banged (not yet hardware SSI) but are verified on hardware, with
channel-order mapping confirmed.

### Known limitations

- **Router / subnet change:** DHCP is only (re)started on an Ethernet **link** transition. If the
  router or subnet changes while the board is wired through a **switch** (so its link never drops),
  **reboot the board — or briefly unplug/replug its cable** — to force a fresh lease. (An automatic
  gateway watchdog was tried and deliberately removed: the only clean lwIP recovery call,
  `dhcp_release()`, administratively downs the interface with no reliable way back up, which took
  the board off Ethernet until reboot.)
- **Inputs over MQTT:** inputs drive local bindings but are not yet published as HA `binary_sensor`
  (`<base>/in/<n>`).
- **Public MQTT broker:** against `test.mosquitto.org` the connection is unreliable — it connects
  but is frequently aborted (lwIP `ERR_ABRT`, logged as `connection error (-10)`) and then retries,
  which appears to be public-broker rate limiting rather than a firmware fault. Use a **local**
  broker (e.g. Mosquitto) for stable operation.

### Planned

- Publish inputs over MQTT as HA `binary_sensor` (`<base>/in/<n>`).
- Optional DRV8860 per-channel fault readback via DOUT (`nFAULT` edge is already logged).
- Optionally migrate the bit-banged chains to hardware SSI0 (input) / SSI1 (output).
</content>
