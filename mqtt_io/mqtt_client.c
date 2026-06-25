//*****************************************************************************
//
// mqtt_client.c - Minimal MQTT 3.1.1 client over the lwIP 1.4.1 raw TCP API.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "lwip/opt.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "driverlib/interrupt.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "utils/uartstdio.h"
#include "mqtt_client.h"

//*****************************************************************************
//
// MQTT control packet types (high nibble of the fixed header byte).
//
//*****************************************************************************
#define MQTT_CONNECT        0x10
#define MQTT_CONNACK        0x20
#define MQTT_PUBLISH        0x30
#define MQTT_PUBACK         0x40
#define MQTT_SUBSCRIBE      0x80
#define MQTT_SUBACK         0x90
#define MQTT_PINGREQ        0xC0
#define MQTT_PINGRESP       0xD0
#define MQTT_DISCONNECT     0xE0

//
// Protocol parameters.
//
#define MQTT_KEEPALIVE_S    60          // Keep-alive interval (seconds).
#define MQTT_RECONNECT_MS   5000        // Delay between reconnect attempts.
#define MQTT_TXBUF_SIZE     512
#define MQTT_RXBUF_SIZE     512
#define MQTT_HOST_LEN       64
#define MQTT_STR_LEN        64

//*****************************************************************************
//
// Client state.
//
//*****************************************************************************
typedef struct
{
    struct tcp_pcb *psPcb;
    tMQTTCliState   eState;
    bool            bEnabled;           // Should we keep (re)connecting?

    ip_addr_t       sBrokerIP;
    char            pcHost[MQTT_HOST_LEN];
    uint16_t        ui16Port;
    char            pcClientID[MQTT_STR_LEN];
    char            pcUser[MQTT_STR_LEN];
    char            pcPass[MQTT_STR_LEN];

    bool            bWillSet;
    char            pcWillTopic[MQTT_STR_LEN];
    char            pcWillMsg[MQTT_STR_LEN];
    uint8_t         ui8WillRetain;

    uint16_t        ui16PacketID;       // Next packet identifier.

    uint32_t        ui32KeepAliveMs;    // Time since last packet sent.
    uint32_t        ui32ReconnectMs;    // Backoff countdown when idle.
    uint32_t        ui32PingWaitMs;     // Time awaiting PINGRESP (0 = none).

    // Receive assembly buffer (packets may span TCP segments).
    uint8_t         pui8Rx[MQTT_RXBUF_SIZE];
    uint16_t        ui16RxLen;

    tMQTTPubCB      pfnPub;
}
tMQTTClient;

static tMQTTClient g_sCli;
static uint8_t g_pui8Tx[MQTT_TXBUF_SIZE];

//*****************************************************************************
//
// Forward declarations of lwIP callbacks.
//
//*****************************************************************************
static err_t MQTTConnectedCB(void *pvArg, struct tcp_pcb *psPcb, err_t eErr);
static err_t MQTTRecvCB(void *pvArg, struct tcp_pcb *psPcb, struct pbuf *psBuf,
                        err_t eErr);
static void  MQTTErrCB(void *pvArg, err_t eErr);
static err_t MQTTPollCB(void *pvArg, struct tcp_pcb *psPcb);
static void  MQTTDNSFoundCB(const char *pcName, ip_addr_t *psIP, void *pvArg);
static void  MQTTTryConnect(void);
static int   MQTTSendConnect(void);

//*****************************************************************************
//
// Critical-section helpers.  lwIP processing happens in the Ethernet/SysTick
// interrupts; masking interrupts makes a sequence of raw-API calls from the
// main loop atomic with respect to that processing.
//
//*****************************************************************************
static inline bool
MQTTLock(void)
{
    return(MAP_IntMasterDisable());
}

static inline void
MQTTUnlock(bool bWasMasked)
{
    if(!bWasMasked)
    {
        MAP_IntMasterEnable();
    }
}

