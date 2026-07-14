//*****************************************************************************
//
// mqtt_app.c - Application glue between configuration, board I/O and the MQTT
// client, including Home Assistant MQTT auto-discovery for the relay outputs
// and digital inputs (binary_sensor for switches, event for pushbuttons).
//
// Topic scheme (base topic is configurable on the web page):
//   <base>/status              -> "online" (retained) / LWT "offline"
//   <base>/relay/<n>/set        <- "ON" / "OFF"   (subscribed, wildcard)
//   <base>/relay/<n>/state      -> "ON" / "OFF"   (retained)
//   <base>/input/<i>/state      -> "ON" / "OFF"   (retained, switch inputs)
//   <base>/input/<i>/event      -> {"event_type":"single"|"double"} (pushbuttons)
//   <base>/cover/<n>/set        <- OPEN / CLOSE / STOP           (shutters)
//   <base>/cover/<n>/state      -> opening/closing/open/closed/stopped (retained)
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "utils/ustdlib.h"
#include "utils/uartstdio.h"
#include "config.h"
#include "relay_chain.h"
#include "din_chain.h"
#include "mqtt_client.h"
#include "mqtt_app.h"
#include "input_events.h"
#include "relay_pulse.h"
#include "output_ctrl.h"

//
// Home Assistant default discovery prefix.
//
#define HA_PREFIX           "homeassistant"

//
// Stable device id derived from the MAC (e.g. "tm4c1294_a1b2c3").
//
static char g_pcDevId[24];

//
// Base topic and the fully-qualified status topic, rebuilt at each start.
//
static char g_pcBase[CFG_TOPIC_LEN];
static char g_pcTopicStatus[CFG_TOPIC_LEN + 16];

//
// Connection-edge tracking and the post-connect publish sequencer.  Steps:
//   1                    -> status "online"
//   2 .. 1+N             -> relay discovery config (N = relay count)
//   2+N .. 1+2N          -> relay state
//   2+2N                 -> subscribe to the command wildcard
//
static bool g_bWasConnected;
static int  g_iPubStep;
static int  g_iPubMax;

//
// Scratch buffers for building topics / discovery payloads.
//
static char g_pcScratchTopic[80];
static char g_pcDiscTopic[96];
static char g_pcDiscPayload[512];

//
// Snapshot of the input chain taken at connect, used to publish the initial
// retained state of every input during the post-connect sequence.
//
static uint8_t g_pui8InSnap[DIN_MAX_BYTES];

//*****************************************************************************
//
// Publish one relay's current state (retained).
//
//*****************************************************************************
static void
MQTTAppPublishRelayState(int iRelay)
{
    const char *pcMsg = RelayChainGet((uint16_t)iRelay) ? "ON" : "OFF";

    usnprintf(g_pcScratchTopic, sizeof(g_pcScratchTopic), "%s/relay/%d/state",
              g_pcBase, iRelay);
    MQTTClientPublish(g_pcScratchTopic, (const uint8_t *)pcMsg,
                      (uint16_t)strlen(pcMsg), 1);
}

//*****************************************************************************
//
// Publish one relay's Home Assistant discovery config (retained switch).
//
//*****************************************************************************
static void
MQTTAppPublishRelayDiscovery(int iRelay)
{
    usnprintf(g_pcDiscTopic, sizeof(g_pcDiscTopic),
              HA_PREFIX "/switch/%s/relay%d/config", g_pcDevId, iRelay);

    //
    // Relays that belong to a shutter are exposed as a cover, not a switch.
    // Clear any stale retained switch config so HA drops the switch entity.
    //
    if(OutputCtrlIsShutterMember(iRelay))
    {
        MQTTClientPublish(g_pcDiscTopic, (const uint8_t *)"", 0, 1);
        return;
    }

    usnprintf(g_pcDiscPayload, sizeof(g_pcDiscPayload),
              "{\"~\":\"%s\",\"name\":\"Out%02d\",\"uniq_id\":\"%s_relay%d\","
              "\"cmd_t\":\"~/relay/%d/set\",\"stat_t\":\"~/relay/%d/state\","
              "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"avty_t\":\"~/status\","
              "\"dev\":{\"ids\":[\"%s\"],\"name\":\"SaKaHub\","
              "\"mdl\":\"%s\",\"mf\":\"TomArts\"}}",
              g_pcBase, iRelay + 1, g_pcDevId, iRelay, iRelay, iRelay,
              g_pcDevId, ConfigGet()->pcClientID);

    MQTTClientPublish(g_pcDiscTopic, (const uint8_t *)g_pcDiscPayload,
                      (uint16_t)strlen(g_pcDiscPayload), 1);
}

