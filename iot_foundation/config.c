//*****************************************************************************
//
// config.c - Persistent configuration stored in on-chip EEPROM.
//
// Plan 11 foundation/product split (decision 15).  This file is the FOUNDATION
// store engine.  The PRODUCT (home-auto schema) half was extracted to
// products/home_auto/app/product_config_store.c (Scope C); the foundation calls
// into it through two hooks declared in product_api.h.  What stays here:
//
//   FOUNDATION (store engine + foundation records):
//     ConfigCRC32, the EEPROM-map static-asserts, ConfigInit (foundation
//     records + the product_config_load() hand-off), ConfigSetDefaults,
//     ConfigGet/Save/HasBroker, the packed device-count accessors (the count
//     nibble lives in the foundation tMQTTConfig record), ConfigOta*,
//     ConfigFactoryReset (foundation records + the product hook), ConfigNtp*.
//
// KNOWN WARTS (deferred): the device-count accessors stay foundation because the
// count nibble is physically in tMQTTConfig; and config.h is not yet split (the
// product record types are still declared alongside the foundation ones).
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pal_log.h"
#include "pal_storage.h"
#include "config.h"
#include "ota.h"
#include "product_api.h"    // Plan 11: product_config_load/_factory_reset hooks


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
_Static_assert(CFG_ROOMCFG_ADDR + sizeof(tRoomConfig) <= CFG_WIFI_EEPROM_ADDR,
               "tRoomConfig overflows into the WIFI record");
_Static_assert(CFG_WIFI_EEPROM_ADDR <= CFG_EEPROM_SIZE,
               "WIFI record base past the end of EEPROM");
//
// Targeted per-name EEPROM writes (ConfigNameSet) require the name stride and the
// names payload base to be 4-byte aligned, or PalStorageWrite faults/corrupts.
//
_Static_assert(CFG_NAME_LEN % 4u == 0u,
               "CFG_NAME_LEN must be a multiple of 4 for targeted name writes");
_Static_assert((CFG_IO_NAMES_ADDR + 4u) % 4u == 0u,
               "names payload base must be 4-byte aligned");

//
// The live, in-RAM copies of the two EEPROM records.
//
static tMQTTConfig   g_sConfig;
static tNTPConfig    g_sNTPConfig;

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
    // Ready the persistent store (on-chip EEPROM on the TM4C).
    //
    if(!PalStorageInit())
    {
        PalLog("Storage init failed; using default config.\n");
        ConfigSetDefaults(&g_sConfig);
        return;
    }

    //
    // Read the stored record.
    //
    PalStorageRead((uint32_t *)&g_sConfig, CFG_EEPROM_ADDR, sizeof(tMQTTConfig));

    //
    // Validate the magic marker and CRC.
    //
    ui32Crc = ConfigCRC32((const uint8_t *)&g_sConfig,
                          sizeof(tMQTTConfig) - sizeof(uint32_t));
    if((g_sConfig.ui32Magic != CFG_MAGIC) || (g_sConfig.ui32Crc != ui32Crc))
    {
        PalLog("No valid config in EEPROM; applying defaults.\n");
        ConfigSetDefaults(&g_sConfig);
    }
    else
    {
        PalLog("Loaded MQTT config: broker '%s:%d'\n",
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
    // Load NTP configuration (foundation record).  Default: pool.ntp.org, TZ 0.
    //
    PalStorageRead((uint32_t *)&g_sNTPConfig, CFG_NTP_EEPROM_ADDR,
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
        PalLog("No NTP config in EEPROM; using pool.ntp.org UTC+0.\n");
    }

    //
    // Product config records (I/O settings, bindings, names, outputs, rooms) are
    // owned by the product; load / default / migrate them through the product
    // hook so the foundation store never enumerates the product schema (Plan 11).
    // The product records read AFTER the foundation ones and BEFORE product_init.
    //
    product_config_load();
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

    ui32Rc = PalStorageWrite((uint32_t *)&g_sConfig, CFG_EEPROM_ADDR,
                           sizeof(tMQTTConfig));
    if(ui32Rc != 0)
    {
        PalLog("EEPROM write failed (0x%x).\n", ui32Rc);
        return(false);
    }

    PalLog("MQTT config saved to EEPROM.\n");
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
// OTA pending flag — a 2-word record at CFG_OTA_EEPROM_ADDR:
//   word 0: magic (OTA_EEPROM_MAGIC) when pending, else 0
//   word 1: firmware size in bytes
//
//*****************************************************************************
bool
ConfigOtaIsPending(void)
{
    uint32_t ui32Magic;
    PalStorageRead(&ui32Magic, CFG_OTA_EEPROM_ADDR, sizeof(uint32_t));
    return(ui32Magic == OTA_EEPROM_MAGIC);
}

void
ConfigOtaSetPending(uint32_t ui32Size)
{
    uint32_t aui32Rec[2] = { OTA_EEPROM_MAGIC, ui32Size };
    if(PalStorageWrite(aui32Rec, CFG_OTA_EEPROM_ADDR, sizeof(aui32Rec)) != 0)
    {
        PalLog("EEPROM write failed (OTA pending flag).\n");
    }
}

void
ConfigOtaClearPending(void)
{
    uint32_t aui32Rec[2] = { 0u, 0u };
    if(PalStorageWrite(aui32Rec, CFG_OTA_EEPROM_ADDR, sizeof(aui32Rec)) != 0)
    {
        PalLog("EEPROM write failed (OTA clear flag).\n");
    }
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
    uint32_t ui32Rc   = 0u;

    //
    // Foundation records.  The product records are invalidated by the product
    // hook so the foundation store never names the product schema (Plan 11).
    //
    ui32Rc |= PalStorageWrite(&ui32Zero, CFG_EEPROM_ADDR,      4);   // tMQTTConfig
    ui32Rc |= PalStorageWrite(&ui32Zero, CFG_OTA_EEPROM_ADDR,  4);   // OTA flag
    ui32Rc |= PalStorageWrite(&ui32Zero, CFG_NTP_EEPROM_ADDR,  4);   // tNTPConfig
    product_config_factory_reset();
    if(ui32Rc != 0)
    {
        PalLog("Config: factory reset had EEPROM write error(s) (0x%x).\n",
                   ui32Rc);
    }
    else
    {
        PalLog("Config: EEPROM factory reset complete.\n");
    }
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


bool
ConfigNtpSave(void)
{
    uint32_t ui32Rc;
    g_sNTPConfig.ui32Magic = CFG_NTP_MAGIC;
    g_sNTPConfig.ui32Crc   = ConfigCRC32((const uint8_t *)&g_sNTPConfig,
                                          sizeof(tNTPConfig) - sizeof(uint32_t));
    ui32Rc = PalStorageWrite((uint32_t *)&g_sNTPConfig, CFG_NTP_EEPROM_ADDR,
                           sizeof(tNTPConfig));
    if(ui32Rc != 0)
    {
        PalLog("EEPROM write failed (NTP config, 0x%x).\n", ui32Rc);
        return(false);
    }
    PalLog("NTP config saved to EEPROM.\n");
    return(true);
}
