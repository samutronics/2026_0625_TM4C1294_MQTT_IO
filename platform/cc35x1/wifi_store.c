//*****************************************************************************
//
// wifi_store.c (CC35x1) - persistent Wi-Fi STA credentials in the PAL store.
//
// Mirrors the magic + CRC32 record pattern config.c uses, but for a single small
// credentials record living at the reserved address CFG_WIFI_EEPROM_ADDR (see
// config.h).  Reuses ConfigCRC32() and the PalStorage byte-addressed backend, so
// the credentials persist through the same NVOCMP flash blob and survive OTA.
//
//*****************************************************************************

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pal_log.h"
#include "pal_storage.h"
#include "config.h"        // CFG_WIFI_EEPROM_ADDR, CFG_WIFI_MAGIC, ConfigCRC32, tRoomConfig
#include "wifi_store.h"

//
// On-store record.  PalStorage requires 4-byte-aligned addresses and lengths, so
// the buffers are padded to keep sizeof(tWifiRecord) a multiple of 4.  ssid and
// pass are always NUL-terminated (the buffers are larger than the exported max
// lengths and the record is zeroed before every save).
//
#define WIFI_SSID_BUF   36u    // WIFI_SSID_MAX (32) + NUL, padded to 4
#define WIFI_PASS_BUF   64u    // WIFI_PASS_MAX (63) + NUL

typedef struct
{
    uint32_t ui32Magic;
    char     pcSsid[WIFI_SSID_BUF];
    char     pcPass[WIFI_PASS_BUF];
    uint32_t ui32Crc;
}
tWifiRecord;   // 4 + 36 + 64 + 4 = 108 B

//
// Address table for multiple credential slots.  Slot 1 is the backup network.
//
static const uint32_t aui32SlotAddr[WIFI_STORE_SLOTS] = {
    CFG_WIFI_EEPROM_ADDR,
    CFG_WIFI2_EEPROM_ADDR
};

//
// Compile-time guards: the records must sit after the room config, fit inside the
// store, and stay 4-byte aligned.  (The store size mirrors PalStorageSize() = the
// TM4C EEPROM size; kept literal here because static_assert needs a constant.)
//
_Static_assert(CFG_WIFI_EEPROM_ADDR >= CFG_ROOMCFG_ADDR + sizeof(tRoomConfig),
               "WIFI slot 0 overlaps the ROOM record");
_Static_assert(CFG_WIFI_EEPROM_ADDR + sizeof(tWifiRecord) <= 6144u,
               "WIFI slot 0 overflows the persistent store");
_Static_assert(CFG_WIFI2_EEPROM_ADDR >= CFG_WIFI_EEPROM_ADDR + sizeof(tWifiRecord),
               "WIFI slot 1 overlaps slot 0");
_Static_assert(CFG_WIFI2_EEPROM_ADDR + sizeof(tWifiRecord) <= 6144u,
               "WIFI slot 1 overflows the persistent store");
_Static_assert((sizeof(tWifiRecord) % 4u) == 0u,
               "tWifiRecord must be 4-byte aligned for PalStorageWrite");
_Static_assert(WIFI_SSID_MAX < WIFI_SSID_BUF, "SSID buffer too small");
_Static_assert(WIFI_PASS_MAX < WIFI_PASS_BUF, "passphrase buffer too small");

//*****************************************************************************
//
// WifiStoreLoad - read and validate the stored credentials from the given slot.
//
//*****************************************************************************
bool
WifiStoreLoad(int iSlot, char *pcSsid, char *pcPass)
{
    tWifiRecord sRec;
    uint32_t    ui32Crc;

    if(iSlot < 0 || iSlot >= WIFI_STORE_SLOTS)
    {
        if(pcSsid != NULL) { pcSsid[0] = '\0'; }
        if(pcPass != NULL) { pcPass[0] = '\0'; }
        return(false);
    }

    if(pcSsid != NULL) { pcSsid[0] = '\0'; }
    if(pcPass != NULL) { pcPass[0] = '\0'; }

    if(PalStorageRead(&sRec, aui32SlotAddr[iSlot], sizeof(sRec)) != 0)
    {
        return(false);
    }

    ui32Crc = ConfigCRC32((const uint8_t *)&sRec,
                          sizeof(sRec) - sizeof(uint32_t));
    if((sRec.ui32Magic != CFG_WIFI_MAGIC) || (sRec.ui32Crc != ui32Crc) ||
       (sRec.pcSsid[0] == '\0'))
    {
        return(false);
    }

    //
    // Copy out, bounded to the exported max lengths so a caller buffer of
    // WIFI_*_MAX+1 is never overrun even if the stored strings were somehow
    // longer than expected.
    //
    if(pcSsid != NULL)
    {
        strncpy(pcSsid, sRec.pcSsid, WIFI_SSID_MAX);
        pcSsid[WIFI_SSID_MAX] = '\0';
    }
    if(pcPass != NULL)
    {
        strncpy(pcPass, sRec.pcPass, WIFI_PASS_MAX);
        pcPass[WIFI_PASS_MAX] = '\0';
    }
    return(true);
}

//*****************************************************************************
//
// WifiStoreSave - stamp, CRC and persist the given credentials to the given slot.
//
//*****************************************************************************
bool
WifiStoreSave(int iSlot, const char *pcSsid, const char *pcPass)
{
    tWifiRecord sRec;
    uint32_t    ui32Rc;

    if(iSlot < 0 || iSlot >= WIFI_STORE_SLOTS)
    {
        return(false);
    }

    memset(&sRec, 0, sizeof(sRec));
    sRec.ui32Magic = CFG_WIFI_MAGIC;
    if(pcSsid != NULL) { strncpy(sRec.pcSsid, pcSsid, WIFI_SSID_MAX); }
    if(pcPass != NULL) { strncpy(sRec.pcPass, pcPass, WIFI_PASS_MAX); }

    sRec.ui32Crc = ConfigCRC32((const uint8_t *)&sRec,
                               sizeof(sRec) - sizeof(uint32_t));

    ui32Rc = PalStorageWrite(&sRec, aui32SlotAddr[iSlot], sizeof(sRec));
    if(ui32Rc != 0)
    {
        PalLog("wifi: credentials save failed (0x%x)\n", ui32Rc);
        return(false);
    }

    PalLog("wifi: credentials saved for SSID '%s'\n", sRec.pcSsid);
    return(true);
}

//*****************************************************************************
//
// WifiStoreClear - invalidate the record in the given slot by zeroing its magic word.
//
//*****************************************************************************
void
WifiStoreClear(int iSlot)
{
    uint32_t ui32Zero = 0u;

    if(iSlot < 0 || iSlot >= WIFI_STORE_SLOTS)
    {
        return;
    }

    if(PalStorageWrite(&ui32Zero, aui32SlotAddr[iSlot], sizeof(ui32Zero)) != 0)
    {
        PalLog("wifi: credentials clear failed\n");
    }
    else
    {
        PalLog("wifi: credentials cleared\n");
    }
}
