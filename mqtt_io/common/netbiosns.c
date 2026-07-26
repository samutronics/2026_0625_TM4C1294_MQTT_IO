//*****************************************************************************
//
// netbiosns.c - Minimal NetBIOS Name Service (NBNS) responder for lwIP 1.4.1.
//
// Listens on UDP port 137 and answers a positive Name Query Response whenever a
// host asks for this device's name (the configured MQTT client ID, uppercased
// and truncated to 15 chars).  This lets a Windows machine on the same subnet
// open "http://<clientid>/" without knowing the DHCP-assigned IP.
//
// The packet is parsed and built by explicit byte offsets (no packed structs)
// to avoid unaligned-access faults on Cortex-M and to keep the code self-
// contained.  Modeled on the UDP responder pattern in utils/locator.c.
//
//*****************************************************************************

#include <stdint.h>
#include <string.h>
#include "lwip/netif.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "config.h"
#include "netbiosns.h"

//*****************************************************************************
//
// NBNS wire constants.
//
//*****************************************************************************
#define NBNS_PORT               137
#define NBNS_NAME_LEN           16      // 15 name chars + 1 type byte
#define NBNS_ENC_LEN            32      // first-level encoded name (2 bytes/char)
#define NBNS_HDR_LEN            12      // transaction header
#define NBNS_QTYPE_NB           0x0020  // NetBIOS general name (IP address)
#define NBNS_MIN_QUERY_LEN      (NBNS_HDR_LEN + 1 + NBNS_ENC_LEN + 1 + 4) // 50

// Offsets within the query datagram.
#define OFF_FLAGS               2
#define OFF_QDCOUNT             4
#define OFF_NAMELEN             12
#define OFF_ENCNAME             13      // 32 encoded bytes
#define OFF_QTYPE               46      // after len(1)+enc(32)+null(1)

//*****************************************************************************
//
// This node's NetBIOS name, snapshotted at init from the client ID.
//
//*****************************************************************************
static char     g_pcNbName[NBNS_NAME_LEN];  // uppercased, NUL padded
static uint32_t g_ui32NbLen;                // length of g_pcNbName (<= 15)

//*****************************************************************************
//
// Uppercase a single ASCII character.
//
//*****************************************************************************
static char
NbUpper(char c)
{
    return ((c >= 'a') && (c <= 'z')) ? (char)(c - ('a' - 'A')) : c;
}

//*****************************************************************************
//
// Decode a 32-byte first-level-encoded NetBIOS name into 16 raw bytes.  Each
// output byte is carried as two nibbles, each offset by 'A'.
//
//*****************************************************************************
static void
NbDecodeName(const uint8_t *pui8Enc, uint8_t *pui8Raw)
{
    uint32_t ui32Idx;

    for(ui32Idx = 0; ui32Idx < NBNS_NAME_LEN; ui32Idx++)
    {
        uint8_t ui8Hi = (uint8_t)(pui8Enc[ui32Idx * 2]     - 'A');
        uint8_t ui8Lo = (uint8_t)(pui8Enc[ui32Idx * 2 + 1] - 'A');
        pui8Raw[ui32Idx] = (uint8_t)((ui8Hi << 4) | (ui8Lo & 0x0f));
    }
}

//*****************************************************************************
//
// Return true if the decoded query name matches this node's name.  The name
// field is 15 chars space-padded (byte 15 is the service type, ignored here so
// we answer both workstation <00> and server <20> queries).
//
//*****************************************************************************
static int
NbNameMatches(const uint8_t *pui8Raw)
{
    uint32_t ui32Len;

    // Trim trailing spaces from the 15-char name portion.
    for(ui32Len = 15; ui32Len > 0; ui32Len--)
    {
        if(pui8Raw[ui32Len - 1] != ' ')
        {
            break;
        }
    }

    if(ui32Len != g_ui32NbLen)
    {
        return 0;
    }

    while(ui32Len--)
    {
        if(NbUpper((char)pui8Raw[ui32Len]) != g_pcNbName[ui32Len])
        {
            return 0;
        }
    }

    return 1;
}

//*****************************************************************************
//
// UDP receive callback: validate an incoming NBNS query and, on a name match,
// send a positive Name Query Response containing our current IP address.
//
//*****************************************************************************
static void
NbReceive(void *arg, struct udp_pcb *pcb, struct pbuf *p,
          struct ip_addr *addr, u16_t port)
{
    uint8_t *d;
    uint8_t pui8Raw[NBNS_NAME_LEN];
    uint16_t ui16Flags, ui16QdCount, ui16QType;
    struct pbuf *pResp;
    uint8_t *r;

