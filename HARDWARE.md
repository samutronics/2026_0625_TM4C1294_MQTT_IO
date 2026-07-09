# Hardware Reference — Control / Input / Output Boards

Extracted from the schematics in [`pdf/`](pdf/):

| File | Board | Date/Rev |
|------|-------|----------|
| `ControlBoardSch.pdf` | Control board (MCU carrier + isolation hub) | 2014-11-29 |
| `InputBoardSCH.pdf`   | Digital input board (16 ch)                | 2015-03-22 |
| `OutputBoardSch.pdf`  | Relay output board (16 ch)                 | 2015-03-22 |

This document is the reference for implementing the hardware drivers (input scan, relay
control, RS-485, SD card). **Pin-level MCU assignments that could not be resolved from the
PDF text are flagged `⚠ CONFIRM`** — verify these against the schematic net-trace (or a
continuity check) before writing the pinmux.

---

## 1. System topology

```
 EK-TM4C1294XL LaunchPad
        │  (plugs into EV300E, a 2×49 header carrying every TM4C GPIO,
        │   plus the 4× 2×10 BoosterPack headers X6..X9)
        ▼
 ┌─────────────────────────── CONTROL BOARD ───────────────────────────┐
 │  • Power: +24V field in → +5V (TPS54332 buck), +3V3 (MCU),           │
 │           5V_ISO (Meanwell NSD05 isolated DC/DC)                     │
 │  • Galvanic isolation barrier: DGND (MCU side) │ GND/PGND (field)    │
 │      - 3× ISO7241 quad digital isolators (SPI/latch/enable/fault)    │
 │      - 1× ISO3086 isolated RS-485 transceiver                        │
 │  • microSD socket on SSI3 (quad SSI)                                 │
 └───────┬─────────────────────────────────────────┬──────────────────┘
         │ P300  "Input"  (2×5)                     │ P301/P302 (2×5)
         │ CLK,CE,LD,SIP,SOP,+5V,+24V,GND           │ RELAY_{Din,Dout,CLK,LATCH,ENABLE},nFAULT,+24V,GND
         ▼                                          ▼
  ┌──────────────┐   daisy chain            ┌──────────────┐   daisy chain
  │ INPUT BOARD 1│──► INPUT BOARD 2 ──► ... │ OUTPUT BOARD1│──► OUTPUT BOARD2 ──► ...
  │ 16 inputs    │  (SOP→SIP)               │ 16 relays    │  (DOUT→DIN)
  └──────────────┘                          └──────────────┘
```

- **1 or more input boards** are daisy-chained; **1 or more output boards** are daisy-chained.
  Both chains are independent shift registers — total bit count = `16 × N_boards`.
- Everything field-side is **galvanically isolated** from the MCU. The MCU only ever touches
  the ISO7241 / ISO3086 near-side pins.

---

## 2. Control board — the MCU interface

### 2.1 Power & isolation domains

| Rail | Source | Domain | Notes |
|------|--------|--------|-------|
| +24V | External field supply (J301) | field/power | Drives relays + input loop supply; TVS `D304` (1500W/39V) protected |
| +5V  | `U304` TPS54332 buck from +24V | field logic | `L300` 2.2 µH |
| +3V3 | LaunchPad / regulator | **MCU (DGND)** | Logic level for all MCU I/O |
| 5V_ISO | `PS300` Meanwell NSD05 isolated DC/DC | **isolated field** | Powers VCC2 side of the ISO7241/ISO3086 |

**Two ground nets straddle the barrier: `DGND` (MCU side) and `GND`/`PGND` (field side).
Never bridge them.** All decoupling on the VCC2 (field) side of the isolators must stay ≤2 mm
from the pins (per schematic note).

### 2.2 Isolators

| Ref | Part | Function |
|-----|------|----------|
| U301, U302, U303 | ISO7241 (quad digital, 3-fwd/1-rev) | Isolate Input-chain + Output-chain control/data/fault |
| U300 | ISO3086 | Isolated RS-485 transceiver (A/B/Y/Z, DE/RE) |

### 2.3 microSD card — **SSI3 (optional, likely unused)**

> **Decision (2026-07):** the SD card is **probably not needed**. This section is kept for
> reference. If SD is dropped, **SSI3 (PQ0–PQ3, PP0/PP1) is free** for other use.

The microSD socket (`J302`, SDAMB push-push) is wired to the TM4C **quad SSI3**. Resistor
options `R318–R325` select **SPI mode** vs native **SD 4-bit mode** (only one populated set).