//*****************************************************************************
//
// Encode an MQTT "remaining length" field.  Returns the number of bytes used.
//
//*****************************************************************************
static uint16_t
MQTTEncodeLen(uint8_t *pui8Buf, uint32_t ui32Len)
{
    uint16_t ui16N = 0;
    do
    {
        uint8_t ui8Byte = ui32Len & 0x7F;
        ui32Len >>= 7;
        if(ui32Len)
        {
            ui8Byte |= 0x80;
        }
        pui8Buf[ui16N++] = ui8Byte;
    }
    while(ui32Len);
    return(ui16N);
}

//*****************************************************************************
//
// Append a length-prefixed UTF-8 string to a buffer at *pui16Pos.
//
//*****************************************************************************
static void
MQTTPutStr(uint8_t *pui8Buf, uint16_t *pui16Pos, const char *pcStr)
{
    uint16_t ui16Len = (uint16_t)strlen(pcStr);
    pui8Buf[(*pui16Pos)++] = (uint8_t)(ui16Len >> 8);
    pui8Buf[(*pui16Pos)++] = (uint8_t)(ui16Len & 0xFF);
    memcpy(&pui8Buf[*pui16Pos], pcStr, ui16Len);
    *pui16Pos += ui16Len;
}

//*****************************************************************************
//
// Write a fully-formed packet (already in g_pui8Tx for the variable header /
// payload starting at offset ui16HdrPos) over the TCP connection.  The fixed
// header (type + remaining length) is prepended here.  Caller must hold the
// lock (or be in interrupt context).  Returns 0 on success.
//
//*****************************************************************************
static int
MQTTSendRaw(const uint8_t *pui8Data, uint16_t ui16Len)
{
    err_t eErr;

    if((g_sCli.psPcb == NULL) || (g_sCli.eState < MQTT_CLI_CONNECT))
    {
        return(-1);
    }

    eErr = tcp_write(g_sCli.psPcb, pui8Data, ui16Len, TCP_WRITE_FLAG_COPY);
    if(eErr != ERR_OK)
    {
        return(-1);
    }
    tcp_output(g_sCli.psPcb);

    g_sCli.ui32KeepAliveMs = 0;
    return(0);
}

//*****************************************************************************
//
// Build and send the CONNECT packet.  Called from the TCP connected callback
// (interrupt context).
//
//*****************************************************************************
static int
MQTTSendConnect(void)
{
    uint16_t ui16Pos, ui16Rem, ui16Hdr;
    uint8_t  ui8Flags;
    uint8_t  pui8Var[256];

    //
    // Variable header: protocol name + level + flags + keep-alive.
    //
    ui16Pos = 0;
    MQTTPutStr(pui8Var, &ui16Pos, "MQTT");      // Protocol name.
    pui8Var[ui16Pos++] = 0x04;                  // Protocol level 4 (3.1.1).

    ui8Flags = 0x02;                            // Clean session.
    if(g_sCli.bWillSet)
    {
        ui8Flags |= 0x04;                       // Will flag (QoS 0).
        if(g_sCli.ui8WillRetain)
        {
            ui8Flags |= 0x20;                   // Will retain.
        }
    }
    if(g_sCli.pcUser[0])
    {
        ui8Flags |= 0x80;                       // User name.
        if(g_sCli.pcPass[0])
        {
            ui8Flags |= 0x40;                   // Password.
        }
    }
    pui8Var[ui16Pos++] = ui8Flags;
    pui8Var[ui16Pos++] = (uint8_t)(MQTT_KEEPALIVE_S >> 8);
    pui8Var[ui16Pos++] = (uint8_t)(MQTT_KEEPALIVE_S & 0xFF);

    //
    // Payload: client id, [will topic, will msg], [user], [pass].
    //
    MQTTPutStr(pui8Var, &ui16Pos, g_sCli.pcClientID);
    if(g_sCli.bWillSet)
    {
        MQTTPutStr(pui8Var, &ui16Pos, g_sCli.pcWillTopic);
        MQTTPutStr(pui8Var, &ui16Pos, g_sCli.pcWillMsg);
    }
    if(g_sCli.pcUser[0])
    {
        MQTTPutStr(pui8Var, &ui16Pos, g_sCli.pcUser);
        if(g_sCli.pcPass[0])
        {
            MQTTPutStr(pui8Var, &ui16Pos, g_sCli.pcPass);
        }
    }

    //
    // Assemble the full packet: fixed header + variable section.
    //
    ui16Rem = ui16Pos;
    g_pui8Tx[0] = MQTT_CONNECT;
    ui16Hdr = 1 + MQTTEncodeLen(&g_pui8Tx[1], ui16Rem);
    memcpy(&g_pui8Tx[ui16Hdr], pui8Var, ui16Rem);

    return(MQTTSendRaw(g_pui8Tx, ui16Hdr + ui16Rem));
}