//*****************************************************************************
//
// Publish one shutter's current cover state (retained).  Called from the
// shutter FSM in output_ctrl.c and from the post-connect sequence.
//
//*****************************************************************************
void
MQTTAppPublishCoverState(int iShutter, const char *pcState)
{
    if(!MQTTClientIsReady())
    {
        return;
    }
    usnprintf(g_pcScratchTopic, sizeof(g_pcScratchTopic), "%s/cover/%d/state",
              g_pcBase, iShutter);
    MQTTClientPublish(g_pcScratchTopic, (const uint8_t *)pcState,
                      (uint16_t)strlen(pcState), 1);
}

//*****************************************************************************
//
// Publish one shutter's Home Assistant cover discovery config (retained).
// Empty (unconfigured) slots clear any stale retained config.
//
//*****************************************************************************
static void
MQTTAppPublishCoverDiscovery(int iShutter)
{
    usnprintf(g_pcDiscTopic, sizeof(g_pcDiscTopic),
              HA_PREFIX "/cover/%s/cover%d/config", g_pcDevId, iShutter);

    if(!OutputCtrlShutterValid(iShutter))
    {
        MQTTClientPublish(g_pcDiscTopic, (const uint8_t *)"", 0, 1);
        return;
    }

    usnprintf(g_pcDiscPayload, sizeof(g_pcDiscPayload),
              "{\"~\":\"%s\",\"name\":\"Shutter%02d\",\"uniq_id\":\"%s_cover%d\","
              "\"cmd_t\":\"~/cover/%d/set\",\"stat_t\":\"~/cover/%d/state\","
              "\"pl_open\":\"OPEN\",\"pl_cls\":\"CLOSE\",\"pl_stop\":\"STOP\","
              "\"stat_open\":\"open\",\"stat_clsd\":\"closed\","
              "\"stat_opening\":\"opening\",\"stat_closing\":\"closing\","
              "\"dev_cla\":\"shutter\",\"avty_t\":\"~/status\","
              "\"dev\":{\"ids\":[\"%s\"],\"name\":\"SaKaHub\","
              "\"mdl\":\"%s\",\"mf\":\"TomArts\"}}",
              g_pcBase, iShutter + 1, g_pcDevId, iShutter, iShutter, iShutter,
              g_pcDevId, ConfigGet()->pcClientID);

    MQTTClientPublish(g_pcDiscTopic, (const uint8_t *)g_pcDiscPayload,
                      (uint16_t)strlen(g_pcDiscPayload), 1);
}

