# Plan: Make the Control Board accept TM4C1294 **or** LP-EM-CC35x1 interchangeably

## Context & goal
The Control Board drives isolated field I/O (SN65HVS882 input chain, DRV8860 relay chain) through
ISO7241 digital isolators. Today only the **TM4C1294 LaunchPad** mates with it, via the big **EV300E**
(CON2×49) connector. The board *also* carries two 40-pin BoosterPack sites — **EV300A+EV300B**
("BoosterPack2") and **EV300C+EV300D** ("BoosterPack1") — the latter originally laid out for a
**CC3200** wireless LaunchPad (its net names are `CC3200_SPI_*`, `CC3200_UART0_*`, `CC3200_I2C_*`,
`CC3200_RESET_OUT`, `CC3200_GPIO`). The **LP-EM-CC35x1** (Cortex-M33) uses the same 40-pin BoosterPack
footprint (P1/P2/P3/P4) and is the CC3200's lineage descendant.

**Goal:** rework the **Control Board only** (trace cuts + jumper wires OK; do not modify either
LaunchPad) so a single board accepts *either* MCU, and configure the pinmux on *both* MCUs so no
electrical misconfiguration occurs regardless of which board is plugged in.

**Key assumption — one MCU at a time.** EV300E and the BoosterPack sites are physically separate, and
A+B vs C+D are separate, so two MCUs are never plugged simultaneously. The electrical risk is therefore
**not** MCU-vs-MCU contention but **each MCU driving a shared isolator net in the wrong direction**.

## Decision: use the **EV300C + EV300D** site
Chosen (per review): it was pre-wired for the CC3200 wireless LaunchPad, so the isolator nets are
already broken out to BoosterPack-standard positions there — the LP-EM-CC35x1 (same footprint) needs
the fewest cuts/jumpers. A+B is raw TM4C GPIO with no wireless-MCU intent.

---

## Reference 1 — Isolator MCU-side nets (FULLY KNOWN, from `pdf/ControlBoardSch.pdf`)
Each net crosses an ISO7241 (U301/U302/U303). **The isolator fixes the direction of each line**, so
*both* MCUs must drive/read each net the same way. This table is the electrical contract.

| Net (Control Board) | Chain role | ISO dir (MCU side) | **Required MCU pin mode** | TM4C port | EV300E pin |
|---|---|---|---|---|---|
| `Input_SPI_clk` | input CLK | MCU→field | **OUTPUT** (push-pull) | PA2 | X11-6 |
| `Input_SPI_CS` (CE) | input chip-enable | MCU→field | **OUTPUT** | PA3 | X11-8 |
| `Input_Latch` (LD) | input load | MCU→field | **OUTPUT** | PH0 | X11-9 |
| `Input_SPI_MISO` (SOP) | input serial data | field→MCU | **INPUT** (hi-Z) | PA5 | X11-12 |
| `OUTPUT_SPI_CLK` | relay CLK | MCU→field | **OUTPUT** | PB5 | X11-7 |
| `OUTPUT_SPI_MOSI` (RELAY_Din) | relay data out | MCU→field | **OUTPUT** | PE4 | X11-22 |
| `OUTPUT_SPI_CS` (RELAY_LATCH) | relay latch | MCU→field | **OUTPUT** | PB4 | X11-5 |
| `REL_EN` (RELAY_ENABLE) | relay output-enable | MCU→field | **OUTPUT** | PH2 | X11-13 |
| `OUTPUT_SPI_MISO` (RELAY_Dout) | relay fault/echo | field→MCU | **INPUT** (hi-Z) | PE5 | X11-24 |
| `Relay_Fault` (nFAULT) | relay open-drain fault | field→MCU | **INPUT + PULL-UP** | PH1 | X11-11 |

→ **7 outputs, 3 inputs.** The three inputs (`Input_SPI_MISO`, `OUTPUT_SPI_MISO`, `Relay_Fault`) are
driven by the isolator's VCC1-side outputs whenever the isolator is enabled — **a CC35x1 pinmux that
sets any of these to OUTPUT would fight the ISO7241 and risk damage.** This is the #1 safety rule.

