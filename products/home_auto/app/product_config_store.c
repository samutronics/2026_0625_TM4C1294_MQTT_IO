//*****************************************************************************
//
// product_config_store.c - Home-auto product configuration schema (Plan 11).
//
// Extracted from iot_foundation/config.c (Scope C, 2026-09). This is the
// PRODUCT half of the config store: the tIOSettings / tIOBindings / tIONames /
// tOutputConfig / tRoomConfig records plus their accessors, defaults and the
// one-time migration helper. It persists these records using ONLY the
// foundation store primitives (ConfigCRC32 + the PAL storage layer), so the
// dependency points product -> foundation.
//
// The foundation calls into here through exactly two hooks declared in
// product_api.h: product_config_load() (from ConfigInit(), after the foundation
// records load and before product_init()) and product_config_factory_reset()
// (from ConfigFactoryReset()). Every accessor below is declared in config.h and
// called by the foundation web/app layers unchanged.
//
// KNOWN WART (deferred): the device-count accessors stay foundation because
// their nibble lives in the foundation tMQTTConfig record, and config.h is not
// yet split (product record types still declared alongside the foundation ones).
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pal_log.h"
#include "pal_storage.h"
#include "config.h"
#include "product_api.h"    // Plan 11: product_config_load/_factory_reset hooks

//
// Platform seam (implemented in webui_platform.c on the CC35x1 / enet_io.c on the
// TM4C): number of platform-local digital inputs appended after the SPI chain -
// the CC35x1's two on-board buttons, or 0 on the TM4C.  Declared locally so this
// low-level persistence module needn't pull in the web-UI header; used only to
// default those inputs to Pushbutton on a fresh I/O settings record.
//
extern int WebPlatformLocalInputCount(void);

//
// The live, in-RAM copies of the product EEPROM records.
//
static tIOSettings   g_sIOSettings;
static tIOBindings   g_sBindings;
static tIONames      g_sIONames;
static tOutputConfig g_sOutCfg;
static tRoomConfig   g_sRoomCfg;

static bool ConfigOutputMigrateV1(void);   // one-time upgrade of the pre-32-shutter record

//*****************************************************************************
//
// product_config_load - Plan 11 product hook.  Load / default / migrate the
// product's config records.  Called by the foundation's ConfigInit() after the
// foundation records (broker, NTP) are loaded and before product_init().  This
// block was lifted verbatim out of ConfigInit().
//
//*****************************************************************************
void
product_config_load(void)
{
    uint32_t ui32Crc;

    //
    // Load the I/O settings record (per-input type) from its own EEPROM block.
    // A missing or corrupt record silently defaults to all-switches (all zeros).
    //
    PalStorageRead((uint32_t *)&g_sIOSettings, CFG_IO_EEPROM_ADDR,
               sizeof(tIOSettings));
    ui32Crc = ConfigCRC32((const uint8_t *)&g_sIOSettings,
                          sizeof(tIOSettings) - sizeof(uint32_t));
    if((g_sIOSettings.ui32Magic != CFG_IO_MAGIC) ||
       (g_sIOSettings.ui32Crc != ui32Crc))
    {
        int iBase = (int)ConfigGetDinDevices() * 8;
        int iLoc  = WebPlatformLocalInputCount();
        int i;

        memset(&g_sIOSettings, 0, sizeof(tIOSettings));
        g_sIOSettings.ui32Magic = CFG_IO_MAGIC;

        //
        // The platform-local inputs (CC35x1 on-board buttons) sit right after the
        // SPI inputs and are momentary, so default them to Pushbutton (single/
        // double-click events) rather than the all-switch default.  Still fully
        // user-changeable on the I/O Config page.
        //
        for(i = 0; (i < iLoc) && ((iBase + i) < CFG_MAX_INPUTS); i++)
        {
            ConfigSetInputPushbutton(iBase + i, true);
        }
        PalLog("No I/O settings in EEPROM; SPI inputs default to switch"
               "%s.\n", (iLoc > 0) ? ", local buttons to pushbutton" : "");
    }

    //
    // Load the binding table (per-input → relay action map).
    // On invalid record default to all slots disabled (all zeros).
    //
    PalStorageRead((uint32_t *)&g_sBindings, CFG_IO_BINDINGS_ADDR,
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
        PalLog("No binding config in EEPROM; defaults: On Change, output=None.\n");
    }

    //
    // Load channel names record.  On invalid or missing record all names
    // default to empty strings (channels use generated labels In01/Out01).
    //
    PalStorageRead((uint32_t *)&g_sIONames, CFG_IO_NAMES_ADDR,
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
        // PalStorageRead() will return on the next boot.  Without this, unwritten
        // name slots retain 0xFF (or stale bytes), causing a CRC mismatch.
        ConfigNamesSave();
        PalLog("No names config in EEPROM; using generated labels.\n");
    }

    //
    // Load per-output behavior (mode + timed duration + shutter table).
    // On invalid or missing record apply defaults (all Standard, 1000 ms, no
    // shutters) and write them back so the CRC matches on the next boot.
    //
    PalStorageRead((uint32_t *)&g_sOutCfg, CFG_OUTCFG_ADDR, sizeof(tOutputConfig));
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
            PalLog("No output config in EEPROM; all outputs default to Standard.\n");
        }
    }

    //
    // Load the room / zone assignments (separate record, so adding it never
    // disturbs any other config).  Missing or invalid -> everything unassigned,
    // written back so the CRC matches on the next boot.
    //
    PalStorageRead((uint32_t *)&g_sRoomCfg, CFG_ROOMCFG_ADDR, sizeof(tRoomConfig));
    ui32Crc = ConfigCRC32((const uint8_t *)&g_sRoomCfg,
                          sizeof(tRoomConfig) - sizeof(uint32_t));
    if((g_sRoomCfg.ui32Magic != CFG_ROOMCFG_MAGIC) ||
       (g_sRoomCfg.ui32Crc != ui32Crc))
    {
        ConfigRoomSetDefaults();
        ConfigRoomSave();
        PalLog("No room config in EEPROM; all outputs/shutters unassigned.\n");
    }
}