//*****************************************************************************
//
// Publish one input channel's Home Assistant discovery config.  The entity
// type depends on whether the input is configured as a switch (binary_sensor)
// or a pushbutton (event).  The stale config for the other type is cleared by
// publishing an empty retained payload to its topic.
//
//*****************************************************************************
static void
MQTTAppPublishInputDiscovery(int iInput)
{
    bool bPB = ConfigInputIsPushbutton(iInput);
    const char *pcActiveComp  = bPB ? "event"         : "binary_sensor";
    const char *pcStaleComp   = bPB ? "binary_sensor" : "event";

    //
    // Clear the stale component's retained config first.
    //
    usnprintf(g_pcDiscTopic, sizeof(g_pcDiscTopic),
              HA_PREFIX "/%s/%s/input%d/config", pcStaleComp, g_pcDevId, iInput);
    MQTTClientPublish(g_pcDiscTopic, (const uint8_t *)"", 0, 1);

    //
    // Publish the active component's config.
    //
    usnprintf(g_pcDiscTopic, sizeof(g_pcDiscTopic),
              HA_PREFIX "/%s/%s/input%d/config", pcActiveComp, g_pcDevId, iInput);

    if(bPB)
    {
        usnprintf(g_pcDiscPayload, sizeof(g_pcDiscPayload),
                  "{\"~\":\"%s\",\"name\":\"In%02d\",\"uniq_id\":\"%s_input%d\","
                  "\"stat_t\":\"~/input/%d/event\","
                  "\"event_types\":[\"single\",\"double\"],"
                  "\"avty_t\":\"~/status\",\"dev\":{\"ids\":[\"%s\"],"
                  "\"name\":\"SaKaHub\",\"mdl\":\"%s\","
                  "\"mf\":\"TomArts\"}}",
                  g_pcBase, iInput + 1, g_pcDevId, iInput, iInput,
                  g_pcDevId, ConfigGet()->pcClientID);
    }
    else
    {
        usnprintf(g_pcDiscPayload, sizeof(g_pcDiscPayload),
                  "{\"~\":\"%s\",\"name\":\"In%02d\",\"uniq_id\":\"%s_input%d\","
                  "\"stat_t\":\"~/input/%d/state\",\"pl_on\":\"ON\",\"pl_off\":"
                  "\"OFF\",\"avty_t\":\"~/status\",\"dev\":{\"ids\":[\"%s\"],"
                  "\"name\":\"SaKaHub\",\"mdl\":\"%s\","
                  "\"mf\":\"TomArts\"}}",
                  g_pcBase, iInput + 1, g_pcDevId, iInput, iInput,
                  g_pcDevId, ConfigGet()->pcClientID);
    }

    MQTTClientPublish(g_pcDiscTopic, (const uint8_t *)g_pcDiscPayload,
                      (uint16_t)strlen(g_pcDiscPayload), 1);
}

//*****************************************************************************
//
// Publish one input channel's state (retained).
//
//*****************************************************************************
void
MQTTAppPublishInput(int iInput, bool bOn)
{
    if(!MQTTClientIsReady())
    {
        return;
    }
    usnprintf(g_pcScratchTopic, sizeof(g_pcScratchTopic), "%s/input/%d/state",
              g_pcBase, iInput);
    MQTTClientPublish(g_pcScratchTopic, (const uint8_t *)(bOn ? "ON" : "OFF"),
                      (uint16_t)(bOn ? 2 : 3), 1);
}

//*****************************************************************************
//
// Publish a pushbutton click event (NOT retained) to the HA event topic.
//
//*****************************************************************************
void
MQTTAppPublishInputEvent(int iInput, const char *pcEvt)
{
    char pcPayload[48];

    if(!MQTTClientIsReady())
    {
        return;
    }
    usnprintf(g_pcScratchTopic, sizeof(g_pcScratchTopic), "%s/input/%d/event",
              g_pcBase, iInput);
    usnprintf(pcPayload, sizeof(pcPayload), "{\"event_type\":\"%s\"}", pcEvt);
    MQTTClientPublish(g_pcScratchTopic, (const uint8_t *)pcPayload,
                      (uint16_t)strlen(pcPayload), 0);
}

