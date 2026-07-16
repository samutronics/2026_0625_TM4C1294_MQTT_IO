//*****************************************************************************
//
// config.c - Persistent MQTT configuration stored in on-chip EEPROM.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/eeprom.h"
#include "driverlib/sysctl.h"
#include "utils/uartstdio.h"
#include "config.h"
#include "ota.h"

//
// Marker identifying a valid configuration record ("MQT1").
//
#define CFG_MAGIC               0x4D515431

//
// EEPROM byte address at which the record is stored.
//
#define CFG_EEPROM_ADDR         0

//
// Total on-chip EEPROM size on the TM4C1294NCPDT (6 KB).
//
#define CFG_EEPROM_SIZE         6144u

//
// Compile-time EEPROM map guards.  Each record occupies a fixed byte address;
// these assertions fail the build if a struct ever grows into the next record's
// slot or past the end of EEPROM — the exact failure that silently wiped stored
// config in the past when tOutputConfig was resized.  (The OTA pending flag
// occupies 8 bytes at CFG_OTA_EEPROM_ADDR, between BIND and NTP.)
//
_Static_assert(sizeof(tMQTTConfig) <= CFG_IO_EEPROM_ADDR,
               "tMQTTConfig overflows into the IOST record");
_Static_assert(CFG_IO_EEPROM_ADDR + sizeof(tIOSettings) <= CFG_IO_BINDINGS_ADDR,
               "tIOSettings overflows into the BIND record");
_Static_assert(CFG_IO_BINDINGS_ADDR + sizeof(tIOBindings) <= CFG_OTA_EEPROM_ADDR,
               "tIOBindings overflows into the OTA pending flag");
_Static_assert(CFG_OTA_EEPROM_ADDR + 8u <= CFG_NTP_EEPROM_ADDR,
               "OTA pending flag overflows into the NTPC record");
_Static_assert(CFG_NTP_EEPROM_ADDR + sizeof(tNTPConfig) <= CFG_IO_NAMES_ADDR,
               "tNTPConfig overflows into the NMES record");
_Static_assert(CFG_IO_NAMES_ADDR + sizeof(tIONames) <= CFG_OUTCFG_ADDR,
               "tIONames overflows into the OUTC record");
_Static_assert(CFG_OUTCFG_ADDR + sizeof(tOutputConfig) <= CFG_ROOMCFG_ADDR,
               "tOutputConfig overflows into the ROOM record — add a NEW record, "
               "never grow OUTC past CFG_ROOMCFG_ADDR");
_Static_assert(CFG_ROOMCFG_ADDR + sizeof(tRoomConfig) <= CFG_EEPROM_SIZE,
               "tRoomConfig overflows the end of EEPROM");
//
// Targeted per-name EEPROM writes (ConfigNameSet) require the name stride and the
// names payload base to be 4-byte aligned, or EEPROMProgram faults/corrupts.
//
_Static_assert(CFG_NAME_LEN % 4u == 0u,
               "CFG_NAME_LEN must be a multiple of 4 for targeted name writes");
_Static_assert((CFG_IO_NAMES_ADDR + 4u) % 4u == 0u,
               "names payload base must be 4-byte aligned");

//
// The live, in-RAM copies of the two EEPROM records.
//
static tMQTTConfig   g_sConfig;
static tIOSettings   g_sIOSettings;
static tIOBindings   g_sBindings;
static tNTPConfig    g_sNTPConfig;
static tIONames      g_sIONames;
static tOutputConfig g_sOutCfg;
static tRoomConfig   g_sRoomCfg;

//*****************************************************************************
//
// Compute a CRC32 (IEEE 802.3, reflected) over a buffer.  Used to validate the
// stored record.  A small table-less implementation is sufficient here.
//
//*****************************************************************************
uint32_t
ConfigCRC32(const uint8_t *pui8Data, uint32_t ui32Len)
{
    uint32_t ui32Crc, ui32Bit, ui32I;

    ui32Crc = 0xFFFFFFFF;
    for(ui32I = 0; ui32I < ui32Len; ui32I++)
    {
        ui32Crc ^= pui8Data[ui32I];
        for(ui32Bit = 0; ui32Bit < 8; ui32Bit++)
        {
            if(ui32Crc & 1)
            {
                ui32Crc = (ui32Crc >> 1) ^ 0xEDB88320;
            }
            else
            {
                ui32Crc >>= 1;
            }
        }
    }
    return(ui32Crc ^ 0xFFFFFFFF);
}

//*****************************************************************************
//
// Populate a record with the compiled-in defaults.
//
//*****************************************************************************
void
ConfigSetDefaults(tMQTTConfig *psCfg)
{
    memset(psCfg, 0, sizeof(tMQTTConfig));
    psCfg->ui32Magic = CFG_MAGIC;
    psCfg->ui16Port = 1883;
    psCfg->ui8UseAuth = 0;
    psCfg->ui8IoDevices = (uint8_t)((CFG_RELAY_DEFAULT_DEVICES << 4) |
                                    CFG_DIN_DEFAULT_DEVICES);
    psCfg->pcHost[0] = '\0';
    strcpy(psCfg->pcClientID, "tm4c1294");
    strcpy(psCfg->pcTopicBase, "tm4c1294");
}

