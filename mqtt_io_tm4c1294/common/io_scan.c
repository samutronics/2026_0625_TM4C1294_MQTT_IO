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
// Total logical input count and "read all inputs" - see io_scan.h.  These
// aggregate the SN65HVS882 SPI chain with the platform-local inputs (the CC35x1
// on-board buttons, via the WebPlatformLocalInput* seam) so both count and sample
// present one contiguous input space: SPI inputs 0..D*8-1 then the local inputs at
// D*8.. .  The SPI device count comes from ConfigGetDinDevices() (not the chain
// driver's live count) so this matches the web UI's "mqdin" tag exactly - the two
// diverge for at most one tick right after a device-count change, and keeping both
// on the config value avoids a transient off-by-a-byte render on the page reload
// that follows a Save.  The local byte is only appended when there is room for it
// in the fixed buffers and the input map (so a maxed-out SPI chain is never
// overrun).
//
//*****************************************************************************
uint16_t
IOInputCount(void)
{
    int iDin   = (int)ConfigGetDinDevices();
    int iSpi   = iDin * 8;
    int iLocal = WebPlatformLocalInputCount();

    if((iLocal > 0) && (iDin < DIN_MAX_BYTES) &&
       ((iSpi + iLocal) <= CFG_MAX_INPUTS))
    {
        return((uint16_t)(iSpi + iLocal));
    }
    return((uint16_t)iSpi);
}

uint8_t
IOInputReadAll(uint8_t *pui8Buf, uint8_t ui8BufLen)
{
    int iDin   = (int)ConfigGetDinDevices();
    int iLocal = WebPlatformLocalInputCount();
    int iTotal = (int)IOInputCount();

    if(ui8BufLen == 0)
    {
        return(0);
    }

    //
    // Zero first so any byte the SPI read does not cover (and the unused high bits
    // of the local byte) reads as 0 rather than stale stack.
    //
    memset(pui8Buf, 0, ui8BufLen);

    //
    // Sample the SPI chain hardware into [0..chain devices-1].
    //
    (void)DINChainRead(pui8Buf, ui8BufLen);

    //
    // Append the platform-local inputs as one byte at the config SPI device index
    // (LSB-first, masked to the low iLocal bits), when they fit the buffer + map.
    // With D == 0 this becomes byte 0; otherwise it follows the SPI bytes.
    //
    if((iLocal > 0) && (iDin < (int)ui8BufLen) && (iDin < DIN_MAX_BYTES) &&
       ((iDin * 8 + iLocal) <= CFG_MAX_INPUTS))
    {
        uint8_t ui8Mask = (uint8_t)((1u << iLocal) - 1u);
        pui8Buf[iDin] = (uint8_t)(WebPlatformLocalInputRead() & ui8Mask);
    }

    //
    // Bytes needed to cover the whole logical input space.
    //
    return((uint8_t)((iTotal + 7) / 8));
}

//*****************************************************************************
//
// Poll all digital inputs (the SN65HVS882 SPI chain plus the appended
// platform-local inputs): publish switch-input changes over MQTT, feed pushbutton
// levels to the click-detector, and apply bindings.  Called from the periodic
// application tick.
//
//*****************************************************************************
static void
DINChainScan(void)
{
    static uint8_t pui8Prev[IO_MAX_BYTES];
    static bool bPrimed;
    uint8_t pui8Cur[IO_MAX_BYTES];
    uint8_t ui8Bytes;
    int iTotal, iInput;

    ui8Bytes = IOInputReadAll(pui8Cur, sizeof(pui8Cur));
    if(ui8Bytes == 0)
    {
        return;
    }
    memcpy(g_pui8LiveInState, pui8Cur, ui8Bytes);

    //
    // Process each input by contiguous index.  Input i maps to bit (i%8) of byte
    // (i/8), LSB-first - matching the live input matrix (SSI_INDEX_INSTATES), the
    // relay side, and the physical channel labels.  Iterating by input index (not
    // by whole bytes) means the unused high bits of the partial local-input byte
    // are never mistaken for inputs.
    //
    // Pushbutton inputs: feed the current level to the click-detector every scan
    // (the FSM needs a continuous level signal for debounce).
    //
    // Switch inputs: detect edges vs the previous scan and publish ON/OFF.  The
    // first scan only primes the baseline; initial retained states are published
    // by the MQTT post-connect sequence.
    //
    iTotal = (int)IOInputCount();
    for(iInput = 0; iInput < iTotal; iInput++)
    {
        int     iByte   = iInput >> 3;
        uint8_t ui8Mask = (uint8_t)(1u << (iInput & 7));
        bool    bActive = (pui8Cur[iByte] & ui8Mask) != 0;
        bool    bChange = ((pui8Cur[iByte] ^ pui8Prev[iByte]) & ui8Mask) != 0;

        if(ConfigInputIsPushbutton(iInput))
        {
            InputEventsUpdate(iInput, bActive);
        }
        else if(bPrimed && bChange)
        {
            PalLog("DIN in%d: %s\n", iInput, bActive ? "ON" : "OFF");
            MQTTAppPublishInput(iInput, bActive);
            ApplyBindings(iInput,
                          bActive ? BIND_TRIG_LEVEL_ON : BIND_TRIG_LEVEL_OFF);
        }
    }
    memcpy(pui8Prev, pui8Cur, ui8Bytes);
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