## Reference 2 — LP-EM-CC35x1 header → DIO (provided) + collision flags
Aligned to standard 40-pin BoosterPack positions (P1=J1 pos 1-10, P2=J2 pos 11-20, P3=J3 21-30, P4=J4 31-40).

| Hdr-Pin | Signal | DIO | ⚠ CC35x1 fixed-function collision |
|---|---|---|---|
| P1-3 | UART0_RX | 4 | |
| P1-4 | UART0_TX | 3 | |
| P1-7 | **SPI_CLK** | **5** | **= XDS110 debug UART TX** (firmware `CONFIG_GPIO_UART2_0_TX`) |
| P1-9 | I2C_SCL | 1 | dedicated I2C |
| P1-10 | I2C_SDA | 2 | dedicated I2C **and** SW1 button / PWM |
| P2-2 | GPIO | 18 | |
| P2-3 | **SPI_CS** | **9** | |
| P2-6 | **SPI_DOUT** | **7** | |
| P2-7 | **SPI_DIN** | **6** | **= XDS110 debug UART RX** (firmware `CONFIG_GPIO_UART2_0_RX`) |
| P2-5 | RESET_OUT | — | tie-off / leave per §Safety |
| P2-10 | GPIO | 15 | |
| P4-9 | GPIO | 16 | |
| P4-10 | GPIO | 17 | UART2_1 TX in current syscfg |
| P2-8 | GPIO | 21 | |
| P2-9 | GPIO | 55 | |
| P2-4 | GPIO | 45 | |
(ADC/AUD/PWM rows on P1/P3/P4 omitted — not needed for the two chains. Full table retained in the
user-supplied pinout.) Reserved elsewhere on-chip (not on these positions but must never be reused):
buttons DIO2/DIO36, RGB LED DIO30/34/35, sensor I2C DIO10/11.

---

## The one remaining unknown — exact isolator-net → EV300C/D pin position
The flattened schematic text lists both the isolator nets and the `CC3200_*` net names but **not their
wire-merges**, and the C/D site has selection jumpers (`JP4`, `JP5`) + resistors `R300–R325` that may
gate routing. **Do not assume** the CC3200 native-SPI carried a specific chain. Close this by
**continuity-buzzing (board unpowered)** from each **ISO7241 VCC1-side pin** (known, silk U301/U302/U303:
`INA/INB/INC` = MCU→field, `OUTD` = field→MCU) out to the EV300C/EV300D header pins. Record which C/D
header position each of the 10 isolator nets lands on, then read the DIO at that position from
Reference 2. That produces the final map. (The `pdf/ControlBoardSch.pdf` is image-only and this
environment can't OCR it; TI's LP-EM-CC35X1 pinout is **SWRU629A** — cite/verify against silkscreen.)

**Fill this table on the bench (one row per isolator net):**

| Isolator net | ISO pin (U30x) | → EV300C/D pin | BoosterPack pos | LP-EM DIO | CC35x1 mode | Rework needed? |
|---|---|---|---|---|---|---|
| Input_SPI_clk | | | | | OUTPUT | |
| Input_SPI_CS | | | | | OUTPUT | |
| Input_Latch | | | | | OUTPUT | |
| Input_SPI_MISO | | | | | INPUT | |
| OUTPUT_SPI_CLK | | | | | OUTPUT | |
| OUTPUT_SPI_MOSI | | | | | OUTPUT | |
| OUTPUT_SPI_CS | | | | | OUTPUT | |
| REL_EN | | | | | OUTPUT | |
| OUTPUT_SPI_MISO | | | | | INPUT | |
| Relay_Fault | | | | | INPUT+PU | |

---

## Electrical-safety rules (both MCUs)
1. **Direction contract (Reference 1).** For every shared net, the CC35x1 SysConfig mode **must match**
   the TM4C: the 7 MCU→field nets = push-pull **Output**, the 3 field→MCU nets = **Input** (nFAULT with
   pull-up). Verify in `.syscfg` before first power-up. A single wrong Output = isolator contention.