static bool ConfigOutputMigrateV1(void);   // one-time upgrade of the pre-32-shutter record

//*****************************************************************************
//
// Initialise the EEPROM and load the stored configuration.
//
//*****************************************************************************
void
ConfigInit(void)
{
    uint32_t ui32Crc;

    //
    // Enable and initialise the EEPROM peripheral.
    //
    SysCtlPeripheralEnable(SYSCTL_PERIPH_EEPROM0);
    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_EEPROM0))
    {
    }
    if(EEPROMInit() != EEPROM_INIT_OK)
    {
        UARTprintf("EEPROM init failed; using default config.\n");
        ConfigSetDefaults(&g_sConfig);
        return;
    }

    //
    // Read the stored record.
    //
    EEPROMRead((uint32_t *)&g_sConfig, CFG_EEPROM_ADDR, sizeof(tMQTTConfig));

    //
    // Validate the magic marker and CRC.
    //
    ui32Crc = ConfigCRC32((const uint8_t *)&g_sConfig,
                          sizeof(tMQTTConfig) - sizeof(uint32_t));
    if((g_sConfig.ui32Magic != CFG_MAGIC) || (g_sConfig.ui32Crc != ui32Crc))
    {
        UARTprintf("No valid config in EEPROM; applying defaults.\n");
        ConfigSetDefaults(&g_sConfig);
    }
    else
    {
        UARTprintf("Loaded MQTT config: broker '%s:%d'\n",
                   g_sConfig.pcHost, g_sConfig.ui16Port);
    }

    //
    // Clamp the packed device counts to sane values.  This also upgrades older
    // records (whose reserved byte held only the input count, or read as 0) to a
    // working default without discarding the rest of the stored configuration.
    //
    ConfigSetDinDevices(g_sConfig.ui8IoDevices & 0x0F);
    ConfigSetRelayDevices((g_sConfig.ui8IoDevices >> 4) & 0x0F);

    //
    // Load the I/O settings record (per-input type) from its own EEPROM block.
    // A missing or corrupt record silently defaults to all-switches (all zeros).
    //
    EEPROMRead((uint32_t *)&g_sIOSettings, CFG_IO_EEPROM_ADDR,
               sizeof(tIOSettings));
    ui32Crc = ConfigCRC32((const uint8_t *)&g_sIOSettings,
                          sizeof(tIOSettings) - sizeof(uint32_t));
    if((g_sIOSettings.ui32Magic != CFG_IO_MAGIC) ||
       (g_sIOSettings.ui32Crc != ui32Crc))
    {
        memset(&g_sIOSettings, 0, sizeof(tIOSettings));
        g_sIOSettings.ui32Magic = CFG_IO_MAGIC;
        UARTprintf("No I/O settings in EEPROM; all inputs default to switch.\n");
    }

    //
    // Load the binding table (per-input → relay action map).
    // On invalid record default to all slots disabled (all zeros).
    //
    EEPROMRead((uint32_t *)&g_sBindings, CFG_IO_BINDINGS_ADDR,
               sizeof(tIOBindings));
    ui32Crc = ConfigCRC32((const uint8_t *)&g_sBindings,
                          sizeof(tIOBindings) - sizeof(uint32_t));
    if((g_sBindings.ui32Magic != CFG_IO_BINDINGS_MAGIC) ||
       (g_sBindings.ui32Crc != ui32Crc))
    {
        int iIn, iSlot;
        memset(&g_sBindings, 0, sizeof(tIOBindings));
        g_sBindings.ui32Magic = CFG_IO_BINDINGS_MAGIC;
        //
        // Default all slots to "On Change" trigger with no output.
        // The output stays BIND_OUTPUT_NONE so nothing fires until the user
        // picks a relay; the trigger is already set so they only need one
        // selection in the web UI.
        //
        for(iIn = 0; iIn < CFG_MAX_INPUTS; iIn++)
        {
            for(iSlot = 0; iSlot < CFG_BIND_SLOTS; iSlot++)
            {
                int iIdx = iIn * CFG_BIND_SLOTS + iSlot;
                g_sBindings.ui8TrigAct[iIdx] = BIND_TRIG_CHANGE; // act=0 (ON), trig=5
                g_sBindings.ui8Output[iIdx]  = BIND_OUTPUT_NONE;
            }
        }
        UARTprintf("No binding config in EEPROM; defaults: On Change, output=None.\n");
    }

    //
    // Load NTP configuration.  Default: pool.ntp.org, TZ offset 0.
    //
    EEPROMRead((uint32_t *)&g_sNTPConfig, CFG_NTP_EEPROM_ADDR,
               sizeof(tNTPConfig));
    ui32Crc = ConfigCRC32((const uint8_t *)&g_sNTPConfig,
                          sizeof(tNTPConfig) - sizeof(uint32_t));
    if((g_sNTPConfig.ui32Magic != CFG_NTP_MAGIC) ||
       (g_sNTPConfig.ui32Crc != ui32Crc))
    {
        memset(&g_sNTPConfig, 0, sizeof(tNTPConfig));
        g_sNTPConfig.ui32Magic = CFG_NTP_MAGIC;
        strncpy(g_sNTPConfig.pcServer, "pool.ntp.org",
                CFG_NTP_SERVER_LEN - 1);
        g_sNTPConfig.i8TzOffset = 0;
        UARTprintf("No NTP config in EEPROM; using pool.ntp.org UTC+0.\n");
    }

    //
    // Load channel names record.  On invalid or missing record all names
    // default to empty strings (channels use generated labels In01/Out01).
    //
    EEPROMRead((uint32_t *)&g_sIONames, CFG_IO_NAMES_ADDR,
               sizeof(tIONames));
    ui32Crc = ConfigCRC32((const uint8_t *)&g_sIONames,
                          sizeof(tIONames) - sizeof(uint32_t));
    if((g_sIONames.ui32Magic != CFG_IO_NAMES_MAGIC) ||
       (g_sIONames.ui32Crc != ui32Crc))
    {
        memset(&g_sIONames, 0, sizeof(tIONames));
        g_sIONames.ui32Magic = CFG_IO_NAMES_MAGIC;
        // Write the zeroed defaults to EEPROM immediately so that subsequent
        // targeted ConfigNameSet() writes produce a CRC that matches what
        // EEPROMRead() will return on the next boot.  Without this, unwritten
        // name slots retain 0xFF (or stale bytes), causing a CRC mismatch.
        ConfigNamesSave();
        UARTprintf("No names config in EEPROM; using generated labels.\n");
    }

    //
    // Load per-output behavior (mode + timed duration + shutter table).
    // On invalid or missing record apply defaults (all Standard, 1000 ms, no
    // shutters) and write them back so the CRC matches on the next boot.
    //
    EEPROMRead((uint32_t *)&g_sOutCfg, CFG_OUTCFG_ADDR, sizeof(tOutputConfig));
    ui32Crc = ConfigCRC32((const uint8_t *)&g_sOutCfg,
                          sizeof(tOutputConfig) - sizeof(uint32_t));
    if((g_sOutCfg.ui32Magic != CFG_OUTCFG_MAGIC) ||
       (g_sOutCfg.ui32Crc != ui32Crc))
    {
        //
        // The new-format record did not validate.  Before wiping to defaults,
        // try to upgrade an older (16-shutter, no-names) record written by a
        // previous firmware so modes + shutters survive a firmware update.
        //
        if(!ConfigOutputMigrateV1())
        {
            ConfigOutputSetDefaults();
            ConfigOutputSave();
            UARTprintf("No output config in EEPROM; all outputs default to Standard.\n");
        }
    }

    //
    // Load the room / zone assignments (separate record, so adding it never
    // disturbs any other config).  Missing or invalid -> everything unassigned,
    // written back so the CRC matches on the next boot.
    //
    EEPROMRead((uint32_t *)&g_sRoomCfg, CFG_ROOMCFG_ADDR, sizeof(tRoomConfig));
    ui32Crc = ConfigCRC32((const uint8_t *)&g_sRoomCfg,
                          sizeof(tRoomConfig) - sizeof(uint32_t));
    if((g_sRoomCfg.ui32Magic != CFG_ROOMCFG_MAGIC) ||
       (g_sRoomCfg.ui32Crc != ui32Crc))
    {
        ConfigRoomSetDefaults();
        ConfigRoomSave();
        UARTprintf("No room config in EEPROM; all outputs/shutters unassigned.\n");
    }
}

