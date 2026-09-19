//*****************************************************************************
//
// mqtt_app.h - Application glue between the EEPROM configuration, the board
// I/O and the MQTT client.  Owns the topic scheme and connection lifecycle.
//
//*****************************************************************************

#ifndef __MQTT_APP_H__
#define __MQTT_APP_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Initialise the MQTT subsystem (call once at start-up).  pui8MAC is the
// 6-byte board MAC address, used to derive a stable Home Assistant device id.
//
void MQTTAppInit(const uint8_t *pui8MAC);

//
// (Re)start the connection using the current EEPROM configuration.  Safe to
// call again after the configuration changes via the web UI.  Does nothing if
// no broker host is configured.
//
void MQTTAppStart(void);

//
// Stop the MQTT connection.
//
void MQTTAppStop(void);

//
// Periodic service.  Call from the main loop with elapsed milliseconds.
//
void MQTTAppTick(uint32_t ui32ElapsedMs);

//
// Foundation-owned topic / identity strings, exposed read-only to the product
// (home-auto) MQTT layer in products/home_auto/app/ha_mqtt.c.  The foundation
// builds these at connect (MQTTAppInit/MQTTAppStart); the product reads them when
// composing its discovery/state topics and parsing incoming commands.  Plan 11.
//
const char *MQTTAppBaseTopic(void);    // configured base topic, e.g. "tm4c1294"
const char *MQTTAppDevId(void);        // stable HA device id, e.g. "tm4c1294_a1b2c3"
const char *MQTTAppStatusTopic(void);  // "<base>/status" availability topic

//
// Advance the staggered post-connect publish sequence one item per call.  Plan
// 11: this is product work driven by product_poll() (on CC35x1 under the TCP/IP
// core lock).  No-op / resets while the broker is disconnected.
//
void MQTTAppPubServiceTick(void);

//
// Publish one SN65HVS882 input channel's state (retained "ON"/"OFF").  Called
// from the input-chain scan when a channel transitions (switch-type inputs).
//
void MQTTAppPublishInput(int iInput, bool bOn);

//
// Publish a pushbutton click event (not retained) to the HA event topic.
// pcEvt is "single" or "double".  Called from the input-events callback.
//
void MQTTAppPublishInputEvent(int iInput, const char *pcEvt);

//
// Publish the on-board temperature (retained) to "<base>/temperature/state" as a
// decimal string in degrees Celsius.  i32CentiC is centi-degrees; bValid=false
// (no reading yet) or a disconnected client skips the publish.  Called from the
// platform main loop's periodic (5 s) temperature poll.
//
void MQTTAppPublishTemp(int32_t i32CentiC, bool bValid);

//
// Re-run the post-connect publish sequence without reconnecting.  Call after
// I/O configuration changes to push updated HA discovery and state.
//
void MQTTAppRepublish(void);

//
// Set one relay output and publish its new retained state to MQTT.  Used by
// the local input→output binding logic in enet_io.c.
//
void MQTTAppSetRelay(int iRelay, bool bOn);

//
// Publish a shutter's cover state (retained): "opening" | "closing" | "open" |
// "closed" | "stopped".  Called from the shutter state machine in output_ctrl.c.
// Topic: <base>/cover/<N>/state.  No-op when the broker is not connected.
//
void MQTTAppPublishCoverState(int iShutter, const char *pcState);

//
// True once connected to the broker (CONNACK received).
//
bool MQTTAppIsConnected(void);

//
// A short human-readable status string for the web UI (e.g. "Connected").
//
const char *MQTTAppStatusStr(void);

#ifdef __cplusplus
}
#endif

#endif // __MQTT_APP_H__
