//*****************************************************************************
//
// ota.c - OTA firmware update: boot-time check and SRAM-resident copier.
//
// OtaApply() is placed in section .TI.ramfunc so the linker copies it to SRAM
// at startup.  All flash operations inside it call ROM_ variants which execute
// from the internal boot ROM at 0x01000000 — never the application flash —
// making it safe to erase the entire app slot while the function is running.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_nvic.h"
#include "inc/hw_types.h"
#include "driverlib/flash.h"
#include "driverlib/interrupt.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "utils/uartstdio.h"
#include "config.h"
#include "ota.h"

//*****************************************************************************
//
// Thin wrappers delegating to config.c EEPROM helpers.
//
//*****************************************************************************
bool
OtaIsPending(void)
{
    return ConfigOtaIsPending();
}

void
OtaSetPending(uint32_t ui32Size)
{
    ConfigOtaSetPending(ui32Size);
}

void
OtaClearPending(void)
{
    ConfigOtaClearPending();
}

//*****************************************************************************
//
// OtaCheckAndApply — call early in main() (after EEPROM / SYSCTL init,
// before network init).  Validates the staged image; if good, calls OtaApply()
// which never returns.  If bad, clears the flag and returns normally so the
// application continues with the existing firmware.
//
//*****************************************************************************
void
OtaCheckAndApply(void)
{
    const tOTAHeader *psHdr;
    uint32_t ui32Crc;

    if(!OtaIsPending())
    {
        return;
    }

    UARTprintf("OTA: update pending, validating...\n");

    psHdr = (const tOTAHeader *)OTA_HEADER_ADDR;

    //
    // Validate the STAGED image before touching the app slot.  A failure here
    // means staging is unusable, so it is safe to clear the flag and boot the
    // existing (untouched) firmware.
    //
    if(psHdr->ui32Magic != OTA_HDR_MAGIC)
    {
        UARTprintf("OTA: bad header magic, aborting.\n");
        OtaClearPending();
        return;
    }

    if(psHdr->ui32Size == 0 || psHdr->ui32Size > OTA_IMAGE_MAX_SIZE)
    {
        UARTprintf("OTA: image size %u out of range, aborting.\n",
                   psHdr->ui32Size);
        OtaClearPending();
        return;
    }

    //
    // CRC32 the staged image.  This runs from flash (the app flash is still
    // intact at this point — OtaApply() hasn't been called yet).
    //
    ui32Crc = ConfigCRC32((const uint8_t *)OTA_IMAGE_ADDR, psHdr->ui32Size);

    if(ui32Crc != psHdr->ui32CRC32)
    {
        UARTprintf("OTA: staging CRC mismatch (got 0x%08x, expected 0x%08x).\n",
                   ui32Crc, psHdr->ui32CRC32);
        OtaClearPending();
        return;
    }

    //
    // Staging is valid.  Is the app slot ALREADY this exact image?  That is the
    // case when a previous OtaApply() finished but power was lost before this
    // boot could clear the flag.  Finish cleanly without re-erasing.
    //
    if(ConfigCRC32((const uint8_t *)OTA_APP_ADDR, psHdr->ui32Size) ==
       psHdr->ui32CRC32)
    {
        UARTprintf("OTA: app already matches staged image; clearing flag.\n");
        OtaClearPending();
        return;
    }

    UARTprintf("OTA: CRC OK, size=%u bytes.  Applying...\n", psHdr->ui32Size);

    //
    // Hand off to the SRAM-resident copier — does not return.
    //
    // CRITICAL: the pending flag stays SET across the erase/program.  If power
    // is lost while OtaApply() is writing, the next boot re-validates staging
    // (still intact) and — because the app slot will NOT yet match — re-applies.
    // Once the app slot CRC matches staging, the branch above clears the flag.
    // This turns a mid-write power loss into an automatic retry instead of a
    // bricked device.  (Residual single-bank window: a power loss while page 0
    // itself is mid-erase/program can still prevent boot — unavoidable without a
    // separate bootloader sector.)
    //
    OtaApply(psHdr->ui32Size);
}

//*****************************************************************************
//
// OtaApply — erase the app slot and reprogram from OTA_IMAGE_ADDR, then reset.
//
// MUST run from SRAM: the function is placed in .TI.ramfunc and is copied to
// SRAM by the C startup code via the BINIT table before main() is called.
// All TI DriverLib calls are ROM_* variants — they execute from the boot ROM
// at 0x01000000, not from flash.
//
//*****************************************************************************
__attribute__((section(".TI.ramfunc"), noinline, noreturn))
void
OtaApply(uint32_t ui32Size)
{
    uint32_t ui32Addr;
    uint32_t ui32SizeAligned;

    //
    // Mask all interrupts — no ISR should run while flash is being erased.
    //
    ROM_IntMasterDisable();

    //
    // Erase the entire app slot (0x00000 to 0x7FFFF) one 16 KB page at a time.
    //
    for(ui32Addr = OTA_APP_ADDR;
        ui32Addr < OTA_APP_ADDR + OTA_APP_SIZE;
        ui32Addr += 0x4000u)
    {
        ROM_FlashErase(ui32Addr);
    }

    //
    // Program the new image from staging to the app slot.
    // ROM_FlashProgram requires count to be a multiple of 4 bytes.
    //
    ui32SizeAligned = (ui32Size + 3u) & ~3u;
    ROM_FlashProgram((uint32_t *)OTA_IMAGE_ADDR, OTA_APP_ADDR, ui32SizeAligned);

    //
    // Reset via NVIC AIRCR — pure register write, no flash or RAM dependency.
    //
    HWREG(NVIC_APINT) = NVIC_APINT_VECTKEY | NVIC_APINT_SYSRESETREQ;

    //
    // Should not reach here, but satisfy the compiler.
    //
    while(1)
    {
    }
}
