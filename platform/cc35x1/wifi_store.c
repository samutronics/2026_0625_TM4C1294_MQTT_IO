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
// Compile-time guards: the record must sit after the room config, fit inside the
// store, and stay 4-byte aligned.  (The store size mirrors PalStorageSize() = the
// TM4C EEPROM size; kept literal here because static_assert needs a constant.)
//
_Static_assert(CFG_WIFI_EEPROM_ADDR >= CFG_ROOMCFG_ADDR + sizeof(tRoomConfig),
               "WIFI record overlaps the ROOM record");
_Static_assert(CFG_WIFI_EEPROM_ADDR + sizeof(tWifiRecord) <= 6144u,
               "WIFI record overflows the persistent store");
_Static_assert((sizeof(tWifiRecord) % 4u) == 0u,
               "tWifiRecord must be 4-byte aligned for PalStorageWrite");
_Static_assert(WIFI_SSID_MAX < WIFI_SSID_BUF, "SSID buffer too small");
_Static_assert(WIFI_PASS_MAX < WIFI_PASS_BUF, "passphrase buffer too small");

//*****************************************************************************
//
// WifiStoreLoad - read and validate the stored credentials.
//
//*****************************************************************************
bool
WifiStoreLoad(char *pcSsid, char *pcPass)
{
    tWifiRecord sRec;
    uint32_t    ui32Crc;

    if(pcSsid != NULL) { pcSsid[0] = '\0'; }
    if(pcPass != NULL) { pcPass[0] = '\0'; }

    if(PalStorageRead(&sRec, CFG_WIFI_EEPROM_ADDR, sizeof(sRec)) != 0)
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
// WifiStoreSave - stamp, CRC and persist the given credentials.
//
//*****************************************************************************
bool
WifiStoreSave(const char *pcSsid, const char *pcPass)
{
    tWifiRecord sRec;
    uint32_t    ui32Rc;

    memset(&sRec, 0, sizeof(sRec));
    sRec.ui32Magic = CFG_WIFI_MAGIC;
    if(pcSsid != NULL) { strncpy(sRec.pcSsid, pcSsid, WIFI_SSID_MAX); }
    if(pcPass != NULL) { strncpy(sRec.pcPass, pcPass, WIFI_PASS_MAX); }

    sRec.ui32Crc = ConfigCRC32((const uint8_t *)&sRec,
                               sizeof(sRec) - sizeof(uint32_t));

    ui32Rc = PalStorageWrite(&sRec, CFG_WIFI_EEPROM_ADDR, sizeof(sRec));
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
// WifiStoreClear - invalidate the record by zeroing its magic word.
//
//*****************************************************************************
void
WifiStoreClear(void)
{
    uint32_t ui32Zero = 0u;

    if(PalStorageWrite(&ui32Zero, CFG_WIFI_EEPROM_ADDR, sizeof(ui32Zero)) != 0)
    {
        PalLog("wifi: credentials clear failed\n");
    }
    else
    {
        PalLog("wifi: credentials cleared\n");
    }
}

//*****************************************************************************
//
// WifiStoreHas - true when a valid credentials record is stored.
//
//*****************************************************************************
bool
WifiStoreHas(void)
{
    char pcSsid[WIFI_SSID_MAX + 1];

    return(WifiStoreLoad(pcSsid, NULL));
}
