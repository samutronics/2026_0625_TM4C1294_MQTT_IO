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
// Initialise the MQTT subsystem (call once at start-up).
//
void MQTTAppInit(void);

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
// True once connected to the broker (CONNACK received).
//
bool MQTTAppIsConnected(void);

//
// Publish a pushbutton transition.  iButton is 1 or 2; bPressed selects the
// "PRESSED"/"RELEASED" payload.
//
void MQTTAppPublishButton(int iButton, bool bPressed);

//
// A short human-readable status string for the web UI (e.g. "Connected").
//
const char *MQTTAppStatusStr(void);

#ifdef __cplusplus
}
#endif

#endif // __MQTT_APP_H__