//*****************************************************************************
//
// Parse a "<base>/relay/<n>/set" topic and extract the relay index.  Returns
// true and sets *piRelay on a match.
//
//*****************************************************************************
//*****************************************************************************
//
// Parse a "<base>/relay/<n>/pulse" topic.  Same logic as MQTTAppParseRelaySet
// but matches the "/pulse" suffix.
//
//*****************************************************************************
static bool
MQTTAppParseRelayPulse(const char *pcTopic, uint16_t ui16Len, int *piRelay)
{
    static const char pcMid[] = "/relay/";
    static const char pcSuf[] = "/pulse";
    int iBaseLen = (int)strlen(g_pcBase);
    int iMidLen  = (int)(sizeof(pcMid) - 1);
    int iSufLen  = (int)(sizeof(pcSuf) - 1);
    int iPos = 0, iNum = 0, iDigits = 0;

    if((int)ui16Len < iBaseLen + iMidLen + 1 + iSufLen) { return(false); }
    if(memcmp(pcTopic, g_pcBase, iBaseLen) != 0)         { return(false); }
    iPos = iBaseLen;
    if(memcmp(pcTopic + iPos, pcMid, iMidLen) != 0)      { return(false); }
    iPos += iMidLen;

    while((iPos < (int)ui16Len) && (pcTopic[iPos] >= '0') &&
          (pcTopic[iPos] <= '9'))
    {
        iNum = (iNum * 10) + (pcTopic[iPos] - '0');
        iPos++;
        iDigits++;
    }
    if(iDigits == 0)                               { return(false); }
    if(((int)ui16Len - iPos) != iSufLen)           { return(false); }
    if(memcmp(pcTopic + iPos, pcSuf, iSufLen) != 0) { return(false); }

    *piRelay = iNum;
    return(true);
}

static bool
MQTTAppParseRelaySet(const char *pcTopic, uint16_t ui16Len, int *piRelay)
{
    static const char pcMid[] = "/relay/";
    static const char pcSuf[] = "/set";
    int iBaseLen = (int)strlen(g_pcBase);
    int iMidLen = (int)(sizeof(pcMid) - 1);
    int iSufLen = (int)(sizeof(pcSuf) - 1);
    int iPos = 0;
    int iNum = 0;
    int iDigits = 0;

    //
    // Base prefix + "/relay/".
    //
    if((int)ui16Len < iBaseLen + iMidLen + 1 + iSufLen)
    {
        return(false);
    }
    if(memcmp(pcTopic, g_pcBase, iBaseLen) != 0)
    {
        return(false);
    }
    iPos = iBaseLen;
    if(memcmp(pcTopic + iPos, pcMid, iMidLen) != 0)
    {
        return(false);
    }
    iPos += iMidLen;

    //
    // Decimal relay index.
    //
    while((iPos < (int)ui16Len) && (pcTopic[iPos] >= '0') &&
          (pcTopic[iPos] <= '9'))
    {
        iNum = (iNum * 10) + (pcTopic[iPos] - '0');
        iPos++;
        iDigits++;
    }
    if(iDigits == 0)
    {
        return(false);
    }

    //
    // Trailing "/set".
    //
    if(((int)ui16Len - iPos) != iSufLen)
    {
        return(false);
    }
    if(memcmp(pcTopic + iPos, pcSuf, iSufLen) != 0)
    {
        return(false);
    }

    *piRelay = iNum;
    return(true);
}

//*****************************************************************************
//
// Parse a "<base>/cover/<n>/set" topic and extract the shutter index.
//
//*****************************************************************************
static bool
MQTTAppParseCoverSet(const char *pcTopic, uint16_t ui16Len, int *piShutter)
{
    static const char pcMid[] = "/cover/";
    static const char pcSuf[] = "/set";
    int iBaseLen = (int)strlen(g_pcBase);
    int iMidLen  = (int)(sizeof(pcMid) - 1);
    int iSufLen  = (int)(sizeof(pcSuf) - 1);
    int iPos = 0, iNum = 0, iDigits = 0;

    if((int)ui16Len < iBaseLen + iMidLen + 1 + iSufLen) { return(false); }
    if(memcmp(pcTopic, g_pcBase, iBaseLen) != 0)         { return(false); }
    iPos = iBaseLen;
    if(memcmp(pcTopic + iPos, pcMid, iMidLen) != 0)      { return(false); }
    iPos += iMidLen;

    while((iPos < (int)ui16Len) && (pcTopic[iPos] >= '0') &&
          (pcTopic[iPos] <= '9'))
    {
        iNum = (iNum * 10) + (pcTopic[iPos] - '0');
        iPos++;
        iDigits++;
    }
    if(iDigits == 0)                                { return(false); }
    if(((int)ui16Len - iPos) != iSufLen)            { return(false); }
    if(memcmp(pcTopic + iPos, pcSuf, iSufLen) != 0) { return(false); }

    *piShutter = iNum;
    return(true);
}