//*****************************************************************************
//
// Legacy tOutputConfig layout (magic "OUTC"): 16 shutter slots, no shutter
// names.  Kept only for one-time migration when the record grew to 32 shutters
// + names.  Returns true if a valid v1 record was found and upgraded in place.
//
//*****************************************************************************
#define CFG_OUTCFG_V1_SHUTTERS  16
typedef struct
{
    uint32_t ui32Magic;
    uint8_t  ui8Mode[CFG_MAX_OUTPUTS];
    uint32_t ui32TimedMs[CFG_MAX_OUTPUTS];
    uint8_t  ui8ShUp  [CFG_OUTCFG_V1_SHUTTERS];
    uint8_t  ui8ShDown[CFG_OUTCFG_V1_SHUTTERS];
    uint32_t ui32ShTravelMs[CFG_OUTCFG_V1_SHUTTERS];
    uint32_t ui32Crc;
}
tOutputConfigV1;   // 4+120+480+16+16+64+4 = 704 B

static bool
ConfigOutputMigrateV1(void)
{
    tOutputConfigV1 sV1;
    uint32_t        ui32Crc;
    int             i;

    EEPROMRead((uint32_t *)&sV1, CFG_OUTCFG_ADDR, sizeof(tOutputConfigV1));
    ui32Crc = ConfigCRC32((const uint8_t *)&sV1,
                          sizeof(tOutputConfigV1) - sizeof(uint32_t));
    if((sV1.ui32Magic != CFG_OUTCFG_MAGIC) || (sV1.ui32Crc != ui32Crc))
    {
        return(false);   // no valid legacy record
    }

    //
    // Valid legacy record: start from clean defaults (zeroes names), then copy
    // the modes, timed durations and the first 16 shutters across, and rewrite
    // in the new 32-shutter format.
    //
    ConfigOutputSetDefaults();
    for(i = 0; i < CFG_MAX_OUTPUTS; i++)
    {
        g_sOutCfg.ui8Mode[i]     = sV1.ui8Mode[i];
        g_sOutCfg.ui32TimedMs[i] = sV1.ui32TimedMs[i];
    }
    for(i = 0; i < CFG_OUTCFG_V1_SHUTTERS; i++)
    {
        g_sOutCfg.ui8ShUp[i]        = sV1.ui8ShUp[i];
        g_sOutCfg.ui8ShDown[i]      = sV1.ui8ShDown[i];
        g_sOutCfg.ui32ShTravelMs[i] = sV1.ui32ShTravelMs[i];
    }
    ConfigOutputSave();
    UARTprintf("Output config migrated from v1 (16 shutters, names blank).\n");
    return(true);
}