| Signal | TM4C pin | SPI-mode role | SD-mode role |
|--------|----------|---------------|--------------|
| SSI3Clk    | **PQ0** | CLK  | CLK |
| SSI3Fss    | **PQ1** | CS   | — |
| SSI3XDAT0  | **PQ2** | DI (MOSI) | CMD |
| SSI3XDAT1  | **PQ3** | DO (MISO) | DAT0 |
| SSI3XDAT2  | **PP0** | — | DAT1/DAT2 |
| SSI3XDAT3  | **PP1** | — | DAT3 |

> Use **SPI mode** (FatFs `diskio` over SSI). Card-detect/write-protect are on the socket's
> mechanical switch pins.

### 2.4 Field-side connectors (control → chains)

| Header | Purpose | Pins carried |
|--------|---------|--------------|
| `P300` "Input" (2×5) | To input-board chain | `CLK`, `CE`, `LD`, `SIP`, `SOP`, +5V, +24V, GND |
| `P301`, `P302` (2×5) | To output-board chain | `RELAY_Din`, `RELAY_Dout`, `RELAY_CLK`, `RELAY_LATCH`, `RELAY_ENABLE`, `nFAULT`, +24V, GND |
| `J300` | RS-485 (isolated) | A/B (+ DE/RE handled on-board via U300) |
| `J301` | +24V / PGND power in | — |

### 2.5 MCU signal nets — **RESOLVED** (traced from EV300E, `ControlBoardSch.pdf`)

Every functional net was traced from its EV300E (`X11`) connector pin back to the TM4C port,
and each lands cleanly on the expected peripheral. The chains use **two separate hardware SSI
modules** (input = SSI0, output = SSI1), the SD card uses SSI3, and RS-485 uses UART0.

**Input chain → SSI0 (RX) + GPIO**

| Net | TM4C pin | X11 | Peripheral role | Drives (SN65HVS882) |
|-----|----------|-----|-----------------|---------------------|
| `Input_SPI_clk`  | **PA2** | X11-6  | SSI0CLK          | `CLK` |
| `Input_SPI_CS`   | **PA3** | X11-8  | SSI0FSS (or GPIO)| `CE` (clock enable, active-low) |
| `Input_SPI_MISO` | **PA5** | X11-12 | SSI0XDAT1 (RX)   | `SOP` (chain data → MCU) |
| `Input_Latch`    | **PH0** | X11-9  | GPIO             | `LD` (load/latch, active-low) |
| *(unused)*       | PA4     | X11-10 | SSI0XDAT0 (TX)   | n/c — read-only chain, no MOSI |

**Output chain → SSI1 (full-duplex) + GPIO**

| Net | TM4C pin | X11 | Peripheral role | Drives (DRV8860) |
|-----|----------|-----|-----------------|------------------|
| `OUTPUT_SPI_CLK`  | **PB5** | X11-7  | SSI1CLK        | `CLK` |
| `OUTPUT_SPI_MOSI` | **PE4** | X11-22 | SSI1XDAT0 (TX) | `DIN`  (relay data out) |
| `OUTPUT_SPI_MISO` | **PE5** | X11-24 | SSI1XDAT1 (RX) | `DOUT` (fault/echo readback) |
| `OUTPUT_SPI_CS`   | **PB4** | X11-5  | SSI1FSS / GPIO | `LATCH` (pulse after full frame — drive as **GPIO**, not FSS) |
| `REL_EN`          | **PH2** | X11-13 | GPIO           | `ENABLE` (global output enable) |
| `Relay_Fault`     | **PH1** | X11-11 | GPIO input (pull-up) | `nFAULT` (open-drain, wired-OR across chain) |

**RS-485 → UART0 + GPIO**

| Net | TM4C pin | X11 | Peripheral role | Drives (ISO3086) |
|-----|----------|-----|-----------------|------------------|
| `RS485_Rx` | **PA0** | X11-74 | U0RX  | `R` |
| `RS485_Tx` | **PA1** | X11-76 | U0TX  | `D` |
| `RS485_DE` | **PK7** | X11-59 | GPIO  | `DE` (driver enable, active-high) |
| `RS485_RE` | **PK6** | X11-63 | GPIO  | `/RE` (receiver enable, active-low) |