    d = (uint8_t *)p->payload;

    // Must be a single, self-contained datagram of at least a full query.
    if((p->len < NBNS_MIN_QUERY_LEN) || (p->tot_len != p->len))
    {
        pbuf_free(p);
        return;
    }

    // Ignore responses (QR bit set) and non-query opcodes; require a question.
    ui16Flags   = (uint16_t)((d[OFF_FLAGS]   << 8) | d[OFF_FLAGS + 1]);
    ui16QdCount = (uint16_t)((d[OFF_QDCOUNT] << 8) | d[OFF_QDCOUNT + 1]);
    if((ui16Flags & 0x8000) || ((ui16Flags & 0x7800) != 0) || (ui16QdCount < 1))
    {
        pbuf_free(p);
        return;
    }

    // Question name must be a 32-byte encoded label; only answer NB queries.
    ui16QType = (uint16_t)((d[OFF_QTYPE] << 8) | d[OFF_QTYPE + 1]);
    if((d[OFF_NAMELEN] != NBNS_ENC_LEN) || (ui16QType != NBNS_QTYPE_NB))
    {
        pbuf_free(p);
        return;
    }

    // Decode and compare to our name.
    NbDecodeName(&d[OFF_ENCNAME], pui8Raw);
    if(!NbNameMatches(pui8Raw))
    {
        pbuf_free(p);
        return;
    }

    // Only respond if we have a bound IP address to advertise.
    if((netif_default == NULL) || (netif_default->ip_addr.addr == 0))
    {
        pbuf_free(p);
        return;
    }

    // Build the 62-byte positive Name Query Response.
    pResp = pbuf_alloc(PBUF_TRANSPORT, 62, PBUF_RAM);
    if(pResp == NULL)
    {
        pbuf_free(p);
        return;
    }
    r = (uint8_t *)pResp->payload;
    memset(r, 0, 62);

    r[0]  = d[0];               // transaction id (echo)
    r[1]  = d[1];
    r[2]  = 0x85;               // flags: response, authoritative, RD
    r[3]  = 0x00;
    // qdcount = 0 (already zeroed)
    r[6]  = 0x00;               // ancount = 1
    r[7]  = 0x01;
    r[12] = NBNS_ENC_LEN;       // answer name: echo the query's encoded label
    memcpy(&r[13], &d[OFF_ENCNAME], NBNS_ENC_LEN);
    r[45] = 0x00;               // name scope terminator
    r[46] = 0x00;               // type = NB (0x0020)
    r[47] = 0x20;
    r[48] = 0x00;               // class = IN (0x0001)
    r[49] = 0x01;
    r[50] = 0x00;               // TTL = 300000 s
    r[51] = 0x04;
    r[52] = 0x93;
    r[53] = 0xe0;
    r[54] = 0x00;               // rdata length = 6
    r[55] = 0x06;
    r[56] = 0x00;               // NB flags: unique, B-node
    r[57] = 0x00;
    memcpy(&r[58], &netif_default->ip_addr.addr, 4);  // IP (network order)

    // The request pbuf is no longer needed.
    pbuf_free(p);

    // Reply to the querying host.
    udp_sendto(pcb, pResp, addr, port);
    pbuf_free(pResp);
}

//*****************************************************************************
//
// Initialize the NetBIOS name responder.
//
//*****************************************************************************
void
NetbiosnsInit(void)
{
    const char *pcId;
    uint32_t ui32Idx;
    struct udp_pcb *pcb;

    // Snapshot the client ID as our NetBIOS name (uppercased, <= 15 chars).
    pcId = ConfigGet()->pcClientID;
    g_ui32NbLen = 0;
    for(ui32Idx = 0; (pcId[ui32Idx] != '\0') && (g_ui32NbLen < 15); ui32Idx++)
    {
        g_pcNbName[g_ui32NbLen++] = NbUpper(pcId[ui32Idx]);
    }
    g_pcNbName[g_ui32NbLen] = '\0';

    // Nothing to serve if the client ID is empty.
    if(g_ui32NbLen == 0)
    {
        return;
    }

    // Open the UDP listener on the NetBIOS name service port.
    pcb = udp_new();
    if(pcb == NULL)
    {
        return;
    }
    udp_recv(pcb, NbReceive, NULL);
    udp_bind(pcb, IP_ADDR_ANY, NBNS_PORT);
}
