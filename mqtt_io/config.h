//*****************************************************************************
//
// config.h - Persistent MQTT configuration stored in on-chip EEPROM.
//
//*****************************************************************************

#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Field size limits (including the NUL terminator).
//
#define CFG_HOST_LEN        64
#define CFG_CLIENTID_LEN    32
#define CFG_USER_LEN        32
#define CFG_PASS_LEN        32
#define CFG_TOPIC_LEN       48

//
// Persistent configuration record.  Laid out so the total size is a multiple
// of 4 bytes (required by the EEPROM block API).
//
typedef struct
{
    uint32_t ui32Magic;                 // Validity marker (CFG_MAGIC).
    uint16_t ui16Port;                  // Broker TCP port (default 1883).
    uint8_t  ui8UseAuth;                // Non-zero if username/password used.
    uint8_t  ui8Pad;                    // Reserved (alignment).
    char     pcHost[CFG_HOST_LEN];      // Broker hostname or dotted-quad IP.
    char     pcClientID[CFG_CLIENTID_LEN];
    char     pcUser[CFG_USER_LEN];
    char     pcPass[CFG_PASS_LEN];
    char     pcTopicBase[CFG_TOPIC_LEN];// Base topic, e.g. "tm4c/dev1".
    uint32_t ui32Crc;                   // CRC32 of all preceding bytes.
}
tMQTTConfig;

//
// Initialise the EEPROM and load the configuration into RAM.  If the stored
// record is missing or corrupt, compiled-in defaults are applied (but not
// written back until ConfigSave() is called).
//
void ConfigInit(void);

//
// Return a pointer to the live in-RAM configuration.
//
tMQTTConfig *ConfigGet(void);

//
// Populate a record with the compiled-in defaults.
//
void ConfigSetDefaults(tMQTTConfig *psCfg);

//
// Persist the live configuration to EEPROM (recomputes the CRC).  Returns
// true on success.
//
bool ConfigSave(void);

//
// True if the live configuration has a non-empty broker host.
//
bool ConfigHasBroker(void);

#ifdef __cplusplus
}
#endif

#endif // __CONFIG_H__
