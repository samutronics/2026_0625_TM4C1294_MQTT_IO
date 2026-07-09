//*****************************************************************************
//
// mqtt_app.c - Application glue between configuration, board I/O and the MQTT
// client, including Home Assistant MQTT auto-discovery for the relay outputs.
//
// Topic scheme (base topic is configurable on the web page):
//   <base>/status            -> "online" (retained) / LWT "offline"
//   <base>/relay/<n>/set      <- "ON" / "OFF"   (subscribed, wildcard)
//   <base>/relay/<n>/state    -> "ON" / "OFF"   (retained)
//
// On each connection the device publishes retained Home Assistant discovery
// configs under "homeassistant/switch/..." so every relay appears automatically
// as a switch under one device, then publishes each relay's current state and
// subscribes to the command wildcard.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "utils/ustdlib.h"
#include "utils/uartstdio.h"
#include "config.h"
#include "relay_chain.h"
#include "mqtt_client.h"
#include "mqtt_app.h"

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
static char g_pcDiscPayload[384];

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

    usnprintf(g_pcDiscPayload, sizeof(g_pcDiscPayload),
              "{\"~\":\"%s\",\"name\":\"Relay %d\",\"uniq_id\":\"%s_relay%d\","
              "\"cmd_t\":\"~/relay/%d/set\",\"stat_t\":\"~/relay/%d/state\","
              "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"avty_t\":\"~/status\","
              "\"dev\":{\"ids\":[\"%s\"],\"name\":\"TM4C1294 MQTT IO\","
              "\"mdl\":\"EK-TM4C1294XL\",\"mf\":\"Texas Instruments\"}}",
              g_pcBase, iRelay, g_pcDevId, iRelay, iRelay, iRelay, g_pcDevId);

    MQTTClientPublish(g_pcDiscTopic, (const uint8_t *)g_pcDiscPayload,
                      (uint16_t)strlen(g_pcDiscPayload), 1);
}

//*****************************************************************************
//
// Parse a "<base>/relay/<n>/set" topic and extract the relay index.  Returns
// true and sets *piRelay on a match.
//
//*****************************************************************************
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
// Incoming-message callback: handle relay command topics.
//
//*****************************************************************************
static void
MQTTAppMsgCB(const char *pcTopic, uint16_t ui16TopicLen,
             const uint8_t *pui8Payload, uint16_t ui16PayloadLen)
{
    int iRelay;
    bool bOn;

    if(!MQTTAppParseRelaySet(pcTopic, ui16TopicLen, &iRelay))
    {
        return;
    }
    if((uint16_t)iRelay >= RelayChainCount())
    {
        return;
    }

    bOn = (ui16PayloadLen >= 2) && (pui8Payload[0] == 'O') &&
          (pui8Payload[1] == 'N');

    RelayChainSet((uint16_t)iRelay, bOn);
    MQTTAppPublishRelayState(iRelay);
    UARTprintf("MQTT: relay %d -> %s\n", iRelay, bOn ? "ON" : "OFF");
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

    if(iStep == 1)
    {
        MQTTClientPublish(g_pcTopicStatus, (const uint8_t *)"online", 6, 1);
    }
    else if(iStep <= (1 + iRelays))
    {
        MQTTAppPublishRelayDiscovery(iStep - 2);
    }
    else if(iStep <= (1 + (2 * iRelays)))
    {
        MQTTAppPublishRelayState(iStep - (2 + iRelays));
    }
    else if(iStep == (2 + (2 * iRelays)))
    {
        //
        // One wildcard subscription covers every relay command topic.
        //
        usnprintf(g_pcScratchTopic, sizeof(g_pcScratchTopic),
                  "%s/relay/+/set", g_pcBase);
        MQTTClientSubscribe(g_pcScratchTopic);
        UARTprintf("MQTT: %d relays published (HA discovery).\n", iRelays);
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
        // Freshly connected: kick off the post-connect publish sequence.
        //
        g_iPubStep = 1;
        g_iPubMax = 2 + (2 * (int)RelayChainCount());
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