> ### RS-485 — deferred (2026-07)
> **RS-485 is out of scope for now.** When it is picked up, note the conflict: RS-485 is on
> UART0 (PA0/PA1) — the same UART the firmware uses for the ICDI backchannel console
> (`enet_io.c:635` `UARTStdioConfig(0, …)`, `pinout.c:88` `GPIO_PA0_U0RX`/`GPIO_PA1_U0TX`). Both
> cannot run at once; at that point either move the console to another UART or drop it. Until
> then the console keeps UART0 and PA0/PA1/PK6/PK7 stay unused by firmware.

> **Freed pins (2026-07 decisions):** LaunchPad buttons (PJ0/PJ1) and LEDs (PN0/PN1) are being
> retired, and the SD card (SSI3: PQ0-3, PP0/1) is likely dropped — all available for reuse.
> None of them are needed by the input/output/RS-485 interface above.

---

## 3. Input board — 16-channel digital input

### 3.1 Devices

Two **`SN65HVS882`** 8-channel digital-input serializers (`U7`, `U8`) → **16 inputs/board**
(`IN1..IN16`). Each channel: series resistor + `10 nF` RC filter, protection diode (`D1..D16`),
and a status LED (`DS2..DS17`). `DS1` = power LED. Field inputs are 24 V-class, current-limited
(RLIM), with configurable debounce.

**SN65HVS882 relevant pins**