//*****************************************************************************
//
// Incoming-message callback: handle relay and cover command topics.
//
//*****************************************************************************
static void
MQTTAppMsgCB(const char *pcTopic, uint16_t ui16TopicLen,
             const uint8_t *pui8Payload, uint16_t ui16PayloadLen)
{
    int iRelay;
    int iShutter;
    bool bOn;

    //
    // Relay ON/OFF command — routed through the output controller so the
    // output's mode (Standard / Timed / shutter member) is honored.
    //
    if(MQTTAppParseRelaySet(pcTopic, ui16TopicLen, &iRelay))
    {
        if((uint16_t)iRelay >= RelayChainCount())
        {
            return;
        }
        bOn = (ui16PayloadLen >= 2) && (pui8Payload[0] == 'O') &&
              (pui8Payload[1] == 'N');
        OutputCtrlCommand(iRelay, bOn ? OUT_CMD_ON : OUT_CMD_OFF);
        UARTprintf("MQTT: relay %d -> %s\n", iRelay, bOn ? "ON" : "OFF");
        return;
    }

    //
    // Cover (shutter) command — payload OPEN / CLOSE / STOP.
    //
    if(MQTTAppParseCoverSet(pcTopic, ui16TopicLen, &iShutter))
    {
        tShCmd eCmd;
        if(ui16PayloadLen == 0) { return; }
        switch(pui8Payload[0])
        {
            case 'O': eCmd = SH_CMD_OPEN;  break;   // OPEN  (direct)
            case 'C': eCmd = SH_CMD_CLOSE; break;   // CLOSE (direct)
            case 'S': eCmd = SH_CMD_STOP;  break;   // STOP
            default:  return;
        }
        OutputCtrlShutter(iShutter, eCmd);
        UARTprintf("MQTT: cover %d cmd %d\n", iShutter, (int)eCmd);
        return;
    }

    //
    // Relay pulse command — payload is duration in milliseconds.
    //
    if(MQTTAppParseRelayPulse(pcTopic, ui16TopicLen, &iRelay))
    {
        uint32_t ui32Ms;
        char acNum[12];
        if((uint16_t)iRelay >= RelayChainCount())
        {
            return;
        }
        if(OutputCtrlIsShutterMember(iRelay))
        {
            return;   // shutter relays are driven via the cover interface
        }
        //
        // Copy payload to null-terminated buffer for ustrtoul.
        //
        if(ui16PayloadLen >= sizeof(acNum))
        {
            ui16PayloadLen = (uint16_t)(sizeof(acNum) - 1);
        }
        memcpy(acNum, pui8Payload, ui16PayloadLen);
        acNum[ui16PayloadLen] = '\0';
        ui32Ms = ustrtoul(acNum, NULL, 10);
        if(ui32Ms == 0)
        {
            ui32Ms = 1000;   // implicit default: empty/0 payload pulses for 1 s
        }
        if(ui32Ms > 3600000u)
        {
            return;   // reject durations > 1 hour
        }
        UARTprintf("MQTT: relay %d pulse %u ms\n", iRelay, ui32Ms);
        RelayPulseStart(iRelay, ui32Ms);
        return;
    }
}

//*****************************************************************************
//
// Build the topic strings from the configured base topic.
//
//*****************************************************************************
static void
MQTTAppBuildTopics(const char *pcBase)
{
    strncpy(g_pcBase, pcBase, sizeof(g_pcBase) - 1);
    g_pcBase[sizeof(g_pcBase) - 1] = '\0';

    usnprintf(g_pcTopicStatus, sizeof(g_pcTopicStatus), "%s/status", pcBase);
}

