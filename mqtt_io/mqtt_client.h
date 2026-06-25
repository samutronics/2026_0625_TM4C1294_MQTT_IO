//*****************************************************************************
//
// mqtt_client.h - Minimal MQTT 3.1.1 client over the lwIP 1.4.1 raw TCP API.
//
// Single connection, QoS 0 publish/subscribe, keep-alive ping, last will and
// automatic reconnect.  Designed for the bare-metal (NO_SYS) lwIP build: all
// callbacks run in lwIP (Ethernet interrupt) context and MQTTClientTick() is
// driven from the application's periodic tick.
//
//*****************************************************************************

#ifndef __MQTT_CLIENT_H__
#define __MQTT_CLIENT_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Connection state.
//
typedef enum
{
    MQTT_CLI_IDLE = 0,      // Disconnected / waiting for (re)connect backoff.
    MQTT_CLI_DNS,           // Resolving broker hostname.
    MQTT_CLI_TCP,           // TCP connecting.
    MQTT_CLI_CONNECT,       // TCP up, CONNECT sent, awaiting CONNACK.
    MQTT_CLI_READY          // CONNACK received - fully connected.
}
tMQTTCliState;

//
// Callback invoked when a PUBLISH is received on a subscribed topic.  The
// topic and payload pointers are only valid for the duration of the call.
//
typedef void (*tMQTTPubCB)(const char *pcTopic, uint16_t ui16TopicLen,
                           const uint8_t *pui8Payload, uint16_t ui16PayloadLen);

//
// Initialise the client.  pfnPub may be NULL if subscriptions are not used.
//
void MQTTClientInit(tMQTTPubCB pfnPub);

//
// Configure the last-will message published by the broker if the client
// drops unexpectedly.  Must be called before MQTTClientStart().  Pass NULL
// topic to disable.
//
void MQTTClientSetWill(const char *pcTopic, const char *pcMsg,
                       uint8_t ui8Retain);

//
// Begin connecting to the broker with the supplied credentials.  Empty user
// string disables authentication.  The strings are copied internally.
//
void MQTTClientStart(const char *pcHost, uint16_t ui16Port,
                     const char *pcClientID, const char *pcUser,
                     const char *pcPass);

//
// Tear down any active connection and stop reconnecting.
//
void MQTTClientStop(void);

//
// True once CONNACK has been received.
//
bool MQTTClientIsReady(void);

//
// Current connection state.
//
tMQTTCliState MQTTClientState(void);

//
// Publish a message (QoS 0).  Returns 0 on success, negative on error
// (not connected or send failure).
//
int MQTTClientPublish(const char *pcTopic, const uint8_t *pui8Payload,
                      uint16_t ui16Len, uint8_t ui8Retain);

//
// Subscribe to a topic (QoS 0).  Returns 0 on success.
//
int MQTTClientSubscribe(const char *pcTopic);

//
// Drive keep-alive and reconnect timers.  Call periodically with the number
// of milliseconds elapsed since the previous call.
//
void MQTTClientTick(uint32_t ui32ElapsedMs);

#ifdef __cplusplus
}
#endif

#endif // __MQTT_CLIENT_H__