//*****************************************************************************
//
// Public: initialise.
//
//*****************************************************************************
void
MQTTClientInit(tMQTTPubCB pfnPub)
{
    memset(&g_sCli, 0, sizeof(g_sCli));
    g_sCli.eState = MQTT_CLI_IDLE;
    g_sCli.ui16PacketID = 1;
    g_sCli.pfnPub = pfnPub;
}

//*****************************************************************************
//
// Public: configure the last-will message.
//
//*****************************************************************************
void
MQTTClientSetWill(const char *pcTopic, const char *pcMsg, uint8_t ui8Retain)
{
    if(pcTopic == NULL)
    {
        g_sCli.bWillSet = false;
        return;
    }
    g_sCli.bWillSet = true;
    g_sCli.ui8WillRetain = ui8Retain;
    strncpy(g_sCli.pcWillTopic, pcTopic, MQTT_STR_LEN - 1);
    g_sCli.pcWillTopic[MQTT_STR_LEN - 1] = '\0';
    strncpy(g_sCli.pcWillMsg, pcMsg, MQTT_STR_LEN - 1);
    g_sCli.pcWillMsg[MQTT_STR_LEN - 1] = '\0';
}

//*****************************************************************************
//
// Public: start connecting.
//
//*****************************************************************************
void
MQTTClientStart(const char *pcHost, uint16_t ui16Port, const char *pcClientID,
                const char *pcUser, const char *pcPass)
{
    bool bMasked = MQTTLock();

    //
    // Drop any existing connection first.
    //
    if(g_sCli.psPcb)
    {
        tcp_arg(g_sCli.psPcb, NULL);
        tcp_abort(g_sCli.psPcb);
        g_sCli.psPcb = NULL;
    }

    strncpy(g_sCli.pcHost, pcHost ? pcHost : "", MQTT_HOST_LEN - 1);
    g_sCli.pcHost[MQTT_HOST_LEN - 1] = '\0';
    g_sCli.ui16Port = ui16Port ? ui16Port : 1883;
    strncpy(g_sCli.pcClientID, pcClientID ? pcClientID : "tm4c", MQTT_STR_LEN - 1);
    g_sCli.pcClientID[MQTT_STR_LEN - 1] = '\0';
    strncpy(g_sCli.pcUser, pcUser ? pcUser : "", MQTT_STR_LEN - 1);
    g_sCli.pcUser[MQTT_STR_LEN - 1] = '\0';
    strncpy(g_sCli.pcPass, pcPass ? pcPass : "", MQTT_STR_LEN - 1);
    g_sCli.pcPass[MQTT_STR_LEN - 1] = '\0';

    g_sCli.bEnabled = true;
    g_sCli.eState = MQTT_CLI_IDLE;
    g_sCli.ui32ReconnectMs = 0;             // Attempt immediately.

    MQTTUnlock(bMasked);
}

//*****************************************************************************
//
// Public: stop.
//
//*****************************************************************************
void
MQTTClientStop(void)
{
    bool bMasked = MQTTLock();

    g_sCli.bEnabled = false;
    if(g_sCli.psPcb)
    {
        tcp_arg(g_sCli.psPcb, NULL);
        tcp_abort(g_sCli.psPcb);
        g_sCli.psPcb = NULL;
    }
    g_sCli.eState = MQTT_CLI_IDLE;

    MQTTUnlock(bMasked);
}

