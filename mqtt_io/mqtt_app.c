//*****************************************************************************
//
// mqtt_app.c - Application glue between configuration, board I/O and the MQTT
// client, including Home Assistant MQTT auto-discovery.
//
// Topic scheme (base topic is configurable on the web page):
//   <base>/status         -> "online" (retained) / LWT "offline"
//   <base>/button/sw1      -> "PRESSED" / "RELEASED"
//   <base>/button/sw2      -> "PRESSED" / "RELEASED"
//   <base>/led/d1/set      <- "ON" / "OFF"   (subscribed)
//   <base>/led/d1/state    -> "ON" / "OFF"   (retained)
//   <base>/led/d2/set      <- "ON" / "OFF"   (subscribed)
//   <base>/led/d2/state    -> "ON" / "OFF"   (retained)
//
// On each connection the device publishes retained Home Assistant discovery
// configuration messages under "homeassistant/..." so the two pushbuttons
// (binary_sensor) and two LEDs (light) appear automatically as one device.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "utils/ustdlib.h"
#include "utils/uartstdio.h"
#include "io.h"
#include "config.h"
#include "mqtt_client.h"
#include "mqtt_app.h"

//
// Home Assistant default discovery prefix.
//
#define HA_PREFIX           "homeassistant"

//
// The on-connect publish sequence is spread one item per service tick to keep
// each TCP segment small.  These are the step indices.
//
#define STEP_STATUS         1
#define STEP_DISC_SW1       2
#define STEP_DISC_SW2       3
#define STEP_DISC_D1        4
#define STEP_DISC_D2        5
#define STEP_STATE_D1       6
#define STEP_STATE_D2       7
#define STEP_SUB_D1         8
#define STEP_SUB_D2         9
#define STEP_DONE           10

//
// Stable device id derived from the MAC (e.g. "tm4c1294_a1b2c3").
//
static char g_pcDevId[24];

//
// Fully-qualified topic strings, rebuilt from the base topic at each start.
//
static char g_pcTopicStatus[CFG_TOPIC_LEN + 16];
static char g_pcTopicBtn1[CFG_TOPIC_LEN + 24];
static char g_pcTopicBtn2[CFG_TOPIC_LEN + 24];
static char g_pcTopicLed1Set[CFG_TOPIC_LEN + 24];
static char g_pcTopicLed1State[CFG_TOPIC_LEN + 24];
static char g_pcTopicLed2Set[CFG_TOPIC_LEN + 24];
static char g_pcTopicLed2State[CFG_TOPIC_LEN + 24];

static char g_pcBase[CFG_TOPIC_LEN];

//
// Connection-edge tracking and the post-connect publish sequencer.
//
static bool g_bWasConnected;
static int  g_iPubStep;

//
// Scratch buffers for building discovery topic/payload.
//
static char g_pcDiscTopic[96];
static char g_pcDiscPayload[384];

//*****************************************************************************
//
// Publish the current LED state (retained).
//
//*****************************************************************************
static void
MQTTAppPublishLedState(int iLed)
{
    const char *pcTopic = (iLed == 2) ? g_pcTopicLed2State : g_pcTopicLed1State;
    const char *pcMsg = io_get_user_led(iLed) ? "ON" : "OFF";
    MQTTClientPublish(pcTopic, (const uint8_t *)pcMsg, (uint16_t)strlen(pcMsg),
                      1);
}