//*****************************************************************************
//
// Populate the in-RAM output-config record with compiled-in defaults:
// every output Standard with a 1000 ms timed duration, and no shutters.
//
//*****************************************************************************
void
ConfigOutputSetDefaults(void)
{
    int i;

    memset(&g_sOutCfg, 0, sizeof(tOutputConfig));
    g_sOutCfg.ui32Magic = CFG_OUTCFG_MAGIC;
    for(i = 0; i < CFG_MAX_OUTPUTS; i++)
    {
        g_sOutCfg.ui8Mode[i]    = OUT_MODE_STANDARD;
        g_sOutCfg.ui32TimedMs[i] = 1000u;
    }
    for(i = 0; i < CFG_MAX_SHUTTERS; i++)
    {
        g_sOutCfg.ui8ShUp[i]   = SHUTTER_NONE;
        g_sOutCfg.ui8ShDown[i] = SHUTTER_NONE;
        g_sOutCfg.ui32ShTravelMs[i] = 20000u;
    }
}

//*****************************************************************************
//
// Packed input/output device-count accessors.
//
//*****************************************************************************
uint8_t
ConfigGetDinDevices(void)
{
    return(g_sConfig.ui8IoDevices & 0x0F);
}

void
ConfigSetDinDevices(uint8_t ui8Devices)
{
    if(ui8Devices > CFG_DIN_MAX_DEVICES)
    {
        ui8Devices = CFG_DIN_DEFAULT_DEVICES;
    }
    g_sConfig.ui8IoDevices = (uint8_t)((g_sConfig.ui8IoDevices & 0xF0) |
                                       ui8Devices);
}

uint8_t
ConfigGetRelayDevices(void)
{
    return((g_sConfig.ui8IoDevices >> 4) & 0x0F);
}

void
ConfigSetRelayDevices(uint8_t ui8Devices)
{
    if(ui8Devices > CFG_RELAY_MAX_DEVICES)
    {
        ui8Devices = CFG_RELAY_DEFAULT_DEVICES;
    }
    g_sConfig.ui8IoDevices = (uint8_t)((g_sConfig.ui8IoDevices & 0x0F) |
                                       (ui8Devices << 4));
}

//*****************************************************************************
//
// Return a pointer to the live configuration.
//
//*****************************************************************************
tMQTTConfig *
ConfigGet(void)
{
    return(&g_sConfig);
}

//*****************************************************************************
//
// Persist the live configuration to EEPROM.
//
//*****************************************************************************
bool
ConfigSave(void)
{
    uint32_t ui32Rc;

    //
    // Stamp the record and recompute its CRC before writing.
    //
    g_sConfig.ui32Magic = CFG_MAGIC;
    g_sConfig.ui32Crc = ConfigCRC32((const uint8_t *)&g_sConfig,
                                    sizeof(tMQTTConfig) - sizeof(uint32_t));

    ui32Rc = EEPROMProgram((uint32_t *)&g_sConfig, CFG_EEPROM_ADDR,
                           sizeof(tMQTTConfig));
    if(ui32Rc != 0)
    {
        UARTprintf("EEPROM write failed (0x%x).\n", ui32Rc);
        return(false);
    }

    UARTprintf("MQTT config saved to EEPROM.\n");
    return(true);
}

//*****************************************************************************
//
// True if a broker host has been configured.
//
//*****************************************************************************
bool
ConfigHasBroker(void)
{
    return(g_sConfig.pcHost[0] != '\0');
}

//*****************************************************************************
//
// Per-input type accessors.  Bit i of g_sIOSettings.ui8InputType[] = 1 means
// input i is configured as a pushbutton (click-event); 0 = level switch.
//
//*****************************************************************************
bool
ConfigInputIsPushbutton(int iInput)
{
    if((iInput < 0) || (iInput >= CFG_MAX_INPUTS))
    {
        return(false);
    }
    return((g_sIOSettings.ui8InputType[iInput / 8] & (1u << (iInput % 8))) != 0);
}

void
ConfigSetInputPushbutton(int iInput, bool bPushbutton)
{
    if((iInput < 0) || (iInput >= CFG_MAX_INPUTS))
    {
        return;
    }
    if(bPushbutton)
    {
        g_sIOSettings.ui8InputType[iInput / 8] |= (uint8_t)(1u << (iInput % 8));
    }
    else
    {
        g_sIOSettings.ui8InputType[iInput / 8] &= (uint8_t)~(1u << (iInput % 8));
    }
}

