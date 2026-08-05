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

#ifdef __cplusplus
}
#endif

#endif // __IO_SCAN_H__
