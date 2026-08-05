//*****************************************************************************
//
// io_scan.c - Periodic field-I/O scan + input->output binding engine.
//
// Portable single-source: moved out of the TM4C enet_io.c so both the TM4C
// (Ethernet) and CC35x1 (Wi-Fi) platforms run identical input-scan, binding and
// fault-monitor logic.  TivaWare UARTprintf() is replaced by PalLog(); every
// other collaborator was already portable.  See io_scan.h for the two-call API.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "din_chain.h"
#include "relay_chain.h"
#include "input_events.h"
#include "output_ctrl.h"
#include "mqtt_app.h"
#include "webui.h"          // g_pui8LiveInState[] (the live input matrix)
#include "pal_log.h"
#include "io_scan.h"

//*****************************************************************************
//
// Apply all binding slots for one input that match the given trigger event.
// Each matching slot sets a relay and publishes its new retained state.
//
//*****************************************************************************
static void
ApplyBindings(int iInput, uint8_t ui8Trig)
{
    int iSlot;

    for(iSlot = 0; iSlot < CFG_BIND_SLOTS; iSlot++)
    {
        uint8_t ta  = ConfigBindingGetTrigAct(iInput, iSlot);
        uint8_t out = ConfigBindingGetOutput(iInput, iSlot);
        uint8_t trig = ta & 0x07u;
        uint8_t act  = (ta >> 3) & 0x03u;

        //
        // Skip slot if: trigger doesn't match, output is not configured, or
        // output index is out of range.  BIND_TRIG_CHANGE matches both
        // BIND_TRIG_LEVEL_ON and BIND_TRIG_LEVEL_OFF so it fires on any
        // state transition of a switch-type input.
        //
        if(out == BIND_OUTPUT_NONE || out >= (uint8_t)RelayChainCount())
        {
            continue;
        }
        if(trig != ui8Trig &&
           !(trig == BIND_TRIG_CHANGE &&
             (ui8Trig == BIND_TRIG_LEVEL_ON || ui8Trig == BIND_TRIG_LEVEL_OFF)))
        {
            continue;
        }

        //
        // Route through the output controller so the bound output's mode is
        // honored.  Standard sets the relay; Timed starts its auto-OFF; a
        // shutter member moves that member's direction on ON/Toggle (press again
        // while moving = stop), stops on OFF, and runs the single-button
        // up/stop/down cycle on Cycle.
        //
        {
            tOutCmd eCmd = (act == BIND_ACT_ON)     ? OUT_CMD_ON     :
                           (act == BIND_ACT_OFF)    ? OUT_CMD_OFF    :
                           (act == BIND_ACT_CYCLE)  ? OUT_CMD_CYCLE  : OUT_CMD_TOGGLE;
            PalLog("Bind: in%d trig%d -> out%d cmd%d\n",
                   iInput, ui8Trig, out, (int)eCmd);
            OutputCtrlCommand(out, eCmd);
        }
    }
}

//*****************************************************************************
//
// Predicate for the click detector: does this input have a double-click action?
// Only when it does must the detector wait out the double-click window before
// committing to "single".  Inputs with a single action but no double action
// fire immediately on release (see InputEventsSetDoubleQuery).
//
//*****************************************************************************
static bool
InputNeedsDouble(int iInput)
{
    int iSlot;

    for(iSlot = 0; iSlot < CFG_BIND_SLOTS; iSlot++)
    {
        uint8_t ta  = ConfigBindingGetTrigAct(iInput, iSlot);
        uint8_t out = ConfigBindingGetOutput(iInput, iSlot);

        if(((ta & 0x07u) == BIND_TRIG_DOUBLE) && (out != BIND_OUTPUT_NONE))
        {
            return true;
        }
    }
    return false;
}