//*****************************************************************************
//
// Persist the I/O settings to EEPROM.
//
//*****************************************************************************
bool
ConfigIOSave(void)
{
    uint32_t ui32Rc;

    g_sIOSettings.ui32Magic = CFG_IO_MAGIC;
    g_sIOSettings.ui32Crc = ConfigCRC32((const uint8_t *)&g_sIOSettings,
                                        sizeof(tIOSettings) - sizeof(uint32_t));

    ui32Rc = EEPROMProgram((uint32_t *)&g_sIOSettings, CFG_IO_EEPROM_ADDR,
                           sizeof(tIOSettings));
    if(ui32Rc != 0)
    {
        UARTprintf("EEPROM write failed (IO settings, 0x%x).\n", ui32Rc);
        return(false);
    }

    UARTprintf("I/O settings saved to EEPROM.\n");
    return(true);
}

//*****************************************************************************
//
// Binding table accessors.  iInput in [0, CFG_MAX_INPUTS), iSlot in [0, CFG_BIND_SLOTS).
//
//*****************************************************************************
uint8_t
ConfigBindingGetTrigAct(int iInput, int iSlot)
{
    if((iInput < 0) || (iInput >= CFG_MAX_INPUTS) ||
       (iSlot < 0)  || (iSlot >= CFG_BIND_SLOTS))
    {
        return(0);
    }
    return(g_sBindings.ui8TrigAct[iInput * CFG_BIND_SLOTS + iSlot]);
}

uint8_t
ConfigBindingGetOutput(int iInput, int iSlot)
{
    if((iInput < 0) || (iInput >= CFG_MAX_INPUTS) ||
       (iSlot < 0)  || (iSlot >= CFG_BIND_SLOTS))
    {
        return(BIND_OUTPUT_NONE);
    }
    return(g_sBindings.ui8Output[iInput * CFG_BIND_SLOTS + iSlot]);
}

void
ConfigBindingSet(int iInput, int iSlot, uint8_t ui8TrigAct, uint8_t ui8Output)
{
    if((iInput < 0) || (iInput >= CFG_MAX_INPUTS) ||
       (iSlot < 0)  || (iSlot >= CFG_BIND_SLOTS))
    {
        return;
    }

    //
    // Reject a malformed slot: the trigger (bits 2:0) must be a known code and
    // the output must be a real relay index or BIND_OUTPUT_NONE.  Store anything
    // else as an unused slot so bad data (e.g. a hand-crafted /iocfg.cgi request)
    // can never drive relay logic.
    //
    if(((ui8TrigAct & 0x07u) > BIND_TRIG_CHANGE) ||
       ((ui8Output != BIND_OUTPUT_NONE) && (ui8Output >= CFG_MAX_OUTPUTS)))
    {
        ui8TrigAct = 0u;
        ui8Output  = BIND_OUTPUT_NONE;
    }

    g_sBindings.ui8TrigAct[iInput * CFG_BIND_SLOTS + iSlot] = ui8TrigAct;
    g_sBindings.ui8Output [iInput * CFG_BIND_SLOTS + iSlot] = ui8Output;
}

//*****************************************************************************
//
// Persist the binding table to EEPROM.
//
//*****************************************************************************
bool
ConfigBindingSave(void)
{
    uint32_t ui32Rc;

    g_sBindings.ui32Magic = CFG_IO_BINDINGS_MAGIC;
    g_sBindings.ui32Crc = ConfigCRC32((const uint8_t *)&g_sBindings,
                                      sizeof(tIOBindings) - sizeof(uint32_t));

    ui32Rc = EEPROMProgram((uint32_t *)&g_sBindings, CFG_IO_BINDINGS_ADDR,
                           sizeof(tIOBindings));
    if(ui32Rc != 0)
    {
        UARTprintf("EEPROM write failed (bindings, 0x%x).\n", ui32Rc);
        return(false);
    }

    UARTprintf("Binding table saved to EEPROM.\n");
    return(true);
}

//*****************************************************************************
//
// OTA pending flag — a 2-word record at CFG_OTA_EEPROM_ADDR:
//   word 0: magic (OTA_EEPROM_MAGIC) when pending, else 0
//   word 1: firmware size in bytes
//
//*****************************************************************************
bool
ConfigOtaIsPending(void)
{
    uint32_t ui32Magic;
    EEPROMRead(&ui32Magic, CFG_OTA_EEPROM_ADDR, sizeof(uint32_t));
    return(ui32Magic == OTA_EEPROM_MAGIC);
}

void
ConfigOtaSetPending(uint32_t ui32Size)
{
    uint32_t aui32Rec[2] = { OTA_EEPROM_MAGIC, ui32Size };
    EEPROMProgram(aui32Rec, CFG_OTA_EEPROM_ADDR, sizeof(aui32Rec));
}

void
ConfigOtaClearPending(void)
{
    uint32_t aui32Rec[2] = { 0u, 0u };
    EEPROMProgram(aui32Rec, CFG_OTA_EEPROM_ADDR, sizeof(aui32Rec));
}

