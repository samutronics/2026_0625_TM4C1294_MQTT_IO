//*****************************************************************************
//
// ota.h - Over-the-air firmware update via HTTP chunked upload.
//
// Flash layout (1 MB total):
//   0x00000 - 0x7FFFF  App slot       (512 KB)
//   0x80000 - 0x83FFF  OTA header     (16 KB sector, first 16 bytes used)
//   0x84000 - 0xFFFFF  OTA image      (496 KB, raw firmware binary)
//
// Upload flow:
//   Browser GET /fwchunk.cgi?seq=N&last=0&data=HEXHEX...
//     seq 0: erases header + first image page, starts writing at OTA_IMAGE_ADDR
//     seq N: writes next hex-decoded chunk to flash
//     last=1: validates CRC32, writes header, sets EEPROM flag, resets
//
// Apply flow (runs from SRAM via .TI.ramfunc on next boot):
//   OtaCheckAndApply() in main() -> validates -> OtaApply(size) -> erase app
//   slot -> program from OTA_IMAGE_ADDR -> reset
//
//*****************************************************************************

#ifndef __OTA_H__
#define __OTA_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Flash addresses.
//
#define OTA_HEADER_ADDR     0x00080000u     // 16-byte metadata sector
#define OTA_IMAGE_ADDR      0x00084000u     // raw firmware binary starts here
#define OTA_APP_ADDR        0x00000000u     // destination app slot start
#define OTA_APP_SIZE        0x00080000u     // 512 KB app slot to erase/program
#define OTA_IMAGE_MAX_SIZE  (0x00100000u - OTA_IMAGE_ADDR)  // 496 KB

//
// OTA header stored at OTA_HEADER_ADDR (exactly one 16-byte record).
//
#define OTA_HDR_MAGIC       0x4F544148u     // "OTAH"

typedef struct
{
    uint32_t ui32Magic;     // OTA_HDR_MAGIC when valid
    uint32_t ui32Size;      // firmware image size in bytes
    uint32_t ui32CRC32;     // CRC32 of the image
    uint32_t ui32Rsvd;
}
tOTAHeader;

//
// EEPROM OTA-pending flag, stored at CFG_OTA_EEPROM_ADDR (two 4-byte words).
// Written after a successful upload; cleared before OtaApply() runs.
//
#define CFG_OTA_EEPROM_ADDR     1252u
#define OTA_EEPROM_MAGIC        0x4F544150u  // "OTAP"

//
// Check whether an uploaded image is pending (EEPROM flag set).
//
bool OtaIsPending(void);

//
// Persist "update ready" to EEPROM.  Called after the last chunk is received
// and CRC-validated.
//
void OtaSetPending(uint32_t ui32Size);

//
// Clear the EEPROM flag.  Called before OtaApply() so a power-loss during
// programming doesn't loop forever.
//
void OtaClearPending(void);

//
// Called early in main(): if an update is pending and the staged image passes
// CRC validation, this function calls OtaApply() and never returns.  If the
// CRC fails it clears the flag and returns normally.
//
void OtaCheckAndApply(void);

//
// Erase the app slot and reprogram it from OTA_IMAGE_ADDR, then reset.
// THIS FUNCTION RUNS FROM SRAM (section .TI.ramfunc) and uses ROM flash
// drivers — safe to call while executing from the flash it will overwrite.
// Does not return.
//
void OtaApply(uint32_t ui32Size) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif // __OTA_H__
