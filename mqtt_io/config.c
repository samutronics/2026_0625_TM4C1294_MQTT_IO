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
// The live, in-RAM copies of the two EEPROM records.
//
static tMQTTConfig g_sConfig;
static tIOSettings g_sIOSettings;
static tIOBindings g_sBindings;
static tNTPConfig  g_sNTPConfig;
static tIONames    g_sIONames;

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
    UARTprintf("Config: EEPROM factory reset complete.\n");
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
