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