//*****************************************************************************
//
// Click-event callback: publish the HA event and apply any binding.
//
//*****************************************************************************
static void
IEClickCallback(int iInput, const char *pcEvt)
{
    uint8_t ui8Trig;

    MQTTAppPublishInputEvent(iInput, pcEvt);

    ui8Trig = (pcEvt[0] == 's') ? BIND_TRIG_SINGLE : BIND_TRIG_DOUBLE;
    ApplyBindings(iInput, ui8Trig);
}

//*****************************************************************************
//
// Poll the SN65HVS882 input chain: publish switch-input changes over MQTT,
// feed pushbutton levels to the click-detector, and apply bindings.  Called
// from the periodic application tick.
//
//*****************************************************************************
static void
DINChainScan(void)
{
    static uint8_t pui8Prev[DIN_MAX_BYTES];
    static bool bPrimed;
    uint8_t pui8Cur[DIN_MAX_BYTES];
    uint8_t ui8Bytes, i;

    ui8Bytes = DINChainRead(pui8Cur, sizeof(pui8Cur));
    if(ui8Bytes == 0)
    {
        return;
    }
    memcpy(g_pui8LiveInState, pui8Cur, ui8Bytes);

    //
    // Process each input.  Channel c of device d maps to bit (7-c) of byte d
    // and to input index d*8+c.
    //
    // Pushbutton inputs: feed the current level to the click-detector every
    // scan (the FSM needs a continuous level signal for debounce).
    //
    // Switch inputs: detect edges vs the previous scan and publish ON/OFF.
    // The first scan only primes the baseline; initial retained states are
    // published by the MQTT post-connect sequence.
    //
    for(i = 0; i < ui8Bytes; i++)
    {
        uint8_t ui8Diff = (uint8_t)(pui8Cur[i] ^ pui8Prev[i]);
        int iBit;

        for(iBit = 0; iBit < 8; iBit++)
        {
            //
            // LSB-first within each byte: input index d*8+b maps to bit b, to
            // match the live input matrix (SSI_INDEX_INSTATES) and the relay
            // side, and thus the physical channel labels.  (Previously this
            // walked bits MSB-first (0x80>>iBit), so a press on channel N was
            // scanned/bound as channel 7-N within its byte - inputs fired the
            // wrong bindings even though the matrix showed the right channel.)
            //
            uint8_t ui8Mask = (uint8_t)(1u << iBit);
            int iInput = (i * 8) + iBit;
            bool bActive = (pui8Cur[i] & ui8Mask) != 0;

            if(ConfigInputIsPushbutton(iInput))
            {
                InputEventsUpdate(iInput, bActive);
            }
            else if(bPrimed && (ui8Diff & ui8Mask))
            {
                PalLog("DIN in%d: %s\n", iInput, bActive ? "ON" : "OFF");
                MQTTAppPublishInput(iInput, bActive);
                ApplyBindings(iInput,
                              bActive ? BIND_TRIG_LEVEL_ON : BIND_TRIG_LEVEL_OFF);
            }
        }
        pui8Prev[i] = pui8Cur[i];
    }
    bPrimed = true;
}

//*****************************************************************************
//
// Log DRV8860 nFAULT transitions (overcurrent / over-temperature / open-load).
//
//*****************************************************************************
static void
RelayFaultScan(void)
{
    static bool bPrevFault;
    bool bFault = RelayChainFault();

    if(bFault != bPrevFault)
    {
        PalLog("Relay nFAULT %s\n", bFault ? "ASSERTED" : "cleared");
        bPrevFault = bFault;
    }
}

//*****************************************************************************
//
// Public API - see io_scan.h.
//
//*****************************************************************************
void
IOScanInit(void)
{
    //
    // Wire the pushbutton click callback to the MQTT event publisher and the
    // binding engine, and tell the detector which inputs need the double-click
    // window.
    //
    InputEventsSetCallback(IEClickCallback);
    InputEventsSetDoubleQuery(InputNeedsDouble);
}

void
IOScanTick(void)
{
    DINChainScan();
    RelayFaultScan();
}