//*****************************************************************************
//
// Initialise the MQTT subsystem and derive the HA device id from the MAC.
//
//*****************************************************************************
void
MQTTAppInit(const uint8_t *pui8MAC)
{
    g_bWasConnected = false;
    g_iPubStep = 0;

    usnprintf(g_pcDevId, sizeof(g_pcDevId), "tm4c1294_%02x%02x%02x",
              pui8MAC[3], pui8MAC[4], pui8MAC[5]);

    MQTTClientInit(MQTTAppMsgCB);
}

//*****************************************************************************
//
// (Re)start the connection from the current configuration.
//
//*****************************************************************************
void
MQTTAppStart(void)
{
    tMQTTConfig *psCfg = ConfigGet();

    if(!ConfigHasBroker())
    {
        UARTprintf("MQTT: no broker configured; idle.\n");
        return;
    }

    MQTTAppBuildTopics(psCfg->pcTopicBase);

    //
    // Last will: broker publishes "offline" (retained) if we drop.
    //
    MQTTClientSetWill(g_pcTopicStatus, "offline", 1);

    g_bWasConnected = false;
    g_iPubStep = 0;

    UARTprintf("MQTT: connecting to %s:%d as '%s'...\n", psCfg->pcHost,
               psCfg->ui16Port, psCfg->pcClientID);

    MQTTClientStart(psCfg->pcHost, psCfg->ui16Port, psCfg->pcClientID,
                    psCfg->ui8UseAuth ? psCfg->pcUser : "",
                    psCfg->ui8UseAuth ? psCfg->pcPass : "");
}

//*****************************************************************************
//
// Stop the connection.
//
//*****************************************************************************
void
MQTTAppStop(void)
{
    MQTTClientStop();
    g_bWasConnected = false;
    g_iPubStep = 0;
}

//*****************************************************************************
//
// Run one step of the post-connect publish sequence.
//
//*****************************************************************************
static void
MQTTAppPostConnect(int iStep)
{
    int iRelays = (int)RelayChainCount();
    int iInputs = (int)DINChainInputCount();
    int iShut   = CFG_MAX_SHUTTERS;
    int iSub    = 2 + (2 * iRelays);        // relay command-topic subscribe step
    int iInBase = iSub;                     // input block starts after iSub
    int iCvBase = iSub + (2 * iInputs);     // cover block starts after inputs
    int iCvSub  = iCvBase + (2 * iShut) + 1;// cover command-topic subscribe step

    if(iStep == 1)
    {
        MQTTClientPublish(g_pcTopicStatus, (const uint8_t *)"online", 6, 1);
    }
    else if(iStep <= (1 + iRelays))
    {
        // Relay switch discovery (shutter-member relays are cleared instead).
        MQTTAppPublishRelayDiscovery(iStep - 2);
    }
    else if(iStep <= (1 + (2 * iRelays)))
    {
        int iRelay = iStep - (2 + iRelays);
        if(!OutputCtrlIsShutterMember(iRelay))
        {
            MQTTAppPublishRelayState(iRelay);
        }
    }
    else if(iStep == iSub)
    {
        if(iRelays > 0)
        {
            usnprintf(g_pcScratchTopic, sizeof(g_pcScratchTopic),
                      "%s/relay/+/set", g_pcBase);
            MQTTClientSubscribe(g_pcScratchTopic);
            usnprintf(g_pcScratchTopic, sizeof(g_pcScratchTopic),
                      "%s/relay/+/pulse", g_pcBase);
            MQTTClientSubscribe(g_pcScratchTopic);
        }
        UARTprintf("MQTT: %d relays published (HA discovery).\n", iRelays);
    }
    else if(iStep <= (iInBase + iInputs))
    {
        MQTTAppPublishInputDiscovery(iStep - iInBase - 1);
    }
    else if(iStep <= (iInBase + (2 * iInputs)))
    {
        //
        // Initial retained state for switch-type inputs only.  Pushbuttons have
        // no retained level state — they publish events on click.
        //
        int iInput = iStep - iInBase - iInputs - 1;
        if(!ConfigInputIsPushbutton(iInput))
        {
            int iMask = 1 << (iInput & 7);   // LSB-first: input d*8+b -> bit b
            MQTTAppPublishInput(iInput,
                                (g_pui8InSnap[iInput / 8] & iMask) != 0);
        }
        if(iInput == (iInputs - 1))
        {
            UARTprintf("MQTT: %d inputs published (HA discovery).\n", iInputs);
        }
    }
    else if(iStep <= (iCvBase + iShut))
    {
        // Cover discovery (unconfigured slots clear stale retained config).
        MQTTAppPublishCoverDiscovery(iStep - iCvBase - 1);
    }
    else if(iStep <= (iCvBase + (2 * iShut)))
    {
        int iSh = iStep - iCvBase - iShut - 1;
        if(OutputCtrlShutterValid(iSh))
        {
            MQTTAppPublishCoverState(iSh, OutputCtrlCoverState(iSh));
        }
    }
    else if(iStep == iCvSub)
    {
        usnprintf(g_pcScratchTopic, sizeof(g_pcScratchTopic),
                  "%s/cover/+/set", g_pcBase);
        MQTTClientSubscribe(g_pcScratchTopic);
    }
}

