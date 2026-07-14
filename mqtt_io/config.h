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
uint32_t ConfigCRC32(const uint8_t *pui8Data, uint32_t ui32Len);

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
// [0, CFG_*_MAX_DEVICES]; 0 means no devices of that type are present.
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
#define BIND_TRIG_CHANGE    5   // fire on any level transition (switch inputs)

#define BIND_ACT_ON         0
#define BIND_ACT_OFF        1
#define BIND_ACT_TOGGLE     2
#define BIND_ACT_CYCLE      3   // shutter single-button cycle (up/stop/down)

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

//
// NTP configuration — stored at a separate EEPROM address so it can be
// written independently.  Defaults: pool.ntp.org, TZ offset 0.
//
#define CFG_NTP_EEPROM_ADDR  1260
#define CFG_NTP_MAGIC        0x4E545043u    // "NTPC"
#define CFG_NTP_SERVER_LEN   32

typedef struct
{
    uint32_t ui32Magic;
    char     pcServer[CFG_NTP_SERVER_LEN];  // NTP server hostname
    int8_t   i8TzOffset;                    // UTC hour offset, -12..14
    uint8_t  ui8Rsvd[3];
    uint32_t ui32Crc;
}
tNTPConfig;   // 44 B

const tNTPConfig *ConfigNtpGet(void);
void ConfigNtpSetServer(const char *pcServer);
void ConfigNtpSetTz(int8_t i8Offset);
bool ConfigNtpSave(void);

//
// Per-channel names — stored at CFG_IO_NAMES_ADDR.
// Each name is CFG_NAME_LEN bytes (11 printable chars + NUL).
// 64 input + 64 output names = 1536 B of payload; record total = 1544 B.
//
#define CFG_NAMES_MAX_INPUTS    64
#define CFG_NAMES_MAX_OUTPUTS   64
#define CFG_NAME_LEN            12      // 11 printable + NUL
#define CFG_IO_NAMES_ADDR       1304
#define CFG_IO_NAMES_MAGIC      0x4E4D4553u  // "NMES"

typedef struct
{
    uint32_t ui32Magic;
    char     pcInputNames [CFG_NAMES_MAX_INPUTS ][CFG_NAME_LEN];  // 768 B
    char     pcOutputNames[CFG_NAMES_MAX_OUTPUTS][CFG_NAME_LEN];  // 768 B
    uint32_t ui32Crc;
}
tIONames;   // 4 + 768 + 768 + 4 = 1544 B, ends at addr 2848

//
// Read a channel name.  Returns a pointer to the live in-RAM string (never
// NULL; empty string means the channel has no custom name).
//
const char *ConfigGetInputName(int iInput);
const char *ConfigGetOutputName(int iOutput);

//
// Update one channel name in RAM and write it directly to EEPROM (targeted
// 12-byte write + CRC update — avoids a full 1544-byte rewrite).
//
void ConfigNameSet(bool bInput, int iIdx, const char *pcName);

//
// Persist the complete tIONames record to EEPROM.  Normally ConfigNameSet()
// is sufficient; this is provided for bulk restore.
//
bool ConfigNamesSave(void);

//
// Per-output behavior — stored at CFG_OUTCFG_ADDR (first free address after the
// names record, which ends at 2848).  Each output is Standard (plain ON/OFF) or
// Timed (auto-OFF after ui32TimedMs).  Shutters pair two explicit relay indices
// (UP/DOWN) with a travel time; an empty slot has ui8ShUp == 0xFF.  Shutter
// membership of an output is derived from the shutter table, not the mode array.
//
#define CFG_MAX_OUTPUTS      (CFG_RELAY_MAX_DEVICES * 8)   // 120
#define CFG_MAX_SHUTTERS     16
#define CFG_OUTCFG_ADDR      2848
#define CFG_OUTCFG_MAGIC     0x4F555443u   // "OUTC"

#define OUT_MODE_STANDARD    0
#define OUT_MODE_TIMED       1

#define SHUTTER_NONE         0xFFu         // empty shutter slot / no member

typedef struct
{
    uint32_t ui32Magic;
    uint8_t  ui8Mode[CFG_MAX_OUTPUTS];         // OUT_MODE_* per output (120 B)
    uint32_t ui32TimedMs[CFG_MAX_OUTPUTS];     // auto-OFF ms per output (480 B)
    uint8_t  ui8ShUp  [CFG_MAX_SHUTTERS];      // UP relay index or SHUTTER_NONE
    uint8_t  ui8ShDown[CFG_MAX_SHUTTERS];      // DOWN relay index
    uint32_t ui32ShTravelMs[CFG_MAX_SHUTTERS]; // travel time ms
    uint32_t ui32Crc;
}
tOutputConfig;   // 4+120+480+16+16+64+4 = 704 B, ends at addr 3552

//
// Per-output mode accessors.
//
uint8_t  ConfigOutMode(int iOut);
void     ConfigSetOutMode(int iOut, uint8_t ui8Mode);
uint32_t ConfigOutTimedMs(int iOut);
void     ConfigSetOutTimedMs(int iOut, uint32_t ui32Ms);

//
// Shutter table accessors.  Slot 0..CFG_MAX_SHUTTERS-1.  A slot is empty when
// its UP index is SHUTTER_NONE.  ConfigShutterGet returns false for empty slots.
//
bool ConfigShutterGet(int iSlot, uint8_t *pui8Up, uint8_t *pui8Down,
                      uint32_t *pui32TravelMs);
void ConfigShutterSet(int iSlot, uint8_t ui8Up, uint8_t ui8Down,
                      uint32_t ui32TravelMs);
void ConfigShutterClear(int iSlot);

//
// Return the shutter slot that uses relay iOut (as UP or DOWN), or -1 if none.
// When found and pbIsUp is non-NULL, *pbIsUp is set true if iOut is the UP relay.
//
int  ConfigShutterOfRelay(int iOut, bool *pbIsUp);

//
// Apply compiled-in output defaults to the in-RAM record (all Standard,
// 1000 ms timed duration, no shutters).
//
void ConfigOutputSetDefaults(void);

//
// Persist the complete tOutputConfig record to EEPROM.
//
bool ConfigOutputSave(void);

//
// OTA pending flag in EEPROM.  Thin wrappers called by ota.c.
// The actual addresses / magic are defined in ota.h (included by ota.c).
//
bool    ConfigOtaIsPending(void);
void    ConfigOtaSetPending(uint32_t ui32Size);
void    ConfigOtaClearPending(void);

//
// Invalidate all EEPROM records so ConfigInit() reloads compiled-in defaults
// on the next boot.  Does not reset the in-RAM config; call after scheduling
// a system reset.
//
void    ConfigFactoryReset(void);

#ifdef __cplusplus
}
#endif

#endif // __CONFIG_H__