//*****************************************************************************
//
// ConfigFactoryReset — zero the magic word of every EEPROM record so that
// ConfigInit() on the next boot finds no valid data and falls back to the
// compiled-in defaults.  All four records are invalidated atomically-ish;
// power loss mid-way leaves at most some records at defaults, never corrupt.
//
//*****************************************************************************
void
ConfigFactoryReset(void)
{
    uint32_t ui32Zero = 0u;
    EEPROMProgram(&ui32Zero, CFG_EEPROM_ADDR,      4);   // tMQTTConfig
    EEPROMProgram(&ui32Zero, CFG_IO_EEPROM_ADDR,   4);   // tIOSettings
    EEPROMProgram(&ui32Zero, CFG_IO_BINDINGS_ADDR, 4);   // tIOBindings
    EEPROMProgram(&ui32Zero, CFG_OTA_EEPROM_ADDR,  4);   // OTA flag
    EEPROMProgram(&ui32Zero, CFG_NTP_EEPROM_ADDR,  4);   // tNTPConfig
    EEPROMProgram(&ui32Zero, CFG_IO_NAMES_ADDR,    4);   // tIONames
    EEPROMProgram(&ui32Zero, CFG_OUTCFG_ADDR,      4);   // tOutputConfig
    EEPROMProgram(&ui32Zero, CFG_ROOMCFG_ADDR,     4);   // tRoomConfig
    UARTprintf("Config: EEPROM factory reset complete.\n");
}

//*****************************************************************************
//
// Per-output mode accessors.
//
//*****************************************************************************
uint8_t
ConfigOutMode(int iOut)
{
    if((iOut < 0) || (iOut >= CFG_MAX_OUTPUTS)) { return(OUT_MODE_STANDARD); }
    return(g_sOutCfg.ui8Mode[iOut]);
}

void
ConfigSetOutMode(int iOut, uint8_t ui8Mode)
{
    if((iOut < 0) || (iOut >= CFG_MAX_OUTPUTS)) { return; }
    if(ui8Mode > OUT_MODE_TIMED) { ui8Mode = OUT_MODE_STANDARD; }
    g_sOutCfg.ui8Mode[iOut] = ui8Mode;
}

uint32_t
ConfigOutTimedMs(int iOut)
{
    if((iOut < 0) || (iOut >= CFG_MAX_OUTPUTS)) { return(1000u); }
    return(g_sOutCfg.ui32TimedMs[iOut]);
}

void
ConfigSetOutTimedMs(int iOut, uint32_t ui32Ms)
{
    if((iOut < 0) || (iOut >= CFG_MAX_OUTPUTS)) { return; }
    if(ui32Ms < 1u)          { ui32Ms = 1u; }
    if(ui32Ms > 3600000u)    { ui32Ms = 3600000u; }
    g_sOutCfg.ui32TimedMs[iOut] = ui32Ms;
}

//*****************************************************************************
//
// Shutter table accessors.
//
//*****************************************************************************
bool
ConfigShutterGet(int iSlot, uint8_t *pui8Up, uint8_t *pui8Down,
                 uint32_t *pui32TravelMs)
{
    if((iSlot < 0) || (iSlot >= CFG_MAX_SHUTTERS)) { return(false); }
    if(g_sOutCfg.ui8ShUp[iSlot] == SHUTTER_NONE)   { return(false); }
    if(pui8Up)        { *pui8Up        = g_sOutCfg.ui8ShUp[iSlot]; }
    if(pui8Down)      { *pui8Down      = g_sOutCfg.ui8ShDown[iSlot]; }
    if(pui32TravelMs) { *pui32TravelMs = g_sOutCfg.ui32ShTravelMs[iSlot]; }
    return(true);
}

void
ConfigShutterSet(int iSlot, uint8_t ui8Up, uint8_t ui8Down,
                 uint32_t ui32TravelMs)
{
    if((iSlot < 0) || (iSlot >= CFG_MAX_SHUTTERS)) { return; }

    //
    // Reject a dangerous/invalid pairing: the same relay for both directions, or
    // an index past the max output count.  Such a slot would let the shutter FSM
    // energize a relay it can never correctly release, so store it as empty.
    // (SHUTTER_NONE is 0xFF = CFG_MAX_OUTPUTS-out-of-range, so the empty-slot
    // path via ConfigShutterClear is unaffected — it sets both to SHUTTER_NONE.)
    //
    if((ui8Up == ui8Down) ||
       (ui8Up >= CFG_MAX_OUTPUTS) || (ui8Down >= CFG_MAX_OUTPUTS))
    {
        g_sOutCfg.ui8ShUp[iSlot]   = SHUTTER_NONE;
        g_sOutCfg.ui8ShDown[iSlot] = SHUTTER_NONE;
        return;
    }

    if(ui32TravelMs < 1u)       { ui32TravelMs = 1u; }
    if(ui32TravelMs > 3600000u) { ui32TravelMs = 3600000u; }
    g_sOutCfg.ui8ShUp[iSlot]        = ui8Up;
    g_sOutCfg.ui8ShDown[iSlot]      = ui8Down;
    g_sOutCfg.ui32ShTravelMs[iSlot] = ui32TravelMs;
}

