//*****************************************************************************
//
// main.c (CC35x1) - application entry for the MQTT-IO gateway on LP-EM-CC35X1.
//
// main_freertos.c (from the SDK) owns C startup: it runs Board_init(), creates
// one detached pthread running mainThread(), and starts the FreeRTOS scheduler.
// This file provides that mainThread() seam.
//
// Checkpoint A (current): a minimal boot baseline - bring the scheduler up and
// idle - to prove the toolchain, SysConfig generation, SDK libraries, and the
// out-of-tree action="link" source mechanism all produce a clean link before
// the portable layer (mqtt_io/common + pal + platform/cc35x1) is wired in.
//
// Checkpoint B will grow mainThread() into the real init+tick seam mirroring the
// TM4C enet_io.c flow: Wlan_Start -> lwIP up -> ConfigInit -> DIN/Relay chain
// init -> httpd + CGI/SSI -> MQTT client -> NetBIOS/SNTP, then a ~10 ms tick.
//
//*****************************************************************************

#include <stddef.h>

/* RTOS header files */
#include <FreeRTOS.h>
#include <task.h>

//*****************************************************************************
//
// mainThread - application task entry (invoked by main_freertos.c).
//
//*****************************************************************************
void *
mainThread(void *pvArg0)
{
    (void)pvArg0;

    //
    // Checkpoint A: nothing to do yet - just yield the CPU forever so the
    // scheduler and idle task run. Replaced by the real init+tick seam in
    // Checkpoint B.
    //
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
