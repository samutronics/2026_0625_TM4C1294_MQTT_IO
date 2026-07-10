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
// Digital-I/O chain sizing.  The input (SN65HVS882) and output (DRV8860) device
// counts are packed one per nibble into a single config byte, so each is limited
// to 15 devices (120 channels) - use the ConfigGet/Set accessors below.  Packing
// keeps the record layout (and therefore existing stored broker settings) intact.
//
#define CFG_DIN_MAX_DEVICES       15    // up to 120 inputs
#define CFG_DIN_DEFAULT_DEVICES   2     // one input board (16 inputs)
#define CFG_RELAY_MAX_DEVICES     15    // up to 120 relays
#define CFG_RELAY_DEFAULT_DEVICES 2     // one output board (16 relays)

//
// Maximum number of individually-configurable inputs (one bit per input).
//
#define CFG_MAX_INPUTS  (CFG_DIN_MAX_DEVICES * 8)   // 120

//
// I/O settings record — stored at a separate EEPROM address so it can be
// written independently without disturbing the broker config above.
//
// Bit i of ui8InputType[] (byte i/8, bit i%8) = 1 → input i is a pushbutton
// (single/double-click event entity); 0 → level-sensitive switch (binary_sensor).
// 15 bytes cover all 120 possible inputs; ui8Rsvd pads to a 4-byte multiple.
//
#define CFG_IO_EEPROM_ADDR  256
#define CFG_IO_MAGIC        0x494F5354  // "IOST"

typedef struct
{
    uint32_t ui32Magic;
    uint8_t  ui8InputType[15];  // one bit per input channel
    uint8_t  ui8Rsvd;
    uint32_t ui32Crc;
}
tIOSettings;

//
// Persistent configuration record.  Laid out so the total size is a multiple
// of 4 bytes (required by the EEPROM block API).
//
typedef struct
{
    uint32_t ui32Magic;                 // Validity marker (CFG_MAGIC).
    uint16_t ui16Port;                  // Broker TCP port (default 1883).
    uint8_t  ui8UseAuth;                // Non-zero if username/password used.
    uint8_t  ui8IoDevices;              // Low nibble=SN65HVS882 count, high=DRV8860 count.
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

//
// Accessors for the packed input/output device counts.  The setters clamp to
// [1, CFG_*_MAX_DEVICES], falling back to the default for out-of-range values.
//
uint8_t ConfigGetDinDevices(void);
void    ConfigSetDinDevices(uint8_t ui8Devices);
uint8_t ConfigGetRelayDevices(void);
void    ConfigSetRelayDevices(uint8_t ui8Devices);

//
// I/O settings (per-input type) — stored in a separate EEPROM record so the
// broker config is never disturbed when only the I/O layout changes.
//
bool ConfigInputIsPushbutton(int iInput);
void ConfigSetInputPushbutton(int iInput, bool bPushbutton);
bool ConfigIOSave(void);

//
// Input-to-output binding table.  Each input has up to CFG_BIND_SLOTS slots,
// each binding one relay to one trigger event with one action.
//
// Slot byte ui8TrigAct: bits 2:0 = trigger (BIND_TRIG_*), bits 4:3 = action (BIND_ACT_*).
// Slot byte ui8Output : relay index; BIND_OUTPUT_NONE (0x7F) = slot unused.
//
#define CFG_BIND_SLOTS        4
#define CFG_IO_BINDINGS_ADDR  280
#define CFG_IO_BINDINGS_MAGIC 0x42494E44   // "BIND"

#define BIND_OUTPUT_NONE    0x7F

#define BIND_TRIG_NONE      0
#define BIND_TRIG_LEVEL_ON  1
#define BIND_TRIG_LEVEL_OFF 2
#define BIND_TRIG_SINGLE    3
#define BIND_TRIG_DOUBLE    4

#define BIND_ACT_ON         0
#define BIND_ACT_OFF        1
#define BIND_ACT_TOGGLE     2

typedef struct
{
    uint32_t ui32Magic;
    uint8_t  ui8TrigAct[CFG_MAX_INPUTS * CFG_BIND_SLOTS]; // index = i*SLOTS+s
    uint8_t  ui8Output [CFG_MAX_INPUTS * CFG_BIND_SLOTS];
    uint32_t ui32Crc;
}
tIOBindings;  // 4+480+480+4 = 968 B at addr 280; ends at 1248 (< 6144 EEPROM)

uint8_t ConfigBindingGetTrigAct(int iInput, int iSlot);
uint8_t ConfigBindingGetOutput (int iInput, int iSlot);
void    ConfigBindingSet(int iInput, int iSlot,
                         uint8_t ui8TrigAct, uint8_t ui8Output);
bool    ConfigBindingSave(void);

#ifdef __cplusplus
}
#endif

#endif // __CONFIG_H__
