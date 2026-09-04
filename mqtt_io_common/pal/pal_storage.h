//*****************************************************************************
//
// pal_storage.h - Platform abstraction for the small, byte-addressed persistent
//                 store that holds the device configuration records.
//
// The store is a flat block of NVM bytes (address 0 .. PalStorageSize()-1).
// config.c owns the record layout within it; this interface only moves bytes.
//
//   Platform          Backing store
//   ----------------  ---------------------------------------------------------
//   TM4C1294          on-chip EEPROM (6 KB) via the TivaWare EEPROM block API
//   CC35x1 (future)   one NVOCMP/NVINTF item on external OSPI flash, RAM-shadowed
//
// Addresses and lengths must be 4-byte aligned (a hard requirement of the TM4C
// EEPROM block API; harmless on other backends).
//
//*****************************************************************************

#ifndef __PAL_STORAGE_H__
#define __PAL_STORAGE_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Ready the persistent store.  Returns true on success; on failure the caller
// should fall back to compiled-in defaults and treat the store as unavailable
// (no persistence).  Must be called once before any read/write.
//
bool PalStorageInit(void);

//
// Total size of the store in bytes.
//
uint32_t PalStorageSize(void);

//
// Read ui32Len bytes starting at ui32Addr into pvBuf.  Returns 0 on success,
// non-zero on error.  (The TM4C backend cannot fail a read and always returns 0.)
//
uint32_t PalStorageRead(void *pvBuf, uint32_t ui32Addr, uint32_t ui32Len);

//
// Write ui32Len bytes from pvBuf to ui32Addr.  Returns 0 on success, non-zero
// (a platform-specific error code) on failure.
//
uint32_t PalStorageWrite(const void *pvBuf, uint32_t ui32Addr, uint32_t ui32Len);

#ifdef __cplusplus
}
#endif

#endif // __PAL_STORAGE_H__