//*****************************************************************************
//
// Begin a connection attempt (DNS resolve then TCP connect).
//
//*****************************************************************************
static void
MQTTTryConnect(void)
{
    err_t eErr;

    if(g_sCli.pcHost[0] == '\0')
    {
        return;
    }

    //
    // Try a dotted-quad first; fall back to DNS for a hostname.
    //
    if(ipaddr_aton(g_sCli.pcHost, &g_sCli.sBrokerIP))
    {
        g_sCli.eState = MQTT_CLI_TCP;
        MQTTDNSFoundCB(g_sCli.pcHost, &g_sCli.sBrokerIP, NULL);
        return;
    }

    g_sCli.eState = MQTT_CLI_DNS;
    eErr = dns_gethostbyname(g_sCli.pcHost, &g_sCli.sBrokerIP,
                             MQTTDNSFoundCB, NULL);
    if(eErr == ERR_OK)
    {
        //
        // Address was cached - proceed straight to TCP connect.
        //
        MQTTDNSFoundCB(g_sCli.pcHost, &g_sCli.sBrokerIP, NULL);
    }
    else if(eErr != ERR_INPROGRESS)
    {
        UARTprintf("MQTT: DNS lookup of '%s' failed.\n", g_sCli.pcHost);
        g_sCli.eState = MQTT_CLI_IDLE;
        g_sCli.ui32ReconnectMs = MQTT_RECONNECT_MS;
    }
}

//*****************************************************************************
//
// DNS resolution callback (also called directly for cached/literal IPs).
//
//*****************************************************************************
static void
MQTTDNSFoundCB(const char *pcName, ip_addr_t *psIP, void *pvArg)
{
    err_t eErr;
    (void)pcName;
    (void)pvArg;

    if(psIP == NULL)
    {
        UARTprintf("MQTT: DNS resolution failed.\n");
        g_sCli.eState = MQTT_CLI_IDLE;
        g_sCli.ui32ReconnectMs = MQTT_RECONNECT_MS;
        return;
    }

    g_sCli.sBrokerIP = *psIP;

    g_sCli.psPcb = tcp_new();
    if(g_sCli.psPcb == NULL)
    {
        g_sCli.eState = MQTT_CLI_IDLE;
        g_sCli.ui32ReconnectMs = MQTT_RECONNECT_MS;
        return;
    }

    tcp_arg(g_sCli.psPcb, &g_sCli);
    tcp_err(g_sCli.psPcb, MQTTErrCB);
    tcp_recv(g_sCli.psPcb, MQTTRecvCB);
    tcp_poll(g_sCli.psPcb, MQTTPollCB, 4);      // ~2 s poll (4 * 500 ms).

    g_sCli.eState = MQTT_CLI_TCP;
    g_sCli.ui16RxLen = 0;

    eErr = tcp_connect(g_sCli.psPcb, &g_sCli.sBrokerIP, g_sCli.ui16Port,
                       MQTTConnectedCB);
    if(eErr != ERR_OK)
    {
        tcp_abort(g_sCli.psPcb);
        g_sCli.psPcb = NULL;
        g_sCli.eState = MQTT_CLI_IDLE;
        g_sCli.ui32ReconnectMs = MQTT_RECONNECT_MS;
    }
}

//*****************************************************************************
//
// TCP connected callback - send the CONNECT packet.
//
//*****************************************************************************
static err_t
MQTTConnectedCB(void *pvArg, struct tcp_pcb *psPcb, err_t eErr)
{
    (void)pvArg;
    (void)psPcb;

    if(eErr != ERR_OK)
    {
        return(eErr);
    }

    g_sCli.eState = MQTT_CLI_CONNECT;
    g_sCli.ui32KeepAliveMs = 0;
    g_sCli.ui32PingWaitMs = 0;

    if(MQTTSendConnect() != 0)
    {
        return(ERR_ABRT);
    }
    return(ERR_OK);
}