//*****************************************************************************
//
// Periodic service.  Detects the connect edge and drives the staggered
// post-connect publish sequence (status, discovery, state, subscribe).
//
//*****************************************************************************
void
MQTTAppTick(uint32_t ui32ElapsedMs)
{
    bool bConnected;

    MQTTClientTick(ui32ElapsedMs);

    bConnected = MQTTClientIsReady();
    if(bConnected && !g_bWasConnected)
    {
        //
        // Freshly connected: kick off the post-connect publish sequence and
        // snapshot the inputs so their initial retained state is published.
        //
        g_iPubStep = 1;
        g_iPubMax = 2 + (2 * (int)RelayChainCount()) +
                    (2 * (int)DINChainInputCount()) +
                    (2 * CFG_MAX_SHUTTERS) + 1;   // + cover disc/state + cover sub
        DINChainRead(g_pui8InSnap, sizeof(g_pui8InSnap));
    }
    if(!bConnected)
    {
        g_iPubStep = 0;
    }
    g_bWasConnected = bConnected;

    //
    // Advance the publish sequence, one item per tick.
    //
    if(bConnected && (g_iPubStep > 0) && (g_iPubStep <= g_iPubMax))
    {
        MQTTAppPostConnect(g_iPubStep);
        g_iPubStep++;
    }
}

//*****************************************************************************
//
// Set one relay and publish its new state (retained).  Called from the local
// input→output binding logic; mirrors what the relay MQTT command handler does.
//
//*****************************************************************************
void
MQTTAppSetRelay(int iRelay, bool bOn)
{
    if((iRelay < 0) || ((uint16_t)iRelay >= RelayChainCount()))
    {
        return;
    }
    RelayChainSet((uint16_t)iRelay, bOn);
    MQTTAppPublishRelayState(iRelay);
}

//*****************************************************************************
//
// Re-run the full post-connect publish sequence (discovery + state) without
// reconnecting.  Call this after the I/O configuration changes so Home
// Assistant sees updated entity types immediately.
//
//*****************************************************************************
void
MQTTAppRepublish(void)
{
    if(!MQTTClientIsReady())
    {
        return;
    }
    DINChainRead(g_pui8InSnap, sizeof(g_pui8InSnap));
    g_iPubStep = 1;
}

//*****************************************************************************
//
// True if connected.
//
//*****************************************************************************
bool
MQTTAppIsConnected(void)
{
    return(MQTTClientIsReady());
}

//*****************************************************************************
//
// Human-readable status for the web UI.
//
//*****************************************************************************
const char *
MQTTAppStatusStr(void)
{
    switch(MQTTClientState())
    {
        case MQTT_CLI_READY:    return("Connected");
        case MQTT_CLI_CONNECT:  return("Authenticating");
        case MQTT_CLI_TCP:      return("Connecting (TCP)");
        case MQTT_CLI_DNS:      return("Resolving host");
        case MQTT_CLI_IDLE:
        default:
            return(ConfigHasBroker() ? "Disconnected" : "Not configured");
    }
}
