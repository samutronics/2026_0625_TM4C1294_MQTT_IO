//*****************************************************************************
//
// main.c (CC35x1) - application entry for the MQTT-IO gateway on LP-EM-CC35X1.
//
// main_freertos.c (from the SDK) owns C startup: it runs Board_init(), creates
// one detached pthread running mainThread(), and starts the FreeRTOS scheduler.
// This file provides that mainThread() seam.
//
// Checkpoint B3 (in progress): first bring up the lwIP TCP/IP thread and the
// apps/httpd web server, proving the httpd.c/fs.c/fsdata.c + lwipopts machinery
// compiles AND links against the SDK.  The full init+tick seam (Wi-Fi STA
// bring-up via platform/cc35x1/net_wifi.c, ConfigInit -> DIN/Relay chain init
// -> MQTT client -> NetBIOS/SNTP, then a ~10 ms tick) lands in the next slice.
//
// Under NO_SYS=0 with LWIP_TCPIP_CORE_LOCKING=1 the lwIP raw API must be entered
// with the core lock held; httpd_init() is therefore wrapped in
// LOCK_TCPIP_CORE()/UNLOCK_TCPIP_CORE().
//
//*****************************************************************************

#include <stddef.h>

/* RTOS header files */
#include <FreeRTOS.h>
#include <task.h>

/* lwIP */
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/tcpip.h"
#include "lwip/apps/httpd.h"

//*****************************************************************************
//
// tcpip_init_done - signalled by the tcpip_thread once it is up.
//
//*****************************************************************************
static void
tcpip_init_done(void *pvArg)
{
    sys_sem_signal((sys_sem_t *)pvArg);
}

//*****************************************************************************
//
// mainThread - application task entry (invoked by main_freertos.c).
//
//*****************************************************************************
void *
mainThread(void *pvArg0)
{
    sys_sem_t sInitSem;

    (void)pvArg0;

    //
    // Start the lwIP TCP/IP thread and block until it has initialised.
    //
    sys_sem_new(&sInitSem, 0);
    tcpip_init(tcpip_init_done, &sInitSem);
    sys_sem_wait(&sInitSem);
    sys_sem_free(&sInitSem);

    //
    // Bring up the HTTP server (raw lwIP call -> hold the core lock).
    //
    LOCK_TCPIP_CORE();
    httpd_init();
    UNLOCK_TCPIP_CORE();

    //
    // B3 slice 2 replaces this idle loop with the real ~10 ms application tick.
    //
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
