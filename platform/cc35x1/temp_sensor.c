//*****************************************************************************
//
// temp_sensor.c (CC35x1) - read the on-board TMP1075 over a bit-banged I2C bus.
//
// The LP-EM-CC35X1 carries a TMP1075NDRLR on I2C1 (SDA = GPIO10, SCL = GPIO11,
// address 0x48), with onboard pull-ups and the sensor connected by default
// (jumpers J15/J18).  Rather than add a SysConfig I2C instance (the SysConfig
// editor is unavailable in this install), we drive a software I2C master on the
// two GPIOs.  As with buttons.c, no SysConfig instance is needed: on CC35xx the
// TI Drivers GPIO index IS the physical GPIO number and gpioPinConfigs[] reserves
// a slot for every pin, so GPIO_setConfig()/GPIO_read() drive any in-range pin at
// runtime.  WFF3 GPIO has no true open-drain mode, so open-drain is emulated the
// classic way: to release a line (logic high) the pin is switched to input and
// the board's I2C pull-ups (plus an internal pull-up) drive it high; to pull it
// low the pin is switched to output-low.  Reading SDA is a plain input read.
//
// The TMP1075 powers up in continuous-conversion mode with the pointer at the
// temperature register (0x00).  A read returns two bytes: a left-justified 12-bit
// two's-complement value in bits [15:4]; temperature = value * 0.0625 C.
//
//*****************************************************************************

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ti/drivers/GPIO.h>

#include "temp_sensor.h"

//
// Physical CC35xx GPIO numbers of the I2C1 lines (== TI Drivers GPIO index).
//
#define I2C_SDA_PIN     10u
#define I2C_SCL_PIN     11u

//
// TMP1075 7-bit address and temperature-register pointer.
//
#define TMP1075_ADDR    0x48u
#define TMP1075_REG_T   0x00u

//
// Half-bit-period delay.  A short volatile busy-wait keeps the clock well under
// the TMP1075's 400 kHz max (bus speed is irrelevant for a 5 s poll); slow and
// safe over the board's I2C pull-ups.
//
#define I2C_DELAY_LOOPS 120u

static bool    g_bValid;            // a good reading has been taken since boot
static int32_t g_i32CentiC;         // last good reading, centi-degrees Celsius

static void
I2cDelay(void)
{
    volatile uint32_t ui32 = I2C_DELAY_LOOPS;
    while(ui32--)
    {
    }
}

//
// Emulated open-drain line primitives: "High" releases the line (switch to input;
// the I2C pull-ups drive it high), "Low" drives it low (output-low).  Reading SDA
// is a plain input read (SDA is left released by the read/ACK helpers).
//
static void SdaHigh(void) { GPIO_setConfig(I2C_SDA_PIN, GPIO_CFG_IN_PU); }
static void SdaLow(void)  { GPIO_setConfig(I2C_SDA_PIN,
                                           GPIO_CFG_OUTPUT | GPIO_CFG_OUT_LOW); }
static void SclHigh(void) { GPIO_setConfig(I2C_SCL_PIN, GPIO_CFG_IN_PU); }
static void SclLow(void)  { GPIO_setConfig(I2C_SCL_PIN,
                                           GPIO_CFG_OUTPUT | GPIO_CFG_OUT_LOW); }
static int  SdaRead(void) { return((int)GPIO_read(I2C_SDA_PIN)); }

static void
I2cStart(void)
{
    SdaHigh(); SclHigh(); I2cDelay();
    SdaLow();  I2cDelay();
    SclLow();  I2cDelay();
}

static void
I2cStop(void)
{
    SdaLow();  I2cDelay();
    SclHigh(); I2cDelay();
    SdaHigh(); I2cDelay();
}

//
// Clock out one bit (MSB-first framing is done by the caller).
//
static void
I2cWriteBit(int iBit)
{
    if(iBit) { SdaHigh(); } else { SdaLow(); }
    I2cDelay();
    SclHigh(); I2cDelay();
    SclLow();  I2cDelay();
}

//
// Clock in one bit (SDA released so the slave can drive it).
//
static int
I2cReadBit(void)
{
    int iBit;

    SdaHigh();               // release SDA for the slave
    I2cDelay();
    SclHigh(); I2cDelay();
    iBit = SdaRead();
    SclLow();  I2cDelay();
    return(iBit);
}

//
// Write a byte; return 0 if the slave ACKed (pulled SDA low on the 9th clock).
//
static int
I2cWriteByte(uint8_t ui8Val)
{
    int i, iAck;

    for(i = 0; i < 8; i++)
    {
        I2cWriteBit((ui8Val & 0x80u) != 0);
        ui8Val = (uint8_t)(ui8Val << 1);
    }
    iAck = I2cReadBit();     // 0 = ACK, 1 = NACK
    return(iAck);
}

//
// Read a byte; drive ACK (bAck true -> pull SDA low) or NACK after the 8th bit.
//
static uint8_t
I2cReadByte(bool bAck)
{
    int     i;
    uint8_t ui8Val = 0;

    for(i = 0; i < 8; i++)
    {
        ui8Val = (uint8_t)((ui8Val << 1) | (I2cReadBit() & 1));
    }
    if(bAck) { SdaLow(); } else { SdaHigh(); }
    I2cDelay();
    SclHigh(); I2cDelay();
    SclLow();  I2cDelay();
    SdaHigh();               // release the bus
    return(ui8Val);
}

void
TempSensorInit(void)
{
    //
    // GPIO_init() readies the module (idempotent).  Release both lines (inputs
    // with pull-ups) so the bus idles high.
    //
    GPIO_init();
    SdaHigh();
    SclHigh();
}

bool
TempSensorPoll(void)
{
    uint8_t ui8Msb, ui8Lsb;
    int16_t i16Reg;
    int32_t i32Raw;

    //
    // Point the TMP1075 at its temperature register (defaults there, but be
    // explicit), then repeated-START and read the two data bytes.
    //
    I2cStart();
    if(I2cWriteByte((uint8_t)(TMP1075_ADDR << 1)) != 0)     // address + write
    {
        I2cStop();
        return(false);
    }
    if(I2cWriteByte(TMP1075_REG_T) != 0)
    {
        I2cStop();
        return(false);
    }

    I2cStart();                                             // repeated START
    if(I2cWriteByte((uint8_t)((TMP1075_ADDR << 1) | 1u)) != 0)  // address + read
    {
        I2cStop();
        return(false);
    }
    ui8Msb = I2cReadByte(true);    // ACK -> more to come
    ui8Lsb = I2cReadByte(false);   // NACK -> last byte
    I2cStop();

    //
    // Left-justified 12-bit two's-complement value in bits [15:4]; an arithmetic
    // shift on the signed 16-bit register sign-extends it.  centi-C = raw * 6.25.
    //
    i16Reg = (int16_t)(((uint16_t)ui8Msb << 8) | ui8Lsb);
    i32Raw = (int32_t)(i16Reg >> 4);
    g_i32CentiC = (i32Raw * 25) / 4;
    g_bValid    = true;
    return(true);
}

bool
TempSensorGet(int32_t *pi32CentiC)
{
    if(!g_bValid)
    {
        return(false);
    }
    if(pi32CentiC != NULL)
    {
        *pi32CentiC = g_i32CentiC;
    }
    return(true);
}
