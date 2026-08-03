//*****************************************************************************
//
// pal_storage.c (CC35x1) - PAL storage backed by NVOCMP/NVINTF on OSPI flash.
//
// config.c treats the store as a flat, byte-addressed blob (address 0 ..
// PalStorageSize()-1) and owns the record layout within it.  The TM4C backs that
// directly with on-chip EEPROM.  The CC35x1 has no such byte-addressable NVM;
// persistence is a key/value store (NVOCMP over external OSPI serial flash,
// reached through the NVINTF function table).  To keep config.c byte-for-byte
// identical across platforms, this backend holds the ENTIRE blob and RAM-shadows
// it:
//
//   Init   - load the NV API, init NV, read the chunks into the RAM shadow
//            (absent chunks => zero-filled => config.c sees no valid magic and
//            falls back to defaults, exactly like a blank EEPROM).
//   Read   - memcpy out of the RAM shadow (cannot fail, like the TM4C read).
//   Write  - update the RAM shadow, then rewrite the NV chunks that the written
//            byte range overlaps.
//
// The blob size matches the TM4C EEPROM (6 KB) so config.c's addressing is the
// same on both.
//
// CHUNKING (why the blob is not one NV item): NVOCMP on CC35xx caps a single
// item at NVOCMP_MAXLEN = 4095 bytes AND requires item+header to fit in one
// 4096-byte flash page.  A single 6144-byte writeItem is rejected with
// NVINTF_BADLENGTH (0x5) - the "EEPROM write failed 0x5" seen at boot, config
// never persisting.  So the blob is split into PAL_STORAGE_NCHUNKS items, keyed
// by subID = chunk index, each PAL_STORAGE_CHUNK (<= 4095, < page) bytes.  A
// write only rewrites the chunks its [addr,addr+len) range touches; because the
// RAM shadow always holds the full current state, rewriting any chunk from it is
// consistent (a chunk carrying other, previously-persisted records is rewritten
// with those bytes intact).  6144 fits easily in NVOCMP's multi-page region
// (default NVOCMP_NVPAGES = 6: 5 data + 1 compaction page).
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
// Per-NV-item chunk size.  Must stay <= NVOCMP_MAXLEN (4095) and leave header
// room within a 4096-byte flash page; 2048 divides 6144 evenly into 3 chunks,
// each comfortably one-item-per-page.
//
#define PAL_STORAGE_CHUNK   2048u
#define PAL_STORAGE_NCHUNKS ((PAL_STORAGE_SIZE + PAL_STORAGE_CHUNK - 1u) / \
                             PAL_STORAGE_CHUNK)

//
// The config blob lives under one itemID; subID selects the chunk (0..N-1).
//
#define PAL_STORAGE_ITEMID  1u

//
// Guard the per-item NVOCMP limit at compile time (see NVOCMP_MAXLEN = 0x0FFF).
//
_Static_assert(PAL_STORAGE_CHUNK <= 4095u,
               "PAL_STORAGE_CHUNK exceeds NVOCMP_MAXLEN (single-item cap)");

static uint8_t           g_pui8Shadow[PAL_STORAGE_SIZE];
static NVINTF_nvFuncts_t g_sNv;
static bool              g_bReady = false;

//
// Build the item identifier for a given chunk.
//
static NVINTF_itemID_t
PalStorageChunkId(uint32_t ui32Chunk)
{
    NVINTF_itemID_t sId;

    sId.systemID = NVINTF_SYSID_APP;
    sId.itemID   = PAL_STORAGE_ITEMID;
    sId.subID    = (uint16_t)ui32Chunk;
    return(sId);
}

//
// Byte length of a chunk (the final chunk may be short if SIZE is not a whole
// multiple of CHUNK; with 6144/2048 all chunks are full, but stay general).
//
static uint16_t
PalStorageChunkLen(uint32_t ui32Chunk)
{
    uint32_t ui32Off = ui32Chunk * PAL_STORAGE_CHUNK;
    uint32_t ui32Rem = PAL_STORAGE_SIZE - ui32Off;

    return((uint16_t)((ui32Rem < PAL_STORAGE_CHUNK) ? ui32Rem
                                                    : PAL_STORAGE_CHUNK));
}

//
// Persist a single chunk from the RAM shadow (delete-then-write keeps one active
// copy, matching the SDK's own NV usage).  Returns the writeItem status.
//
static uint8_t
PalStorageWriteChunk(uint32_t ui32Chunk)
{
    NVINTF_itemID_t sId    = PalStorageChunkId(ui32Chunk);
    uint32_t        ui32Off = ui32Chunk * PAL_STORAGE_CHUNK;

    if(g_sNv.deleteItem != NULL)
    {
        g_sNv.deleteItem(sId);
    }

    return(g_sNv.writeItem(sId, PalStorageChunkLen(ui32Chunk),
                           &g_pui8Shadow[ui32Off]));
}

//*****************************************************************************
//
// Load the NV API, initialise NV, and prime the RAM shadow from flash.
//
//*****************************************************************************
bool
PalStorageInit(void)
{
    uint32_t ui32Chunk;

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
    // Pull each persisted chunk into the shadow.  A non-zero return means the
    // chunk does not exist yet (first boot / never written) - present those
    // bytes as zero so config.c applies its compiled-in defaults, matching an
    // erased TM4C EEPROM.
    //
    for(ui32Chunk = 0; ui32Chunk < PAL_STORAGE_NCHUNKS; ui32Chunk++)
    {
        uint32_t ui32Off = ui32Chunk * PAL_STORAGE_CHUNK;
        uint16_t ui16Len = PalStorageChunkLen(ui32Chunk);

        if(g_sNv.readItem(PalStorageChunkId(ui32Chunk), 0, ui16Len,
                          &g_pui8Shadow[ui32Off]) != 0)
        {
            memset(&g_pui8Shadow[ui32Off], 0, ui16Len);
        }
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
// Update the shadow, then persist the chunks the written range overlaps.  Only
// the touched chunks are rewritten; each carries whatever the shadow currently
// holds for its span, so records sharing a chunk are preserved.
//
//*****************************************************************************
uint32_t
PalStorageWrite(const void *pvBuf, uint32_t ui32Addr, uint32_t ui32Len)
{
    uint32_t ui32First;
    uint32_t ui32Last;
    uint32_t ui32Chunk;
    uint32_t ui32Result = 0;

    if(!g_bReady || (ui32Len == 0) || ((ui32Addr + ui32Len) > PAL_STORAGE_SIZE))
    {
        return(1);
    }

    memcpy(&g_pui8Shadow[ui32Addr], pvBuf, ui32Len);

    //
    // Chunks spanned by [ui32Addr, ui32Addr + ui32Len).
    //
    ui32First = ui32Addr / PAL_STORAGE_CHUNK;
    ui32Last  = (ui32Addr + ui32Len - 1u) / PAL_STORAGE_CHUNK;

    for(ui32Chunk = ui32First; ui32Chunk <= ui32Last; ui32Chunk++)
    {
        uint8_t ui8Status = PalStorageWriteChunk(ui32Chunk);

        //
        // Surface the first failure but still attempt the remaining chunks so
        // the shadow and flash do not diverge more than necessary.
        //
        if((ui8Status != 0) && (ui32Result == 0))
        {
            ui32Result = ui8Status;
        }
    }

    return(ui32Result);
}
