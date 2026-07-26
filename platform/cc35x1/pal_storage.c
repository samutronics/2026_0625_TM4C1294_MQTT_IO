//*****************************************************************************
//
// pal_storage.c (CC35x1) - PAL storage backed by NVOCMP/NVINTF on OSPI flash.
//
// config.c treats the store as a flat, byte-addressed blob (address 0 ..
// PalStorageSize()-1) and owns the record layout within it.  The TM4C backs that
// directly with on-chip EEPROM.  The CC35x1 has no such byte-addressable NVM;
// persistence is a key/value store (NVOCMP over external OSPI serial flash,
// reached through the NVINTF function table).  To keep config.c byte-for-byte
// identical across platforms, this backend holds the ENTIRE blob in one NV item
// (systemID = NVINTF_SYSID_APP) and RAM-shadows it:
//
//   Init   - load the NV API, init NV, read the item into the RAM shadow
//            (absent item => zero-filled shadow => config.c sees no valid magic
//            and falls back to defaults, exactly like a blank EEPROM).
//   Read   - memcpy out of the RAM shadow (cannot fail, like the TM4C read).
//   Write  - update the RAM shadow, then rewrite the whole item.
//
// The blob size matches the TM4C EEPROM (6 KB) so config.c's addressing is the
// same on both.  Config writes are infrequent, so rewriting the full item each
// time (NVOCMP handles page compaction/wear) is acceptable.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "ti/common/nv/nvintf.h"
#include "ti/common/nv/nvocmp.h"
#include "pal_storage.h"

//
// Blob size - kept equal to the TM4C1294 on-chip EEPROM (6 KB) so config.c's
// record offsets are platform-independent.
//
#define PAL_STORAGE_SIZE    6144u

//
// The single NV item that holds the whole config blob.
//
#define PAL_STORAGE_ITEMID  1u
#define PAL_STORAGE_SUBID   0u

static uint8_t           g_pui8Shadow[PAL_STORAGE_SIZE];
static NVINTF_nvFuncts_t g_sNv;
static bool              g_bReady = false;

//
// Build the item identifier for the config blob.
//
static NVINTF_itemID_t
PalStorageItemId(void)
{
    NVINTF_itemID_t sId;

    sId.systemID = NVINTF_SYSID_APP;
    sId.itemID   = PAL_STORAGE_ITEMID;
    sId.subID    = PAL_STORAGE_SUBID;
    return(sId);
}

//*****************************************************************************
//
// Load the NV API, initialise NV, and prime the RAM shadow from flash.
//
//*****************************************************************************
bool
PalStorageInit(void)
{
    NVINTF_itemID_t sId = PalStorageItemId();

    NVOCMP_loadApiPtrs(&g_sNv);

    if((g_sNv.initNV == NULL) || (g_sNv.readItem == NULL) ||
       (g_sNv.writeItem == NULL))
    {
        return(false);
    }

    if(g_sNv.initNV(NULL) != 0)
    {
        return(false);
    }

    //
    // Pull the persisted blob into the shadow.  A non-zero return means the
    // item does not exist yet (first boot) - present a blank store so config.c
    // applies its compiled-in defaults, matching an erased TM4C EEPROM.
    //
    if(g_sNv.readItem(sId, 0, (uint16_t)PAL_STORAGE_SIZE, g_pui8Shadow) != 0)
    {
        memset(g_pui8Shadow, 0, PAL_STORAGE_SIZE);
    }

    g_bReady = true;
    return(true);
}

uint32_t
PalStorageSize(void)
{
    return(PAL_STORAGE_SIZE);
}

//*****************************************************************************
//
// Serve reads straight from the RAM shadow.
//
//*****************************************************************************
uint32_t
PalStorageRead(void *pvBuf, uint32_t ui32Addr, uint32_t ui32Len)
{
    if(!g_bReady || ((ui32Addr + ui32Len) > PAL_STORAGE_SIZE))
    {
        return(1);
    }

    memcpy(pvBuf, &g_pui8Shadow[ui32Addr], ui32Len);
    return(0);
}

//*****************************************************************************
//
// Update the shadow, then persist the whole blob as one NV item.  deleteItem
// keeps a single active copy (matches the SDK's own NV usage pattern); the
// create happens inside writeItem when the item is absent.
//
//*****************************************************************************
uint32_t
PalStorageWrite(const void *pvBuf, uint32_t ui32Addr, uint32_t ui32Len)
{
    NVINTF_itemID_t sId = PalStorageItemId();

    if(!g_bReady || ((ui32Addr + ui32Len) > PAL_STORAGE_SIZE))
    {
        return(1);
    }

    memcpy(&g_pui8Shadow[ui32Addr], pvBuf, ui32Len);

    if(g_sNv.deleteItem != NULL)
    {
        g_sNv.deleteItem(sId);
    }

    return((uint32_t)g_sNv.writeItem(sId, (uint16_t)PAL_STORAGE_SIZE,
                                     g_pui8Shadow));
}
