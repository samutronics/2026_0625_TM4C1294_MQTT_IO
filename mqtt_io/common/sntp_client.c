//*****************************************************************************
//
// sntp_client.c - Minimal SNTP client (RFC 4330 / NTP v3 unicast client).
//
// Flow:
//   SntpInit()  -> DNS lookup of server hostname
//   DNS cb      -> send 48-byte NTP request to port 123
//   UDP recv cb -> parse transmit timestamp, compute local time
//   SntpTick()  -> re-sync every SNTP_RESYNC_MS; retry every SNTP_RETRY_MS
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "utils/uartstdio.h"
#include "utils/ustdlib.h"
#include "config.h"
#include "sntp_client.h"

//*****************************************************************************
// Configuration
//*****************************************************************************
#define NTP_PORT            123u
#define NTP_PACKET_SIZE     48u
#define NTP_EPOCH_DELTA     2208988800u   // seconds between 1900 and 1970

#define SNTP_RETRY_MS       30000u        // retry after 30 s on failure
#define SNTP_RESYNC_MS      3600000u      // re-sync every 1 hour

//*****************************************************************************
// State machine
//*****************************************************************************
typedef enum
{
    SNTP_IDLE,          // not started or network not ready
    SNTP_DNS_PENDING,   // waiting for DNS callback
    SNTP_WAIT_REPLY,    // request sent, waiting for NTP reply
    SNTP_SYNCED         // have a valid time; countdown to re-sync
}
tSntpState;

static tSntpState    g_eSntpState    = SNTP_IDLE;
static struct udp_pcb *g_pSntpPcb   = NULL;
static ip_addr_t     g_sSntpAddr;
static uint32_t      g_ui32Elapsed  = 0;    // ms since last state entry
static uint32_t      g_ui32UnixBase = 0;    // Unix timestamp of last sync
static uint32_t      g_ui32MsSinceSync = 0; // ms elapsed since g_ui32UnixBase was set
static bool          g_bSynced      = false;

//*****************************************************************************
// Forward declaration
//*****************************************************************************
static void SntpSendRequest(void);

//*****************************************************************************
// UDP receive callback — parses the NTP reply.
//*****************************************************************************
static void
SntpRecvCB(void *arg, struct udp_pcb *pcb, struct pbuf *p,
            ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb; (void)addr; (void)port;

    if(p == NULL) { return; }

    if(p->tot_len >= NTP_PACKET_SIZE)
    {
        uint8_t aui8Pkt[NTP_PACKET_SIZE];
        uint32_t ui32Secs;

        pbuf_copy_partial(p, aui8Pkt, NTP_PACKET_SIZE, 0);
        pbuf_free(p);

        //
        // Transmit timestamp (offset 40, big-endian 32-bit seconds since 1900).
        //
        ui32Secs = ((uint32_t)aui8Pkt[40] << 24) |
                   ((uint32_t)aui8Pkt[41] << 16) |
                   ((uint32_t)aui8Pkt[42] <<  8) |
                   ((uint32_t)aui8Pkt[43]);

        if(ui32Secs < NTP_EPOCH_DELTA)
        {
            UARTprintf("SNTP: bogus timestamp, retrying.\n");
            g_eSntpState  = SNTP_DNS_PENDING;
            g_ui32Elapsed = 0;
            return;
        }

        g_ui32UnixBase    = ui32Secs - NTP_EPOCH_DELTA;
        g_ui32MsSinceSync = 0;
        g_bSynced         = true;
        g_eSntpState      = SNTP_SYNCED;
        g_ui32Elapsed     = 0;
        UARTprintf("SNTP: synced, Unix=%u\n", g_ui32UnixBase);
    }
    else
    {
        pbuf_free(p);
    }
}

//*****************************************************************************
// DNS found callback.
//*****************************************************************************
static void
SntpDnsCB(const char *pcName, ip_addr_t *pAddr, void *pArg)
{
    (void)pcName; (void)pArg;

    if(pAddr == NULL)
    {
        UARTprintf("SNTP: DNS failed for %s, retrying.\n", pcName);
        g_eSntpState  = SNTP_IDLE;   // will retry after SNTP_RETRY_MS
        g_ui32Elapsed = 0;
        return;
    }

    g_sSntpAddr   = *pAddr;
    g_eSntpState  = SNTP_WAIT_REPLY;
    g_ui32Elapsed = 0;
    SntpSendRequest();
}