void
ConfigShutterClear(int iSlot)
{
    if((iSlot < 0) || (iSlot >= CFG_MAX_SHUTTERS)) { return; }
    g_sOutCfg.ui8ShUp[iSlot]   = SHUTTER_NONE;
    g_sOutCfg.ui8ShDown[iSlot] = SHUTTER_NONE;
    memset(g_sOutCfg.pcShName[iSlot], 0, CFG_NAME_LEN);
}

const char *
ConfigShutterName(int iSlot)
{
    if((iSlot < 0) || (iSlot >= CFG_MAX_SHUTTERS)) { return(""); }
    return(g_sOutCfg.pcShName[iSlot]);
}

void
ConfigShutterNameSet(int iSlot, const char *pcName)
{
    if((iSlot < 0) || (iSlot >= CFG_MAX_SHUTTERS)) { return; }
    memset(g_sOutCfg.pcShName[iSlot], 0, CFG_NAME_LEN);
    if(pcName) { strncpy(g_sOutCfg.pcShName[iSlot], pcName, CFG_NAME_LEN - 1); }
}

int
ConfigShutterOfRelay(int iOut, bool *pbIsUp)
{
    int i;
    if((iOut < 0) || (iOut >= CFG_MAX_OUTPUTS)) { return(-1); }
    for(i = 0; i < CFG_MAX_SHUTTERS; i++)
    {
        if(g_sOutCfg.ui8ShUp[i] == SHUTTER_NONE) { continue; }
        if((int)g_sOutCfg.ui8ShUp[i] == iOut)
        {
            if(pbIsUp) { *pbIsUp = true; }
            return(i);
        }
        if((int)g_sOutCfg.ui8ShDown[i] == iOut)
        {
            if(pbIsUp) { *pbIsUp = false; }
            return(i);
        }
    }
    return(-1);
}

//*****************************************************************************
//
// Persist the complete tOutputConfig record to EEPROM.
//
//*****************************************************************************
bool
ConfigOutputSave(void)
{
    uint32_t ui32Rc;

    g_sOutCfg.ui32Magic = CFG_OUTCFG_MAGIC;
    g_sOutCfg.ui32Crc   = ConfigCRC32((const uint8_t *)&g_sOutCfg,
                                      sizeof(tOutputConfig) - sizeof(uint32_t));
    ui32Rc = EEPROMProgram((uint32_t *)&g_sOutCfg, CFG_OUTCFG_ADDR,
                           sizeof(tOutputConfig));
    if(ui32Rc != 0)
    {
        UARTprintf("EEPROM write failed (output config, 0x%x).\n", ui32Rc);
        return(false);
    }
    UARTprintf("Output config saved to EEPROM.\n");
    return(true);
}

//*****************************************************************************
//
// Room / zone accessors.  Room index 0..CFG_MAX_ROOMS-1; ROOM_NONE = unassigned.
// A room is "defined" on the UI side when its name is non-empty.
//
//*****************************************************************************
const char *
ConfigRoomName(int iRoom)
{
    if((iRoom < 0) || (iRoom >= CFG_MAX_ROOMS)) { return(""); }
    return(g_sRoomCfg.pcRoomName[iRoom]);
}

void
ConfigRoomNameSet(int iRoom, const char *pcName)
{
    if((iRoom < 0) || (iRoom >= CFG_MAX_ROOMS)) { return; }
    memset(g_sRoomCfg.pcRoomName[iRoom], 0, CFG_NAME_LEN);
    if(pcName) { strncpy(g_sRoomCfg.pcRoomName[iRoom], pcName, CFG_NAME_LEN - 1); }
}

uint8_t
ConfigOutRoom(int iOut)
{
    if((iOut < 0) || (iOut >= CFG_MAX_OUTPUTS)) { return(ROOM_NONE); }
    return(g_sRoomCfg.ui8OutRoom[iOut]);
}

void
ConfigOutRoomSet(int iOut, uint8_t ui8Room)
{
    if((iOut < 0) || (iOut >= CFG_MAX_OUTPUTS)) { return; }
    g_sRoomCfg.ui8OutRoom[iOut] = (ui8Room < CFG_MAX_ROOMS) ? ui8Room : ROOM_NONE;
}

uint8_t
ConfigShRoom(int iShutter)
{
    if((iShutter < 0) || (iShutter >= CFG_MAX_SHUTTERS)) { return(ROOM_NONE); }
    return(g_sRoomCfg.ui8ShRoom[iShutter]);
}

void
ConfigShRoomSet(int iShutter, uint8_t ui8Room)
{
    if((iShutter < 0) || (iShutter >= CFG_MAX_SHUTTERS)) { return; }
    g_sRoomCfg.ui8ShRoom[iShutter] = (ui8Room < CFG_MAX_ROOMS) ? ui8Room : ROOM_NONE;
}

void
ConfigRoomSetDefaults(void)
{
    int i;
    memset(&g_sRoomCfg, 0, sizeof(tRoomConfig));
    g_sRoomCfg.ui32Magic = CFG_ROOMCFG_MAGIC;
    for(i = 0; i < CFG_MAX_OUTPUTS; i++)  { g_sRoomCfg.ui8OutRoom[i] = ROOM_NONE; }
    for(i = 0; i < CFG_MAX_SHUTTERS; i++) { g_sRoomCfg.ui8ShRoom[i]  = ROOM_NONE; }
}

