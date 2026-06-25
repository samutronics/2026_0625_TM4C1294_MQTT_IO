//*****************************************************************************
//
// mqtt_app.c - Application glue between configuration, board I/O and the MQTT
// client.
//
// Topic scheme (base topic is configurable on the web page):
//   <base>/status        -> "online" (retained) / LWT "offline"
//   <base>/button/sw1     -> "PRESSED" / "RELEASED"
//   <base>/button/sw2     -> "PRESSED" / "RELEASED"
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "utils/ustdlib.h"
#include "utils/uartstdio.h"
#include "config.h"
#include "mqtt_client.h"
#include "mqtt_app.h"

//
// Fully-qualified topic strings, rebuilt from the base topic at each start.
//
static char g_pcTopicStatus[CFG_TOPIC_LEN + 16];
static char g_pcTopicBtn1[CFG_TOPIC_LEN + 24];
static char g_pcTopicBtn2[CFG_TOPIC_LEN + 24];

//
// Tracks the connection edge so we can publish the retained "online" status
// exactly once per (re)connection.
//
static bool g_bWasConnected;

//*****************************************************************************
//
// Incoming-message callback.  v1 exposes the buttons as publishers only, so
// this simply logs anything received (the subscribe path is left in place for
// future control topics such as LEDs).
//
//*****************************************************************************
static void
MQTTAppMsgCB(const char *pcTopic, uint16_t ui16TopicLen,
             const uint8_t *pui8Payload, uint16_t ui16PayloadLen)
{
    (void)pui8Payload;
    UARTprintf("MQTT: rx on %.*s (%d bytes)\n", ui16TopicLen, pcTopic,
               ui16PayloadLen);
}

//*****************************************************************************
//
// Build the topic strings from the configured base topic.
//
//*****************************************************************************
static void
MQTTAppBuildTopics(const char *pcBase)
{
    usnprintf(g_pcTopicStatus, sizeof(g_pcTopicStatus), "%s/status", pcBase);
    usnprintf(g_pcTopicBtn1, sizeof(g_pcTopicBtn1), "%s/button/sw1", pcBase);
    usnprintf(g_pcTopicBtn2, sizeof(g_pcTopicBtn2), "%s/button/sw2", pcBase);
}

//*****************************************************************************
//
// Initialise the MQTT subsystem.
//
//*****************************************************************************
void
MQTTAppInit(void)
{
    g_bWasConnected = false;
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
}

//*****************************************************************************
//
// Periodic service.  Detects the connect edge and publishes the retained
// "online" status once per connection.
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
        // Freshly connected: announce ourselves (retained).
        //
        MQTTClientPublish(g_pcTopicStatus, (const uint8_t *)"online", 6, 1);
    }
    g_bWasConnected = bConnected;
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
    UARTprintf("MQTT: pub %s = %s\n", pcTopic, pcMsg);
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
