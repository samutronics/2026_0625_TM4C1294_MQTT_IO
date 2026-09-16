//*****************************************************************************
//
// pal_sys.h - Platform abstraction for system-level control the portable app
//             needs.  Currently just a reset, used by the web UI reboot /
//             factory-reset flow (WebUIResetPending() -> PalReboot()).
//
//*****************************************************************************

#ifndef __PAL_SYS_H__
#define __PAL_SYS_H__

#ifdef __cplusplus
extern "C"
{
#endif

//
// Reset the MCU.  Does not return.  (TM4C: MAP_SysCtlReset; CC35x1: Cortex-M33
// SYSRESETREQ.)  On a NO_SYS=0 build the caller should let any in-flight TCP
// response flush first (a short delay in the tick) before invoking this.
//
void PalReboot(void);

#ifdef __cplusplus
}
#endif

#endif // __PAL_SYS_H__