//*****************************************************************************
//
// product_config_factory_reset - Plan 11 product hook.  Zero the magic word of
// each product config record so ConfigInit() reloads product defaults on the
// next boot.  Called by the foundation's ConfigFactoryReset() (which handles the
// foundation records itself).
//
//*****************************************************************************
void
product_config_factory_reset(void)
{
    uint32_t ui32Zero = 0u;
    uint32_t ui32Rc   = 0u;
    ui32Rc |= PalStorageWrite(&ui32Zero, CFG_IO_EEPROM_ADDR,   4);   // tIOSettings
    ui32Rc |= PalStorageWrite(&ui32Zero, CFG_IO_BINDINGS_ADDR, 4);   // tIOBindings
    ui32Rc |= PalStorageWrite(&ui32Zero, CFG_IO_NAMES_ADDR,    4);   // tIONames
    ui32Rc |= PalStorageWrite(&ui32Zero, CFG_OUTCFG_ADDR,      4);   // tOutputConfig
    ui32Rc |= PalStorageWrite(&ui32Zero, CFG_ROOMCFG_ADDR,     4);   // tRoomConfig
    if(ui32Rc != 0)
    {
        PalLog("Product config: factory reset EEPROM write error(s) (0x%x).\n",
               ui32Rc);
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

    PalStorageRead((uint32_t *)&sV1, CFG_OUTCFG_ADDR, sizeof(tOutputConfigV1));
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
    PalLog("Output config migrated from v1 (16 shutters, names blank).\n");
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

    ui32Rc = PalStorageWrite((uint32_t *)&g_sIOSettings, CFG_IO_EEPROM_ADDR,
                           sizeof(tIOSettings));
    if(ui32Rc != 0)
    {
        PalLog("EEPROM write failed (IO settings, 0x%x).\n", ui32Rc);
        return(false);
    }

    PalLog("I/O settings saved to EEPROM.\n");
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

    ui32Rc = PalStorageWrite((uint32_t *)&g_sBindings, CFG_IO_BINDINGS_ADDR,
                           sizeof(tIOBindings));
    if(ui32Rc != 0)
    {
        PalLog("EEPROM write failed (bindings, 0x%x).\n", ui32Rc);
        return(false);
    }

    PalLog("Binding table saved to EEPROM.\n");
    return(true);
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
// Ensure no relay is used by more than one shutter (as UP or DOWN).  Two shutters
// sharing a relay would drive it from two independent FSMs with unsynchronised
// interlocks — a hardware-conflict path.  Any later slot that collides with an
// earlier valid one is emptied.  Returns the number of slots cleared.
//
//*****************************************************************************
static int
ConfigShutterDedup(void)
{
    bool    bUsed[CFG_MAX_OUTPUTS];
    int     iSlot, iCleared = 0;
    uint8_t ui8Up, ui8Dn;

    memset(bUsed, 0, sizeof(bUsed));
    for(iSlot = 0; iSlot < CFG_MAX_SHUTTERS; iSlot++)
    {
        ui8Up = g_sOutCfg.ui8ShUp[iSlot];
        ui8Dn = g_sOutCfg.ui8ShDown[iSlot];
        if((ui8Up >= CFG_MAX_OUTPUTS) || (ui8Dn >= CFG_MAX_OUTPUTS))
        {
            continue;                       // empty / invalid slot
        }
        if(bUsed[ui8Up] || bUsed[ui8Dn])
        {
            g_sOutCfg.ui8ShUp[iSlot]   = SHUTTER_NONE;
            g_sOutCfg.ui8ShDown[iSlot] = SHUTTER_NONE;
            iCleared++;
            continue;
        }
        bUsed[ui8Up] = true;
        bUsed[ui8Dn] = true;
    }
    return(iCleared);
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
    int      iCleared;

    iCleared = ConfigShutterDedup();
    if(iCleared != 0)
    {
        PalLog("Config: cleared %d shutter(s) sharing a relay.\n", iCleared);
    }

    g_sOutCfg.ui32Magic = CFG_OUTCFG_MAGIC;
    g_sOutCfg.ui32Crc   = ConfigCRC32((const uint8_t *)&g_sOutCfg,
                                      sizeof(tOutputConfig) - sizeof(uint32_t));
    ui32Rc = PalStorageWrite((uint32_t *)&g_sOutCfg, CFG_OUTCFG_ADDR,
                           sizeof(tOutputConfig));
    if(ui32Rc != 0)
    {
        PalLog("EEPROM write failed (output config, 0x%x).\n", ui32Rc);
        return(false);
    }
    PalLog("Output config saved to EEPROM.\n");
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
    ui32Rc = PalStorageWrite((uint32_t *)&g_sRoomCfg, CFG_ROOMCFG_ADDR,
                           sizeof(tRoomConfig));
    if(ui32Rc != 0)
    {
        PalLog("EEPROM write failed (room config, 0x%x).\n", ui32Rc);
        return(false);
    }
    PalLog("Room config saved to EEPROM.\n");
    return(true);
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
    uint32_t ui32Rc;

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
    // The CRC is written LAST: if power is lost before it lands, the record fails
    // CRC on next boot and self-heals to blank names (rather than trusting stale
    // data).  All three writes are checked so a hardware failure is at least logged.
    ui32Rc  = PalStorageWrite(&g_sIONames.ui32Magic, CFG_IO_NAMES_ADDR, 4u);
    ui32Rc |= PalStorageWrite((uint32_t *)(uintptr_t)pcDst, ui32Addr, CFG_NAME_LEN);

    g_sIONames.ui32Crc = ConfigCRC32((const uint8_t *)&g_sIONames,
                                      sizeof(tIONames) - sizeof(uint32_t));
    ui32Rc |= PalStorageWrite(&g_sIONames.ui32Crc,
                  CFG_IO_NAMES_ADDR + sizeof(tIONames) - sizeof(uint32_t), 4u);
    if(ui32Rc != 0)
    {
        PalLog("EEPROM write failed (name %d, 0x%x).\n", iIdx, ui32Rc);
    }
}

bool
ConfigNamesSave(void)
{
    uint32_t ui32Rc;
    g_sIONames.ui32Magic = CFG_IO_NAMES_MAGIC;
    g_sIONames.ui32Crc   = ConfigCRC32((const uint8_t *)&g_sIONames,
                                        sizeof(tIONames) - sizeof(uint32_t));
    ui32Rc = PalStorageWrite((uint32_t *)&g_sIONames, CFG_IO_NAMES_ADDR,
                           sizeof(tIONames));
    return(ui32Rc == 0);
}
