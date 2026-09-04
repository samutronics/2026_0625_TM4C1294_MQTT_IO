# Plan 4 — Control Board MCU-interchange rework (TM4C1294 ⇄ LP-EM-CC35x1)

**Priority:** Med · **Status:** OPEN, GATED on a bench continuity map · Memory: `mcu-interchange-plan`

## Goal
Rework the Control Board so it accepts either a TM4C1294 LaunchPad (via EV300E) or an LP-EM-CC35x1 (via EV300C+EV300D) interchangeably, with pinmux configured on both MCUs so neither creates an electrical conflict.

## Before you start (read)
- `CCS.md`, `CLAUDE.md`, `MEMORY.md`.
- **Root doc `MCU_INTERCHANGE_PLAN.md`** — the full design (this is the execution wrapper).
- Memory `mcu-interchange-plan`. Hardware refs: `HARDWARE.md`, `docs/pdf/ControlBoardSch.pdf`, both `board_pins.h`.

## HARD GATE — do this first, before ANY trace cut
The exact **isolator-net → EV300C/D pin** mapping is unknown (the flattened schematic hides wire-merges; the C/D site has selection jumpers JP4/JP5 + resistors R300–R325). **Build the 10-row continuity map on the bench (unpowered):** buzz from each ISO7241 VCC1-side pin (U301/U302/U303: INA/INB/INC = MCU outputs, OUTD = MCU input) to the EV300C/D header pins, then read the LP-EM-CC35x1 DIO at that pin from the user-provided P1/P2 table (in `MCU_INTERCHANGE_PLAN.md`). Fill the table. **Do not cut/jumper anything until this table is complete and reviewed by the user.**

## Known inputs (already in MCU_INTERCHANGE_PLAN.md)
- Site chosen: **EV300C+EV300D** (ex-CC3200 BoosterPack1).
- ISO7241 direction contract (fixed): 7 MCU-outputs (Input CLK/CS/LD, Relay CLK/MOSI/LATCH/EN) + 3 MCU-inputs (Input MISO, Relay MISO/Dout, nFAULT). Both MCUs must match these directions.
- TM4C ports (EV300E) and LP-EM-CC35x1 P1/P2→DIO map are documented in the root plan.
- Collisions to avoid on CC35x1: DIO5/6 (debug UART), DIO1/2 (I2C + SW1), DIO30/34/35 (RGB LED).

## Execution (only AFTER the gate + user approval)
1. For each isolator net, place it on the chosen C/D pin; where the CC35x1 DIO there is reserved/colliding, plan the minimal **cut + jumper** on the Control Board (board only; never modify a LaunchPad).
2. Set the CC35x1 pinmux to match via the **SysConfig MCP** (the current `platform/cc35x1/mqtt_io_cc35x1.syscfg` board_pins are provisional placeholders) — direction per the ISO contract. Update `platform/cc35x1/board_pins.h` accordingly. TM4C firmware unchanged.
3. Electrical safety: confirm single 3V3 source (no back-feed), DGND/field-GND stay isolated, RESET_OUT tie-off, and that the 3 MCU-input nets are never configured as outputs on either MCU.

## Verification
- Continuity map peer-reviewed; per-net direction matches the ISO contract on both MCUs.
- With CC35x1 plugged: build/flash, confirm input & relay chains scan and actuate (needs the SN65HVS882/DRV8860 add-on boards); with TM4C plugged: unchanged behavior.

## Do NOT
- Do NOT cut a trace before the continuity map is complete and user-approved.
- Do NOT hand-edit `.syscfg` (SysConfig MCP only). Do NOT modify either LaunchPad.