//*****************************************************************************
// Build and transmit a 48-byte NTP client request.
//*****************************************************************************
static void
SntpSendRequest(void)
{
    struct pbuf *p;
    uint8_t     *pui8Data;

    if(g_pSntpPcb == NULL) { return; }

    p = pbuf_alloc(PBUF_TRANSPORT, NTP_PACKET_SIZE, PBUF_RAM);
    if(p == NULL) { return; }

    pui8Data = (uint8_t *)p->payload;
    memset(pui8Data, 0, NTP_PACKET_SIZE);
    pui8Data[0] = 0x1Bu;   // LI=0, Version=3, Mode=3 (client)

    udp_sendto(g_pSntpPcb, p, &g_sSntpAddr, NTP_PORT);
    pbuf_free(p);
    UARTprintf("SNTP: request sent to server.\n");
}

//*****************************************************************************
// Start (or restart) the SNTP client.
//*****************************************************************************
void
SntpInit(void)
{
    const tNTPConfig *psNtp = ConfigNtpGet();

    if(g_pSntpPcb == NULL)
    {
        g_pSntpPcb = udp_new();
        if(g_pSntpPcb == NULL) { return; }
        udp_recv(g_pSntpPcb, SntpRecvCB, NULL);
    }

    g_eSntpState  = SNTP_DNS_PENDING;
    g_ui32Elapsed = 0;

    //
    // Kick off DNS resolution.  The callback fires from the lwIP timer context.
    //
    if(dns_gethostbyname(psNtp->pcServer, &g_sSntpAddr, SntpDnsCB, NULL)
       == ERR_OK)
    {
        //
        // Address already cached — go straight to sending.
        //
        g_eSntpState = SNTP_WAIT_REPLY;
        SntpSendRequest();
    }
}

//*****************************************************************************
// Advance timers.  Call from the main loop with elapsed ms.
//*****************************************************************************
void
SntpTick(uint32_t ui32ElapsedMs)
{
    g_ui32Elapsed += ui32ElapsedMs;

    if(g_bSynced)
    {
        g_ui32MsSinceSync += ui32ElapsedMs;
    }

    switch(g_eSntpState)
    {
        case SNTP_IDLE:
            //
            // Wait before retrying after a DNS failure.
            //
            if(g_ui32Elapsed >= SNTP_RETRY_MS)
            {
                g_ui32Elapsed = 0;
                SntpInit();
            }
            break;

        case SNTP_DNS_PENDING:
            //
            // lwIP DNS drives itself via sys_check_timeouts() called in the
            // Ethernet ISR — nothing to do here except guard against timeout.
            //
            if(g_ui32Elapsed >= SNTP_RETRY_MS)
            {
                g_eSntpState  = SNTP_IDLE;
                g_ui32Elapsed = 0;
            }
            break;

        case SNTP_WAIT_REPLY:
            //
            // Guard against a missing reply.
            //
            if(g_ui32Elapsed >= 5000u)   // 5 s timeout
            {
                UARTprintf("SNTP: no reply, retrying DNS.\n");
                g_eSntpState  = SNTP_IDLE;
                g_ui32Elapsed = 0;
            }
            break;

        case SNTP_SYNCED:
            //
            // Schedule a periodic re-sync.
            //
            if(g_ui32Elapsed >= SNTP_RESYNC_MS)
            {
                g_ui32Elapsed = 0;
                UARTprintf("SNTP: re-syncing.\n");
                SntpInit();
            }
            break;
    }
}

//*****************************************************************************
bool
SntpIsSynced(void)
{
    return(g_bSynced);
}

//*****************************************************************************
void
SntpGetTimeStr(char *pcBuf, int iBufLen)
{
    uint32_t ui32Unix;
    uint32_t ui32Day, ui32Hour, ui32Min, ui32Sec;
    int8_t   i8Tz;

    if(!g_bSynced || iBufLen < 9)
    {
        if(iBufLen >= 9) { usnprintf(pcBuf, iBufLen, "--:--:--"); }
        return;
    }

    //
    // Add ms-level elapsed time to get current second.
    //
    ui32Unix = g_ui32UnixBase + (g_ui32MsSinceSync / 1000u);

    //
    // Apply TZ offset (hours).
    //
    i8Tz = ConfigNtpGet()->i8TzOffset;
    if(i8Tz >= 0)
    {
        ui32Unix += (uint32_t)i8Tz * 3600u;
    }
    else
    {
        uint32_t ui32Sub = (uint32_t)(-i8Tz) * 3600u;
        ui32Unix = (ui32Unix > ui32Sub) ? (ui32Unix - ui32Sub) : 0u;
    }

    //
    // Extract HH:MM:SS.  Gregorian calendar not needed — only time of day.
    //
    ui32Sec  = ui32Unix % 60u;
    ui32Min  = (ui32Unix / 60u) % 60u;
    ui32Hour = (ui32Unix / 3600u) % 24u;
    (void)ui32Day;

    usnprintf(pcBuf, iBufLen, "%02u:%02u:%02u", ui32Hour, ui32Min, ui32Sec);
}
