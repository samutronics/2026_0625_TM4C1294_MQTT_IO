//*****************************************************************************
//
// temp_sensor.h (CC35x1) - LP-EM-CC35X1 on-board TMP1075 temperature sensor.
//
// The TMP1075 hangs off I2C1 (SDA = GPIO10, SCL = GPIO11, 7-bit address 0x48)
// with the sensors enabled by default (jumpers J15/J18).  We read it with a
// software (bit-banged) I2C master on those two GPIOs - no SysConfig I2C instance
// required - so it works with the stock pin configuration.  See temp_sensor.c.
//
// Temperatures are reported in centi-degrees Celsius (e.g. 2344 = 23.44 C) as a
// signed integer; the TMP1075's resolution is 0.0625 C.
//
//*****************************************************************************

#ifndef __TEMP_SENSOR_H__
#define __TEMP_SENSOR_H__

#include <stdbool.h>
#include <stdint.h>

//
// Configure the two I2C GPIOs (idempotent).  Call once at startup.
//
void TempSensorInit(void);

//
// Read the sensor over the bit-banged I2C bus and update the cached value.
// Returns true on a successful read.  Bit-bangs GPIO only (no lwIP); call from
// the application tick, not the httpd thread.
//
bool TempSensorPoll(void);

//
// Latest cached reading in centi-degrees Celsius.  Returns true and writes
// *pi32CentiC if a valid reading has been taken since boot; false otherwise.
//
bool TempSensorGet(int32_t *pi32CentiC);

#endif // __TEMP_SENSOR_H__