| Pin | Name | Role |
|-----|------|------|
| IP0..IP7 | Inputs | 8 field inputs (parallel load) |
| `LD`  | Load  | Latch parallel input state into shift register (active-low pulse) |
| `CE`  | Clock enable | Gates shifting (active-low) |
| `CLK` | Clock | Shift clock |
| `SIP` | Serial In Port | Cascade input (from upstream device's SOP) |
| `SOP` | Serial Out Port | Cascade output (serial data toward MCU) |
| DB0, DB1 | Debounce select | Input filter time config |
| RLIM | Current limit | Sets per-channel input current |
| TOK  | — | Timeout/status clock |

`U7` and `U8` are cascaded on-board (SOP→SIP) to form one 16-bit register.

### 3.2 Read protocol (per chain of N boards)

1. Pulse **`LD`** low→high to latch all inputs across every device simultaneously.
2. With **`CE`** asserted, issue `16 × N` **`CLK`** edges, sampling **`SOP`** (→ `Input_SPI_MISO`)
   on each — this is a plain shift-register read; can be done with an SSI RX or bit-bang.
3. First bits out correspond to the device nearest the MCU end of the chain. Establish the exact
   bit→(board,channel) order empirically on first bring-up (toggle one known input).

### 3.3 Connectors

| Ref | Type | Purpose |
|-----|------|---------|
| `P1` "Input" (2×5) | chain **in** (from control board / previous board) | CLK, CE, LD, SIP, SOP, +5V, +24V, GND |
| `P2` "Output" (2×5) | chain **out** (to next input board) | same signals, SOP/SIP passed along |
| `J1`, `J2` (8-pos) | Field input terminals | IN1..IN8, IN9..IN16 |
| `P3`/`J3` | +24V / PGND | Board power |

---

## 4. Output board — 16-channel relay output

### 4.1 Devices

Two **`DRV8860`** 8-channel serial low-side drivers (`U200`, `U201`) → **16 relays/board**
(`REL200..REL215`). Relay coils run from **+24V** (`VM`). Board power LED `DS200`.

**DRV8860 relevant pins**

| Pin | Name | Role |
|-----|------|------|
| OUT1..OUT8 | Outputs | Low-side relay coil drivers |
| `DIN`  | Data in | Serial data in (from upstream DOUT / control board) |
| `CLK`  | Clock | Shift clock |
| `LATCH`| Latch | Transfer shift register → output latch |
| `DOUT` | Data out | Cascade out (also fault-status readback) |
| `ENABLE` | Enable | Output enable (global) |
| `nFAULT` | Fault | Open-drain, wired-OR across chain (active-low) |
| `VM` | Supply | +24V coil supply |

`U200`/`U201` cascaded on-board (DOUT→DIN) → one 16-bit register.

### 4.2 Write protocol (per chain of N boards)

1. Shift `16 × N` bits into **`DIN`** (`RELAY_Din`), MSB first, on **`CLK`** edges — last board's
   data first (it's furthest down the DOUT→DIN chain). SSI TX or bit-bang.
2. Pulse **`LATCH`** to transfer all shift registers to outputs simultaneously.
3. Hold **`ENABLE`** high to energize outputs.
4. Monitor **`nFAULT`** (needs pull-up); on fault, clock data back out on **`DOUT`**
   (`RELAY_Dout` → `OUTPUT_SPI_MISO`) to read per-channel fault status (see DRV8860 datasheet
   fault-readout sequence). DRV8860 also supports per-channel PWM if needed later.

### 4.3 Connectors

| Ref | Type | Purpose |
|-----|------|---------|
| `P200` (2×5) | chain **in** (from control board / previous board) | RELAY_{Din,Dout,CLK,LATCH,ENABLE}, nFAULT, +24V, GND |
| `P201` (2×5) | chain **out** (to next output board) | same, Din/Dout passed along |
| `J201`, `J202` (Wago 2365-408, 8-pos) | Relay contact terminals | 16 relay outputs |
| `J200` | +24V / PGND | Board power |

---

## 5. Driver implementation checklist

Suggested module layout (extends the existing `io.c` / `mqtt_app.c`):

- [x] **`board_pins.h`** — resolved §2.5 mapping captured as named macros (SSI0 = input,
      SSI1 = output, PH0/PH1/PH2 = LD/nFAULT/EN; RS485 pins under `#if 0`).
- [x] **Retire onboard buttons/LEDs** — SW1/SW2 publish and D1/D2 control removed from
      `io.c`/`mqtt_app.c` + their HA discovery. `mqtt_app` now publishes only `status`
      online/offline. Frees PJ0/PJ1. *(PN0/PN1 still driven by the legacy enet_io web LED/anim
      demo in `io.c`/`io_fs.c` — reclaim when that demo is removed.)*
- [x] `din_chain.c` — SN65HVS882 read: pulse `LD` (PH0) → clock `devices*8` bits, capture on
      PA5 → packed bitmap. **Bit-banged** on PA2/PA3/PA5 for bring-up (not yet hardware SSI0).
      Device count is web-configurable + EEPROM-persisted (`ui8DinDevices`). Current wiring
      logs input changes over UART; MQTT publish of inputs still TODO. *(Verify bit↔channel
      order and the sample-vs-clock edge on hardware — open questions 2/3 below.)*
- [x] `relay_chain.c` — DRV8860 write: shift `devices*8` bits out DIN (PE4), pulse `LATCH`
      (PB4), drive `ENABLE` (PH2); `nFAULT` (PH1, pull-up) read + logged. **Bit-banged** on
      PB5/PE4/PB4 for bring-up (not yet hardware SSI1). Device count web-configurable
      (`ConfigGet/SetRelayDevices`, default 2 = 16 relays). Relays exposed over MQTT as HA
      `switch` entities: `<base>/relay/<n>/set` (wildcard subscribe) + retained
      `<base>/relay/<n>/state`. DOUT (PE5) per-channel fault readback still TODO.
      *(Verify relay-index ↔ physical-output order on hardware — open question 3.)*
- [ ] `rs485.c` — **DEFERRED** (out of scope for now). UART0 (PA0/PA1) + DE (PK7)/RE (PK6) via
      ISO3086; protocol TBD. Requires moving the console off UART0 first (§2.5).
- [ ] `sdcard.c` — **optional / deprioritized** (§2.3). SSI3 (PQ0-3). Only if SD logging/config
      is wanted; otherwise leave SSI3 unused.
- [x] **Input device count config:** `ui8DinDevices` (cascaded SN65HVS882 count) is set on the
      web form and persisted in EEPROM (clamped 1..16, default 2). Output-board count still TODO.
- [~] **MQTT surface:** relays are live — one HA `switch` per relay at
      `<base>/relay/<n>/set|state`. Still TODO: `binary_sensor` per input
      (`<base>/in/<n>`) and a fault sensor for `nFAULT`.

### Open questions to settle at bring-up
1. ~~TM4C GPIO/peripheral per net~~ — **RESOLVED** (§2.5): input=SSI0, output=SSI1.
2. Bit ↔ (board, channel) ordering for both chains (verify by toggling one known point).
3. SN65HVS882 debounce (DB0/DB1) and RLIM values as fitted — affects input timing.
4. Confirm `OUTPUT_SPI_CS` (PB4) and `Input_SPI_CS` (PA3) are driven as plain GPIO where the
   shift-register `LATCH`/`CE` timing doesn't match SSI FSS auto-pulsing.

*(RS-485 deferred — see §2.5; protocol/role and the UART0 console move are parked until then.)*
```
