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
#include "pal_str.h"
#include "pal_log.h"
#include "config.h"
#include "relay_chain.h"
#include "din_chain.h"
#include "io_scan.h"
#include "webui.h"
#include "mqtt_client.h"
#include "mqtt_app.h"
#include "input_events.h"
#include "relay_pulse.h"
#include "output_ctrl.h"
#include "product_api.h"    // Plan 11: product_on_connect() / product_on_mqtt() hooks

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
// Connection-edge tracking.  The post-connect publish sequencer itself is
// product work and lives in products/home_auto/app/ha_mqtt.c (Plan 11).
//
static bool g_bWasConnected;

//*****************************************************************************
//
// Foundation-owned topic / identity accessors (read-only), used by the product
// MQTT layer (ha_mqtt.c) to compose its discovery/state topics and parse
// incoming commands.  Declared in mqtt_app.h.  Plan 11.
//
//*****************************************************************************
const char *MQTTAppBaseTopic(void)   { return(g_pcBase); }
const char *MQTTAppDevId(void)       { return(g_pcDevId); }
const char *MQTTAppStatusTopic(void) { return(g_pcTopicStatus); }

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

    PalSnprintf(g_pcTopicStatus, sizeof(g_pcTopicStatus), "%s/status", pcBase);
}

//*****************************************************************************
//
// Foundation MQTT glue: incoming-message callback registered with the client.
// Routes every message to the product hook (product_on_mqtt).  The product
// owns all application semantics; the foundation only forwards.
//
//*****************************************************************************
static void
MQTTAppMsgCB(const char *pcTopic, uint16_t ui16TopicLen,
             const uint8_t *pui8Payload, uint16_t ui16PayloadLen)
{
    product_on_mqtt(pcTopic, ui16TopicLen, pui8Payload, ui16PayloadLen);
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

    PalSnprintf(g_pcDevId, sizeof(g_pcDevId), "tm4c1294_%02x%02x%02x",
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
        PalLog("MQTT: no broker configured; idle.\n");
        return;
    }

    MQTTAppBuildTopics(psCfg->pcTopicBase);

    //
    // Last will: broker publishes "offline" (retained) if we drop.
    //
    MQTTClientSetWill(g_pcTopicStatus, "offline", 1);

    g_bWasConnected = false;

    PalLog("MQTT: connecting to %s:%d as '%s'...\n", psCfg->pcHost,
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
// Periodic service (foundation MQTT glue).  Services the MQTT client and
// detects the connect rising edge, handing off to the product's
// product_on_connect() hook.  The staggered publish sequence itself is advanced
// by the product via MQTTAppPubServiceTick() from product_poll().
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
        product_on_connect();
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