2. **Debug-UART collision (DIO5/DIO6).** These are `SPI_CLK`(P1-7) / `SPI_DIN`(P2-7) on the BoosterPack
   **and** the XDS110 backchannel UART in the current firmware. Decide per bench map: either route the
   chain signal that lands there through DIO5/6 and **move the debug UART** to another DIO (or accept no
   backchannel while docked), or place that chain signal on a different C/D position via cut+jumper.
   Whichever: the two functions cannot both own DIO5/6 while docked.
3. **Single 3V3 source.** The BoosterPack site presents 3V3 (pin 1) and 5V (pin 21). The LP-EM-CC35x1
   makes its own 3V3 from USB. Confirm exactly one source drives the 3V3 rail — either power the board
   from the LP (leave the board's 3V3 feed to C/D open) **or** feed the LP from the board (LP's 3V3
   out-jumper removed). Never both (back-feed). Keep DGND (MCU) and field GND/PGND **unbridged** —
   isolation barrier is sacred.
4. **RESET_OUT (P2-5).** Ensure the board isn't holding the CC35x1 in reset and isn't driven against
   the LP's reset. Leave open or pulled per the LP-EM-CC35x1 reset scheme; verify at bring-up.
5. **Reserved CC35x1 pins.** Never assign an isolator net to DIO2/36 (buttons), DIO30/34/35 (RGB LED),
   or DIO10/11 (sensor I2C). If the bench map lands an isolator net on one of these positions, relocate
   it with a cut+jumper to a free-GPIO position.

## Rework procedure (Control Board only)
1. Complete the bench-fill table (§ above) — this is the gate; no cutting before it's filled.
2. For each isolator net whose C/D-position DIO is **usable** (free GPIO, correct direction achievable
   in SysConfig): **no rework** — just assign it in the CC35x1 pinmux (§ next).
3. For each net that lands on a **reserved/colliding** DIO (rules 2 & 5) or on **no usable pin**: cut
   the offending trace at the C/D header and **jumper** the isolator net to a free-GPIO header position.
   Prefer P2/P4 free GPIO (DIO15/16/17/18/21/45/55) for relocations.
4. Resolve the JP4/JP5 selection jumpers + R300–R325 so the isolators are driven from the **C/D site**
   when the CC35x1 is docked (and still from EV300E for the TM4C). If those are hard "either/or" straps,
   document the strap position per MCU.
5. Confirm power/reset straps per rules 3 & 4.

## Firmware / pinmux changes (CC35x1)
- Update the pinmux via the **SysConfig MCP** on `mqtt_io_cc35x1.syscfg` (never hand-edit `.syscfg`):
  reassign `CONFIG_GPIO_DIN_*` / `CONFIG_GPIO_REL_*` to the **confirmed** C/D DIOs, directions/pulls per
  Reference 1. Regenerate `ti_drivers_config.h`.
- Update `platform/cc35x1/board_pins.h` (currently **provisional placeholders** — it says so) to the
  final DIOs; keep the symbol names so `din_chain.c`/`relay_chain.c` need no change.
- If the debug UART is displaced (rule 2), move `CONFIG_GPIO_UART2_0_*` to a spare DIO or gate it.
- TM4C firmware: **unchanged** — its pinmux already matches Reference 1 via EV300E.

## Verification
1. **Unpowered:** bench-fill table complete; continuity-verify every isolator net → chosen C/D pin →
   DIO; confirm DGND/field-GND still isolated (isolators un-bridged); confirm single 3V3 source.
2. **Powered, CC35x1 docked (no field boards):** per-net direction check — scope/meter each of the 7
   output nets toggling from firmware and confirm the 3 input nets are hi-Z/pulled (never driven by MCU).
3. **Chains attached:** run the input scan + relay drive; confirm `din_chain`/`relay_chain` read/actuate
   correctly (bit-order per [[bringup-gotchas]]); nFAULT reads high idle.
4. **Regression:** re-dock the **TM4C** on EV300E → confirm it still works unchanged (proves the rework
   didn't break the EV300E path).
5. Only after 1–4: integrate with the MQTT/HA path.

## Open data to obtain
- The bench-fill map (the gate for everything).
- JP4/JP5 + R300–R325 strap semantics (from a visual `ControlBoardSch.pdf` trace or continuity).
- LP-EM-CC35x1 reset + power-jumper scheme (SWRU629A / silkscreen).