//*****************************************************************************
//
// Handle one fully-received MQTT packet held in the assembly buffer.
//
//*****************************************************************************
static void
MQTTHandlePacket(const uint8_t *pui8Pkt, uint16_t ui16TotLen,
                 uint16_t ui16HdrLen, uint32_t ui32RemLen)
{
    uint8_t ui8Type = pui8Pkt[0] & 0xF0;
    const uint8_t *pui8Body = &pui8Pkt[ui16HdrLen];

    (void)ui16TotLen;

    switch(ui8Type)
    {
        case MQTT_CONNACK:
        {
            //
            // CONNACK: byte 1 is the return code (0 = accepted).
            //
            if((ui32RemLen >= 2) && (pui8Body[1] == 0))
            {
                g_sCli.eState = MQTT_CLI_READY;
                UARTprintf("MQTT: connected to broker.\n");
            }
            else
            {
                UARTprintf("MQTT: CONNECT refused (rc=%d).\n",
                           (ui32RemLen >= 2) ? pui8Body[1] : -1);
                if(g_sCli.psPcb)
                {
                    tcp_abort(g_sCli.psPcb);
                    g_sCli.psPcb = NULL;
                }
                g_sCli.eState = MQTT_CLI_IDLE;
                g_sCli.ui32ReconnectMs = MQTT_RECONNECT_MS;
            }
            break;
        }

        case MQTT_PINGRESP:
        {
            g_sCli.ui32PingWaitMs = 0;
            break;
        }

        case MQTT_SUBACK:
        case MQTT_PUBACK:
        {
            //
            // Nothing to do for QoS 0 housekeeping.
            //
            break;
        }

        case MQTT_PUBLISH:
        {
            //
            // Topic length, topic, then payload (QoS 0 -> no packet id).
            //
            if(ui32RemLen >= 2)
            {
                uint16_t ui16TopicLen = ((uint16_t)pui8Body[0] << 8) |
                                         pui8Body[1];
                if((uint32_t)(2 + ui16TopicLen) <= ui32RemLen)
                {
                    const char *pcTopic = (const char *)&pui8Body[2];
                    const uint8_t *pui8Payload = &pui8Body[2 + ui16TopicLen];
                    uint16_t ui16PayLen = (uint16_t)(ui32RemLen - 2 -
                                                     ui16TopicLen);
                    if(g_sCli.pfnPub)
                    {
                        g_sCli.pfnPub(pcTopic, ui16TopicLen, pui8Payload,
                                      ui16PayLen);
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

//*****************************************************************************
//
// TCP receive callback - assemble and dispatch MQTT packets.
//
//*****************************************************************************
static err_t
MQTTRecvCB(void *pvArg, struct tcp_pcb *psPcb, struct pbuf *psBuf, err_t eErr)
{
    struct pbuf *psSeg;
    (void)pvArg;

    //
    // A NULL pbuf signals the remote closed the connection.
    //
    if((psBuf == NULL) || (eErr != ERR_OK))
    {
        if(psBuf)
        {
            pbuf_free(psBuf);
        }
        tcp_arg(psPcb, NULL);
        tcp_close(psPcb);
        if(g_sCli.psPcb == psPcb)
        {
            g_sCli.psPcb = NULL;
            g_sCli.eState = MQTT_CLI_IDLE;
            g_sCli.ui32ReconnectMs = MQTT_RECONNECT_MS;
        }
        return(ERR_OK);
    }

    //
    // Copy the incoming data into the assembly buffer.
    //
    for(psSeg = psBuf; psSeg != NULL; psSeg = psSeg->next)
    {
        if((g_sCli.ui16RxLen + psSeg->len) <= MQTT_RXBUF_SIZE)
        {
            memcpy(&g_sCli.pui8Rx[g_sCli.ui16RxLen], psSeg->payload,
                   psSeg->len);
            g_sCli.ui16RxLen += psSeg->len;
        }
        else
        {
            //
            // Overflow - discard what we have and resync.
            //
            g_sCli.ui16RxLen = 0;
        }
    }

    tcp_recved(psPcb, psBuf->tot_len);
    pbuf_free(psBuf);

    //
    // Parse as many complete packets as the buffer holds.
    //
    while(g_sCli.ui16RxLen >= 2)
    {
        uint32_t ui32RemLen = 0;
        uint16_t ui16Mult = 1, ui16HdrLen = 1, ui16Idx;

        //
        // Decode the variable-length "remaining length".
        //
        for(ui16Idx = 1; ui16Idx < g_sCli.ui16RxLen && ui16Idx <= 4; ui16Idx++)
        {
            uint8_t ui8Byte = g_sCli.pui8Rx[ui16Idx];
            ui32RemLen += (ui8Byte & 0x7F) * ui16Mult;
            ui16HdrLen++;
            if((ui8Byte & 0x80) == 0)
            {
                break;
            }
            ui16Mult <<= 7;
        }

        //
        // If the length field or full packet is not yet present, wait.
        //
        if((g_sCli.pui8Rx[ui16HdrLen - 1] & 0x80) &&
           (g_sCli.ui16RxLen <= ui16HdrLen))
        {
            break;
        }
        if(g_sCli.ui16RxLen < (ui16HdrLen + ui32RemLen))
        {
            break;
        }

        MQTTHandlePacket(g_sCli.pui8Rx, (uint16_t)(ui16HdrLen + ui32RemLen),
                         ui16HdrLen, ui32RemLen);

        //
        // Shift any trailing bytes down to the front of the buffer.
        //
        {
            uint16_t ui16Consumed = (uint16_t)(ui16HdrLen + ui32RemLen);
            uint16_t ui16Left = g_sCli.ui16RxLen - ui16Consumed;
            if(ui16Left)
            {
                memmove(g_sCli.pui8Rx, &g_sCli.pui8Rx[ui16Consumed], ui16Left);
            }
            g_sCli.ui16RxLen = ui16Left;
        }
    }

    return(ERR_OK);
}

//*****************************************************************************
//
// TCP error callback - the pcb is already freed by lwIP.
//
//*****************************************************************************
static void
MQTTErrCB(void *pvArg, err_t eErr)
{
    (void)pvArg;
    (void)eErr;

    g_sCli.psPcb = NULL;
    g_sCli.eState = MQTT_CLI_IDLE;
    g_sCli.ui32ReconnectMs = MQTT_RECONNECT_MS;
    UARTprintf("MQTT: connection error (%d).\n", eErr);
}

//*****************************************************************************
//
// TCP poll callback - used only as a keep-alive safety net.
//
//*****************************************************************************
static err_t
MQTTPollCB(void *pvArg, struct tcp_pcb *psPcb)
{
    (void)pvArg;
    (void)psPcb;
    return(ERR_OK);
}

//*****************************************************************************
//
// Public: publish (QoS 0).
//
//*****************************************************************************
int
MQTTClientPublish(const char *pcTopic, const uint8_t *pui8Payload,
                  uint16_t ui16Len, uint8_t ui8Retain)
{
    uint16_t ui16Pos, ui16Rem, ui16Hdr, ui16TopicLen;
    bool bMasked;
    int iRc;

    if(g_sCli.eState != MQTT_CLI_READY)
    {
        return(-1);
    }

    ui16TopicLen = (uint16_t)strlen(pcTopic);
    ui16Rem = 2 + ui16TopicLen + ui16Len;       // No packet id for QoS 0.
    if((ui16Rem + 5) > MQTT_TXBUF_SIZE)
    {
        return(-1);
    }

    bMasked = MQTTLock();

    g_pui8Tx[0] = MQTT_PUBLISH | (ui8Retain ? 0x01 : 0x00);
    ui16Hdr = 1 + MQTTEncodeLen(&g_pui8Tx[1], ui16Rem);

    ui16Pos = ui16Hdr;
    MQTTPutStr(g_pui8Tx, &ui16Pos, pcTopic);
    if(ui16Len)
    {
        memcpy(&g_pui8Tx[ui16Pos], pui8Payload, ui16Len);
        ui16Pos += ui16Len;
    }

    iRc = MQTTSendRaw(g_pui8Tx, ui16Pos);

    MQTTUnlock(bMasked);
    return(iRc);
}

//*****************************************************************************
//
// Public: subscribe (QoS 0).
//
//*****************************************************************************
int
MQTTClientSubscribe(const char *pcTopic)
{
    uint16_t ui16Pos, ui16Rem, ui16Hdr, ui16TopicLen;
    bool bMasked;
    int iRc;

    if(g_sCli.eState != MQTT_CLI_READY)
    {
        return(-1);
    }

    ui16TopicLen = (uint16_t)strlen(pcTopic);
    ui16Rem = 2 + 2 + ui16TopicLen + 1;         // pktid + topic + qos byte.

    bMasked = MQTTLock();

    g_pui8Tx[0] = MQTT_SUBSCRIBE | 0x02;        // SUBSCRIBE requires flags 0x02.
    ui16Hdr = 1 + MQTTEncodeLen(&g_pui8Tx[1], ui16Rem);

    ui16Pos = ui16Hdr;
    g_pui8Tx[ui16Pos++] = (uint8_t)(g_sCli.ui16PacketID >> 8);
    g_pui8Tx[ui16Pos++] = (uint8_t)(g_sCli.ui16PacketID & 0xFF);
    g_sCli.ui16PacketID++;
    MQTTPutStr(g_pui8Tx, &ui16Pos, pcTopic);
    g_pui8Tx[ui16Pos++] = 0x00;                 // Requested QoS 0.

    iRc = MQTTSendRaw(g_pui8Tx, ui16Pos);

    MQTTUnlock(bMasked);
    return(iRc);
}

//*****************************************************************************
//
// Send a PINGREQ.  Caller holds the lock.
//
//*****************************************************************************
static void
MQTTSendPing(void)
{
    uint8_t pui8Ping[2];
    pui8Ping[0] = MQTT_PINGREQ;
    pui8Ping[1] = 0x00;
    MQTTSendRaw(pui8Ping, 2);
}

//*****************************************************************************
//
// Public: periodic tick.  Drives reconnect backoff and keep-alive.
//
//*****************************************************************************
void
MQTTClientTick(uint32_t ui32ElapsedMs)
{
    bool bMasked = MQTTLock();

    switch(g_sCli.eState)
    {
        case MQTT_CLI_IDLE:
        {
            if(g_sCli.bEnabled && (g_sCli.pcHost[0] != '\0'))
            {
                if(g_sCli.ui32ReconnectMs <= ui32ElapsedMs)
                {
                    g_sCli.ui32ReconnectMs = 0;
                    MQTTTryConnect();
                }
                else
                {
                    g_sCli.ui32ReconnectMs -= ui32ElapsedMs;
                }
            }
            break;
        }

        case MQTT_CLI_READY:
        {
            g_sCli.ui32KeepAliveMs += ui32ElapsedMs;

            //
            // Send a ping at half the keep-alive interval.
            //
            if(g_sCli.ui32KeepAliveMs >= ((MQTT_KEEPALIVE_S * 1000) / 2))
            {
                MQTTSendPing();
                if(g_sCli.ui32PingWaitMs == 0)
                {
                    g_sCli.ui32PingWaitMs = 1;
                }
            }

            //
            // If a ping has gone unanswered for too long, drop the link.
            //
            if(g_sCli.ui32PingWaitMs)
            {
                g_sCli.ui32PingWaitMs += ui32ElapsedMs;
                if(g_sCli.ui32PingWaitMs > (MQTT_KEEPALIVE_S * 1000))
                {
                    UARTprintf("MQTT: keep-alive timeout.\n");
                    if(g_sCli.psPcb)
                    {
                        tcp_abort(g_sCli.psPcb);
                        g_sCli.psPcb = NULL;
                    }
                    g_sCli.eState = MQTT_CLI_IDLE;
                    g_sCli.ui32ReconnectMs = MQTT_RECONNECT_MS;
                }
            }
            break;
        }

        case MQTT_CLI_DNS:
        case MQTT_CLI_TCP:
        case MQTT_CLI_CONNECT:
        {
            //
            // Guard against a connection that never completes.
            //
            g_sCli.ui32KeepAliveMs += ui32ElapsedMs;
            if(g_sCli.ui32KeepAliveMs > 10000)
            {
                if(g_sCli.psPcb)
                {
                    tcp_abort(g_sCli.psPcb);
                    g_sCli.psPcb = NULL;
                }
                g_sCli.eState = MQTT_CLI_IDLE;
                g_sCli.ui32ReconnectMs = MQTT_RECONNECT_MS;
            }
            break;
        }

        default:
            break;
    }

    MQTTUnlock(bMasked);
}

//*****************************************************************************
//
// Public: state queries.
//
//*****************************************************************************
bool
MQTTClientIsReady(void)
{
    return(g_sCli.eState == MQTT_CLI_READY);
}

tMQTTCliState
MQTTClientState(void)
{
    return(g_sCli.eState);
}
