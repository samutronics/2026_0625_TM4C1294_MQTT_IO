//*****************************************************************************
//
// pal_log.c (CC35x1) - PAL logging backed by the SDK UART2 driver.
//
// Portable code calls PalLog() the same way on both platforms.  On the TM4C the
// backend is TivaWare uartstdio (UART0 backchannel); here it is the TI Drivers
// UART2 instance CONFIG_UART2_0 (the LaunchPad's XDS110 backchannel, 115200-8N1,
// configured in SysConfig), matching the SDK's own uart_term.c adapter.
//
// The line is formatted with libc vsnprintf into a bounded stack buffer and
// pushed out with a single UART2_write.  The UART is opened lazily on the first
// call so the interface stays init-free (mirrors uartstdio's implicit backend);
// the platform main() may also open CONFIG_UART2_0 first - UART2_open is not
// re-entered once the handle is cached.
//
// NOTE: the default UART2 read/write mode is BLOCKING, which requires the
// FreeRTOS scheduler to be running.  PalLog is therefore for task context; very
// early boot / fault-path logging should use a polling sink instead.
//
//*****************************************************************************

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <ti/drivers/UART2.h>
#include "ti_drivers_config.h"
#include "pal_log.h"

//
// Longest log line we emit; longer output is truncated (vsnprintf keeps it
// NUL-terminated).  Kept off the stack-frame budget of deep call chains.
//
#define PAL_LOG_BUF_LEN     160

static UART2_Handle g_sLogUart = NULL;

//
// Shared formatter/emitter for PalLog() and Report(): lazily open the
// backchannel UART, vsnprintf into a bounded buffer, and push one write.
//
static void
LogVList(const char *pcFormat, va_list vaArgP)
{
    char   pcBuf[PAL_LOG_BUF_LEN];
    int    iLen;
    size_t szWritten;

    //
    // Open the backchannel UART on first use.
    //
    if(g_sLogUart == NULL)
    {
        UART2_Params sParams;

        UART2_Params_init(&sParams);
        sParams.baudRate = 115200;
        g_sLogUart = UART2_open(CONFIG_UART2_0, &sParams);
        if(g_sLogUart == NULL)
        {
            return;
        }
    }

    iLen = vsnprintf(pcBuf, sizeof(pcBuf), pcFormat, vaArgP);

    if(iLen <= 0)
    {
        return;
    }

    //
    // Clamp a truncated result to the buffer's usable length.
    //
    if((size_t)iLen >= sizeof(pcBuf))
    {
        iLen = (int)(sizeof(pcBuf) - 1);
    }

    UART2_write(g_sLogUart, pcBuf, (size_t)iLen, &szWritten);
}

void
PalLog(const char *pcFormat, ...)
{
    va_list vaArgP;

    va_start(vaArgP, pcFormat);
    LogVList(pcFormat, vaArgP);
    va_end(vaArgP);
}

//
// Report() is the console-logging primitive the SDK's lwIP port and Wi-Fi
// stack call through the LWIP_PLATFORM_DIAG/ASSERT macros (normally provided by
// the demo's uart_term.c).  We back it with the same UART2 sink as PalLog
// instead of pulling in uart_term.c, which would contend for the same UART.
//
void
Report(const char *pcFormat, ...)
{
    va_list vaArgP;

    va_start(vaArgP, pcFormat);
    LogVList(pcFormat, vaArgP);
    va_end(vaArgP);
}
