//*****************************************************************************
//
// relay_pulse.c - Timed relay pulse implementation.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "relay_chain.h"
#include "mqtt_app.h"
#include "relay_pulse.h"

//
// Per-relay countdown in milliseconds.  Zero means no active pulse.
// Sized for the maximum possible relay count (CFG_RELAY_MAX_DEVICES * 8 = 120).
//
#define PULSE_MAX_RELAYS    (CFG_RELAY_MAX_DEVICES * 8)

static uint32_t g_aui32PulseMs[PULSE_MAX_RELAYS];

//*****************************************************************************
void
RelayPulseStart(int iRelay, uint32_t ui32Ms)
{
    if((iRelay < 0) || (iRelay >= PULSE_MAX_RELAYS))
    {
        return;
    }

    if(ui32Ms == 0)
    {
        g_aui32PulseMs[iRelay] = 0;
        return;
    }

    //
    // Turn relay on, publish retained state, start countdown.
    // MQTTAppSetRelay handles bounds checking, RelayChainSet, and publish.
    //
    MQTTAppSetRelay(iRelay, true);
    g_aui32PulseMs[iRelay] = ui32Ms;
}

//*****************************************************************************
bool
RelayPulseActive(int iRelay)
{
    if((iRelay < 0) || (iRelay >= PULSE_MAX_RELAYS))
    {
        return(false);
    }
    return(g_aui32PulseMs[iRelay] > 0);
}

//*****************************************************************************
void
RelayPulseTick(uint32_t ui32ElapsedMs)
{
    int i;
    uint16_t ui16Count = RelayChainCount();

    for(i = 0; i < (int)ui16Count; i++)
    {
        if(g_aui32PulseMs[i] == 0)
        {
            continue;
        }

        if(ui32ElapsedMs >= g_aui32PulseMs[i])
        {
            g_aui32PulseMs[i] = 0;
            MQTTAppSetRelay(i, false);
        }
        else
        {
            g_aui32PulseMs[i] -= ui32ElapsedMs;
        }
    }
}
