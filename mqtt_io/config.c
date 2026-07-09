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

//
// Marker identifying a valid configuration record ("MQT1").
//
#define CFG_MAGIC               0x4D515431

//
// EEPROM byte address at which the record is stored.
//
#define CFG_EEPROM_ADDR         0

//
// The live, in-RAM copy of the configuration.
//
static tMQTTConfig g_sConfig;

//*****************************************************************************
//
// Compute a CRC32 (IEEE 802.3, reflected) over a buffer.  Used to validate the
// stored record.  A small table-less implementation is sufficient here.
//
//*****************************************************************************
static uint32_t
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
    if((ui8Devices == 0) || (ui8Devices > CFG_DIN_MAX_DEVICES))
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
    if((ui8Devices == 0) || (ui8Devices > CFG_RELAY_MAX_DEVICES))
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
