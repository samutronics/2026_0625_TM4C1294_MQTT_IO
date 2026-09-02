//*****************************************************************************
//
// io_scan.h - Periodic field-I/O scan + input->output binding engine.
//
// Single-source across platforms.  Each application tick this layer polls the
// SN65HVS882 input chain and the DRV8860 relay nFAULT line, feeds pushbutton
// levels to the click-detector, publishes input state/events over MQTT, and
// applies the configured input->output bindings through the output controller.
//
// The chain drivers (din_chain/relay_chain) are bit-banged through pal_gpio and
// every collaborator (config, input_events, output_ctrl, mqtt_app) is portable,
// so this engine is MCU-independent: each platform only calls the two entry
// points below from its main loop.
//
//*****************************************************************************

#ifndef __IO_SCAN_H__
#define __IO_SCAN_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

//
// Wire the pushbutton click-detector to the MQTT event publisher and the
// binding engine.  Call once, after InputEvents and MQTTApp are initialized.
//
void IOScanInit(void);

//
// Poll the input chain (publish changes, run bindings) and the relay fault
// line (log transitions).  Call once per application tick.
//
void IOScanTick(void);

//
// Total logical digital-input count: the SN65HVS882 SPI chain inputs
// (ConfigGetDinDevices()*8) plus the platform-local inputs appended after them
// (WebPlatformLocalInputCount - e.g. the CC35x1 on-board buttons), clamped so the
// appended byte fits the fixed IO_MAX_BYTES buffers and stays within CFG_MAX_INPUTS.
// This is the single "how many inputs exist" lever the web UI, MQTT discovery and
// the scan all key off, so the local inputs behave exactly like SPI inputs.
//
uint16_t IOInputCount(void);

//
// Sample every input into pui8Buf, LSB-first per byte: the SPI chain bytes first
// (via DINChainRead), then one appended byte holding the platform-local inputs at
// byte index == SPI device count.  Returns the number of bytes written.  The
// appended byte is masked to the low WebPlatformLocalInputCount() bits.
//
uint8_t IOInputReadAll(uint8_t *pui8Buf, uint8_t ui8BufLen);

#ifdef __cplusplus
}
#endif

#endif // __IO_SCAN_H__