//*****************************************************************************
//
// Incoming-message callback: handle LED command topics.
//
//*****************************************************************************
static void
MQTTAppMsgCB(const char *pcTopic, uint16_t ui16TopicLen,
             const uint8_t *pui8Payload, uint16_t ui16PayloadLen)
{
    bool bOn = (ui16PayloadLen >= 2) && (pui8Payload[0] == 'O') &&
               (pui8Payload[1] == 'N');
    int iLed = 0;

    if((ui16TopicLen == strlen(g_pcTopicLed1Set)) &&
       (memcmp(pcTopic, g_pcTopicLed1Set, ui16TopicLen) == 0))
    {
        iLed = 1;
    }
    else if((ui16TopicLen == strlen(g_pcTopicLed2Set)) &&
            (memcmp(pcTopic, g_pcTopicLed2Set, ui16TopicLen) == 0))
    {
        iLed = 2;
    }

    if(iLed)
    {
        io_set_user_led(iLed, bOn);
        MQTTAppPublishLedState(iLed);
        UARTprintf("MQTT: LED D%d -> %s\n", iLed, bOn ? "ON" : "OFF");
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
    usnprintf(g_pcTopicBtn1, sizeof(g_pcTopicBtn1), "%s/button/sw1", pcBase);
    usnprintf(g_pcTopicBtn2, sizeof(g_pcTopicBtn2), "%s/button/sw2", pcBase);
    usnprintf(g_pcTopicLed1Set, sizeof(g_pcTopicLed1Set), "%s/led/d1/set",
              pcBase);
    usnprintf(g_pcTopicLed1State, sizeof(g_pcTopicLed1State), "%s/led/d1/state",
              pcBase);
    usnprintf(g_pcTopicLed2Set, sizeof(g_pcTopicLed2Set), "%s/led/d2/set",
              pcBase);
    usnprintf(g_pcTopicLed2State, sizeof(g_pcTopicLed2State), "%s/led/d2/state",
              pcBase);
}

//*****************************************************************************
//
// Publish a Home Assistant discovery config (retained).  The "~" base-topic
// abbreviation keeps payloads short, and the shared "dev" block groups all
// entities under a single HA device.
//
//*****************************************************************************
static void
MQTTAppPublishDiscoveryButton(const char *pcComponent, const char *pcObj,
                              const char *pcName, const char *pcStateSub)
{
    usnprintf(g_pcDiscTopic, sizeof(g_pcDiscTopic),
              HA_PREFIX "/%s/%s/%s/config", pcComponent, g_pcDevId, pcObj);

    usnprintf(g_pcDiscPayload, sizeof(g_pcDiscPayload),
              "{\"~\":\"%s\",\"name\":\"%s\",\"uniq_id\":\"%s_%s\","
              "\"stat_t\":\"~/%s\",\"pl_on\":\"PRESSED\",\"pl_off\":\"RELEASED\","
              "\"avty_t\":\"~/status\",\"dev\":{\"ids\":[\"%s\"],"
              "\"name\":\"TM4C1294 MQTT IO\",\"mdl\":\"EK-TM4C1294XL\","
              "\"mf\":\"Texas Instruments\"}}",
              g_pcBase, pcName, g_pcDevId, pcObj, pcStateSub, g_pcDevId);

    MQTTClientPublish(g_pcDiscTopic, (const uint8_t *)g_pcDiscPayload,
                      (uint16_t)strlen(g_pcDiscPayload), 1);
}

static void
MQTTAppPublishDiscoveryLed(const char *pcObj, const char *pcName,
                           const char *pcSetSub, const char *pcStateSub)
{
    usnprintf(g_pcDiscTopic, sizeof(g_pcDiscTopic),
              HA_PREFIX "/light/%s/%s/config", g_pcDevId, pcObj);

    usnprintf(g_pcDiscPayload, sizeof(g_pcDiscPayload),
              "{\"~\":\"%s\",\"name\":\"%s\",\"uniq_id\":\"%s_%s\","
              "\"cmd_t\":\"~/%s\",\"stat_t\":\"~/%s\","
              "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"avty_t\":\"~/status\","
              "\"dev\":{\"ids\":[\"%s\"],\"name\":\"TM4C1294 MQTT IO\","
              "\"mdl\":\"EK-TM4C1294XL\",\"mf\":\"Texas Instruments\"}}",
              g_pcBase, pcName, g_pcDevId, pcObj, pcSetSub, pcStateSub,
              g_pcDevId);

    MQTTClientPublish(g_pcDiscTopic, (const uint8_t *)g_pcDiscPayload,
                      (uint16_t)strlen(g_pcDiscPayload), 1);
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
    switch(iStep)
    {
        case STEP_STATUS:
            MQTTClientPublish(g_pcTopicStatus, (const uint8_t *)"online", 6, 1);
            break;
        case STEP_DISC_SW1:
            MQTTAppPublishDiscoveryButton("binary_sensor", "sw1", "SW1",
                                          "button/sw1");
            break;
        case STEP_DISC_SW2:
            MQTTAppPublishDiscoveryButton("binary_sensor", "sw2", "SW2",
                                          "button/sw2");
            break;
        case STEP_DISC_D1:
            MQTTAppPublishDiscoveryLed("d1", "LED D1", "led/d1/set",
                                       "led/d1/state");
            break;
        case STEP_DISC_D2:
            MQTTAppPublishDiscoveryLed("d2", "LED D2", "led/d2/set",
                                       "led/d2/state");
            break;
        case STEP_STATE_D1:
            MQTTAppPublishLedState(1);
            break;
        case STEP_STATE_D2:
            MQTTAppPublishLedState(2);
            break;
        case STEP_SUB_D1:
            MQTTClientSubscribe(g_pcTopicLed1Set);
            break;
        case STEP_SUB_D2:
            MQTTClientSubscribe(g_pcTopicLed2Set);
            UARTprintf("MQTT: HA discovery published.\n");
            break;
        default:
            break;
    }
}

//*****************************************************************************
//
// Periodic service.  Detects the connect edge and drives the staggered
// post-connect publish sequence (status, discovery, LED state, subscribe).
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
        g_iPubStep = STEP_STATUS;
    }
    if(!bConnected)
    {
        g_iPubStep = 0;
    }
    g_bWasConnected = bConnected;

    //
    // Advance the publish sequence, one item per tick.
    //
    if(bConnected && (g_iPubStep > 0) && (g_iPubStep < STEP_DONE))
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
// Publish a pushbutton transition.
//
//*****************************************************************************
void
MQTTAppPublishButton(int iButton, bool bPressed)
{
    const char *pcTopic = (iButton == 2) ? g_pcTopicBtn2 : g_pcTopicBtn1;
    const char *pcMsg = bPressed ? "PRESSED" : "RELEASED";

    if(!MQTTClientIsReady())
    {
        return;
    }
    MQTTClientPublish(pcTopic, (const uint8_t *)pcMsg,
                      (uint16_t)strlen(pcMsg), 0);
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