bool
ConfigRoomSave(void)
{
    uint32_t ui32Rc;

    g_sRoomCfg.ui32Magic = CFG_ROOMCFG_MAGIC;
    g_sRoomCfg.ui32Crc   = ConfigCRC32((const uint8_t *)&g_sRoomCfg,
                                       sizeof(tRoomConfig) - sizeof(uint32_t));
    ui32Rc = EEPROMProgram((uint32_t *)&g_sRoomCfg, CFG_ROOMCFG_ADDR,
                           sizeof(tRoomConfig));
    if(ui32Rc != 0)
    {
        UARTprintf("EEPROM write failed (room config, 0x%x).\n", ui32Rc);
        return(false);
    }
    UARTprintf("Room config saved to EEPROM.\n");
    return(true);
}

//*****************************************************************************
//
// NTP configuration accessors.
//
//*****************************************************************************
const tNTPConfig *
ConfigNtpGet(void)
{
    return(&g_sNTPConfig);
}

void
ConfigNtpSetServer(const char *pcServer)
{
    memset(g_sNTPConfig.pcServer, 0, CFG_NTP_SERVER_LEN);
    strncpy(g_sNTPConfig.pcServer, pcServer, CFG_NTP_SERVER_LEN - 1);
}

void
ConfigNtpSetTz(int8_t i8Offset)
{
    g_sNTPConfig.i8TzOffset = i8Offset;
}

//*****************************************************************************
//
// Channel name accessors and EEPROM update.
//
//*****************************************************************************
const char *
ConfigGetInputName(int iInput)
{
    if(iInput < 0 || iInput >= CFG_NAMES_MAX_INPUTS)
    {
        return("");
    }
    return(g_sIONames.pcInputNames[iInput]);
}

const char *
ConfigGetOutputName(int iOutput)
{
    if(iOutput < 0 || iOutput >= CFG_NAMES_MAX_OUTPUTS)
    {
        return("");
    }
    return(g_sIONames.pcOutputNames[iOutput]);
}

//
// Write one name entry to RAM, then do a targeted 12-byte EEPROM write plus
// a 4-byte CRC update — no full 1544-byte rewrite needed.
//
void
ConfigNameSet(bool bInput, int iIdx, const char *pcName)
{
    char     *pcDst;
    uint32_t ui32Addr;

    if(bInput)
    {
        if(iIdx < 0 || iIdx >= CFG_NAMES_MAX_INPUTS) { return; }
        pcDst    = g_sIONames.pcInputNames[iIdx];
        ui32Addr = CFG_IO_NAMES_ADDR + 4u +
                   (uint32_t)iIdx * CFG_NAME_LEN;
    }
    else
    {
        if(iIdx < 0 || iIdx >= CFG_NAMES_MAX_OUTPUTS) { return; }
        pcDst    = g_sIONames.pcOutputNames[iIdx];
        ui32Addr = CFG_IO_NAMES_ADDR + 4u +
                   CFG_NAMES_MAX_INPUTS * CFG_NAME_LEN +
                   (uint32_t)iIdx * CFG_NAME_LEN;
    }

    memset(pcDst, 0, CFG_NAME_LEN);
    strncpy(pcDst, pcName, CFG_NAME_LEN - 1);

    // Persist the magic word on every targeted write so the record survives a
    // reboot even if ConfigNamesSave() was never called (e.g. after factory reset).
    EEPROMProgram(&g_sIONames.ui32Magic, CFG_IO_NAMES_ADDR, 4u);
    EEPROMProgram((uint32_t *)(uintptr_t)pcDst, ui32Addr, CFG_NAME_LEN);

    g_sIONames.ui32Crc = ConfigCRC32((const uint8_t *)&g_sIONames,
                                      sizeof(tIONames) - sizeof(uint32_t));
    EEPROMProgram(&g_sIONames.ui32Crc,
                  CFG_IO_NAMES_ADDR + sizeof(tIONames) - sizeof(uint32_t), 4u);
}

bool
ConfigNamesSave(void)
{
    uint32_t ui32Rc;
    g_sIONames.ui32Magic = CFG_IO_NAMES_MAGIC;
    g_sIONames.ui32Crc   = ConfigCRC32((const uint8_t *)&g_sIONames,
                                        sizeof(tIONames) - sizeof(uint32_t));
    ui32Rc = EEPROMProgram((uint32_t *)&g_sIONames, CFG_IO_NAMES_ADDR,
                           sizeof(tIONames));
    return(ui32Rc == 0);
}

bool
ConfigNtpSave(void)
{
    uint32_t ui32Rc;
    g_sNTPConfig.ui32Magic = CFG_NTP_MAGIC;
    g_sNTPConfig.ui32Crc   = ConfigCRC32((const uint8_t *)&g_sNTPConfig,
                                          sizeof(tNTPConfig) - sizeof(uint32_t));
    ui32Rc = EEPROMProgram((uint32_t *)&g_sNTPConfig, CFG_NTP_EEPROM_ADDR,
                           sizeof(tNTPConfig));
    if(ui32Rc != 0)
    {
        UARTprintf("EEPROM write failed (NTP config, 0x%x).\n", ui32Rc);
        return(false);
    }
    UARTprintf("NTP config saved to EEPROM.\n");
    return(true);
}
