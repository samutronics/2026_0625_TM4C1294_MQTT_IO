//*****************************************************************************
//
// main.c (CC35x1) - application entry for the MQTT-IO gateway on LP-EM-CC35X1.
//
// main_freertos.c (from the SDK) owns C startup: it runs Board_init(), creates
// one detached pthread running mainThread(), and starts the FreeRTOS scheduler.
// This file provides that mainThread() seam - the CC35x1 analogue of the TM4C
// enet_io.c main(): bring up Wi-Fi STA -> lwIP -> DHCP, load config, start the
// web/name/time services and the field-I/O chains, init MQTT, then run the
// periodic ~10 ms application tick.
//
// Threading (NO_SYS=0, LWIP_TCPIP_CORE_LOCKING=1): raw lwIP entry points must be
// called with the core lock held.  The one-shot inits (httpd_init, NetbiosnsInit,
// SntpInit) and the net-touching tick calls (SntpTick, MQTTAppTick) are wrapped
// in LOCK_TCPIP_CORE()/UNLOCK_TCPIP_CORE() here.  (mqtt_client.c also brackets
// its own critical sections with the pal_irq seam; fully marshalling the MQTT
// raw-API onto tcpip_thread is the deferred threading-hardening step.)
//
// The input/relay scan + binding glue (DINChainScan, RelayFaultScan,
// ApplyBindings, the click-event callback) now lives in the shared
// common/io_scan.c; this tick drives it through IOScanInit()/IOScanTick(),
// giving both platforms identical field-I/O behaviour.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* RTOS */
#include <FreeRTOS.h>
#include <task.h>

/* lwIP */
#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/apps/httpd.h"

/* Platform */
#include "net_wifi.h"
#include "wifi_store.h"
#include "pal_log.h"

/* Portable application layer (shared with the TM4C build) */
#include "config.h"
#include "din_chain.h"
#include "relay_chain.h"
#include "mqtt_app.h"
#include "output_ctrl.h"
#include "relay_pulse.h"
#include "input_events.h"
#include "io_scan.h"
#include "netbiosns.h"
#include "sntp_client.h"
#include "webui.h"
#include "webui_platform.h"

//
// Optional compile-time Wi-Fi station credentials for bench/dev use.  If a local
// (git-ignored) wifi_credentials.h exists and defines WIFI_SSID / WIFI_PASS, they
// seed the credentials store on first boot so a freshly-flashed dev board joins
// immediately.  In the field the credentials come from SoftAP provisioning (see
// wifi_store), so the header is entirely optional: a fresh checkout without it
// simply boots into the "MQTT-IO-Setup" access point for provisioning.
//
#if defined(__has_include)
#  if __has_include("wifi_credentials.h")
#    include "wifi_credentials.h"
#  endif
#endif

//
// Application tick period (matches the TM4C SYSTICKMS = 1000/SYSTICKHZ).
//
#define SYSTICKMS       10U

//
// Wi-Fi association retry policy.  A single Wlan_Connect can be deauthed
// mid-handshake, so we retry a few times during bring-up and then keep retrying
// in the background from the tick loop until DHCP completes.
//
#define IP_POLL_MS          100U    // DHCP-acquired poll granularity
#define WIFI_ATTEMPT_MS   12000U    // per-attempt wait for a DHCP lease
#define WIFI_MAX_ATTEMPTS     3U    // association attempts during bring-up
#define WIFI_RETRY_MS     15000U    // background reconnect interval (no IP yet)
#define WIFI_FALLBACK_MS  45000U    // no-IP time in STA before falling back to AP

//
// Periodic liveness heartbeat.  The app otherwise logs only on state changes,
// so the serial backchannel goes quiet after boot and looks dead.  A heartbeat
// every HEARTBEAT_MS proves the tick is running and surfaces link state (IP,
// MQTT, connect/disconnect churn + last 802.11 disconnect reason) live.
//
#define HEARTBEAT_MS      10000U

//*****************************************************************************
//
// mainThread - application task entry (invoked by main_freertos.c).
//
//*****************************************************************************
void *
mainThread(void *pvArg0)
{
    uint8_t  pui8MAC[6];
    uint32_t ui32Waited;
    uint32_t ui32RetryMs = 0;
    uint32_t ui32UptimeMs = 0;
    uint32_t ui32HeartbeatMs = 0;
    bool     bMQTTStarted = false;
    bool     bTrialChecked = false;
    char     pcSsid[WIFI_SSID_MAX + 1];
    char     pcPass[WIFI_PASS_MAX + 1];
    bool     bHaveCreds;
    bool     bStaHadIp = false;    // a working STA link has been seen this session
    uint32_t ui32NoIpMs = 0;       // time in STA with no IP (drives AP fallback)

    (void)pvArg0;

    //
    // Bring up the lwIP TCP/IP thread.
    //
    NetWifiInit();

    //
    // Load persistent configuration (NVS/NVOCMP via pal_storage).
    //
    ConfigInit();

    //
    // Initialise PSA Firmware Update (reads the boot report, sets component
    // states, caches the OTA staging-size cap).  Must run before we query FWU
    // state or serve the web UI's OTA handler.
    //
    WebPlatformOtaInit();

    //
    // Start the NWP, then bring up Wi-Fi.  Credentials come from the persistent
    // store (filled by SoftAP provisioning); a compile-time wifi_credentials.h,
    // if present, seeds them once for bench/dev use.  With no credentials the
    // device brings up the open "MQTT-IO-Setup" AP so the user can provision it.
    //
    NetWifiDriverStart();

    bHaveCreds = WifiStoreLoad(pcSsid, pcPass);
#if defined(WIFI_SSID) && defined(WIFI_PASS)
    if(!bHaveCreds)
    {
        strncpy(pcSsid, WIFI_SSID, WIFI_SSID_MAX); pcSsid[WIFI_SSID_MAX] = '\0';
        strncpy(pcPass, WIFI_PASS, WIFI_PASS_MAX); pcPass[WIFI_PASS_MAX] = '\0';
        bHaveCreds = true;
        PalLog("wifi: using compile-time dev credentials for '%s'\n", pcSsid);
    }
#endif

    if(bHaveCreds)
    {
        uint32_t ui32Attempt;

        //
        // Join as a station; DHCP starts on link-up.  Wait for the lease,
        // re-issuing the association up to WIFI_MAX_ATTEMPTS times.  If it never
        // completes (wrong password, AP gone, device moved), fall back to the
        // setup AP so it can be re-provisioned without JTAG.
        //
        NetWifiStaUp(pcSsid, pcPass);

        for(ui32Attempt = 1U; ui32Attempt <= WIFI_MAX_ATTEMPTS; ui32Attempt++)
        {
            for(ui32Waited = 0;
                !NetWifiIsIpAcquired() && (ui32Waited < WIFI_ATTEMPT_MS);
                ui32Waited += IP_POLL_MS)
            {
                vTaskDelay(pdMS_TO_TICKS(IP_POLL_MS));
            }
            if(NetWifiIsIpAcquired())
            {
                break;
            }
            PalLog("net: no IP after attempt %u/%u, reconnecting\n",
                   (unsigned)ui32Attempt, (unsigned)WIFI_MAX_ATTEMPTS);
            NetWifiReconnect(pcSsid, pcPass);
        }

        if(!NetWifiIsIpAcquired())
        {
            PalLog("wifi: could not join '%s'; starting setup AP\n", pcSsid);
            NetWifiStaDown();
            NetWifiApUp();
        }
    }
    else
    {
        PalLog("wifi: no stored credentials; starting setup AP\n");
        NetWifiApUp();
    }

    //
    // MAC seeds the Home Assistant device id (mirrors the TM4C USER0/1 MAC).
    //
    NetWifiGetMac(pui8MAC);

    //
    // Web server, NetBIOS name responder, and SNTP client (all raw lwIP -> hold
    // the core lock).  CGI/SSI handler registration is deferred to the handler
    // port; httpd here serves the static fs/ image.
    //
    LOCK_TCPIP_CORE();
    httpd_init();
    WebUIRegister();          // SSI + CGI handlers (shared with the TM4C build)
    NetbiosnsInit();
    SntpInit();
    UNLOCK_TCPIP_CORE();

    //
    // Field-I/O chains: relays power up off with outputs enabled.
    //
    DINChainInit(ConfigGetDinDevices());
    RelayChainInit(ConfigGetRelayDevices());
    OutputCtrlReload();
    PalLog("io: %u input dev, %u relay dev\n",
           ConfigGetDinDevices(), ConfigGetRelayDevices());

    //
    // MQTT client subsystem; start publishing once we have an IP.
    //
    MQTTAppInit(pui8MAC);
    if(NetWifiIsIpAcquired())
    {
        MQTTAppStart();
        bMQTTStarted = true;
    }

    //
    // Wire the input-scan / binding engine to the click detector.
    //
    IOScanInit();

    //
    // Periodic application tick.
    //
    for(;;)
    {
        vTaskDelay(pdMS_TO_TICKS(SYSTICKMS));
        ui32UptimeMs += SYSTICKMS;

        //
        // Keep the web UI's "ipaddr" SSI tag current (0.0.0.0 before DHCP).
        //
        g_ui32IPAddress = NetWifiGetIp4();

        //
        // Periodic liveness heartbeat over the serial backchannel: uptime, the
        // current IP (0.0.0.0 until DHCP completes), whether MQTT has started,
        // and the Wi-Fi connect/disconnect churn + last 802.11 disconnect reason
        // (reason 15 = handshake timeout).  This is the reliable "is it alive?"
        // signal since every other log is edge-triggered.
        //
        ui32HeartbeatMs += SYSTICKMS;
        if(ui32HeartbeatMs >= HEARTBEAT_MS)
        {
            char pcIp[16];

            ui32HeartbeatMs = 0;
            NetWifiGetIp(pcIp, (int)sizeof(pcIp));
            PalLog("hb: up %us ip %s mqtt %d conn %d disc %d rsn %d\n",
                   (unsigned)(ui32UptimeMs / 1000U), pcIp, (int)bMQTTStarted,
                   g_iNetConnects, g_iNetDisconnects, g_iLastDiscReason);
        }

        //
        // Apply a deferred MQTT start once DHCP completes after boot.
        //
        if(!bMQTTStarted && NetWifiIsIpAcquired())
        {
            MQTTAppStart();
            bMQTTStarted = true;
        }

        //
        // OTA trial gate: the first time we reach a healthy state (Wi-Fi up +
        // an IP), commit a firmware that was just installed and is running in
        // TRIAL.  A bad OTA that never gets here is rolled back to the previous
        // slot by the bootloader on the next power-cycle.  No-op when not in
        // trial (the normal case); may reboot once to finalise when it is.
        //
        if(!bTrialChecked && NetWifiIsIpAcquired())
        {
            bTrialChecked = true;
            WebPlatformOtaTrialAccept();
        }

        //
        // Service web-UI requests raised by the CGI handlers (which run on the
        // tcpip_thread and only set a flag).
        //
        if(WebUIResetPending())
        {
            //
            // Let the HTTP response flush, then reset.  WebPlatformFinalizeReboot
            // reboots through PSA FWU when an OTA image was just staged (so the
            // bootloader swaps to the new slot), otherwise a plain reset for
            // /reboot.cgi / factory-reset.  Does not return.
            //
            vTaskDelay(pdMS_TO_TICKS(300));
            WebPlatformFinalizeReboot();
        }

        if(WebUIMqttApplyPending())
        {
            //
            // A web config change.  Re-apply device counts / output modes (only
            // re-init the relay chain when its count actually changed, so an
            // unrelated save does not switch relays off), then (re)connect MQTT
            // with the new broker settings.  Mirrors the TM4C apply path.
            //
            DINChainSetDevices(ConfigGetDinDevices());
            if(RelayChainGetDevices() != ConfigGetRelayDevices())
            {
                RelayChainSetDevices(ConfigGetRelayDevices());
            }
            OutputCtrlReload();
            MQTTAppStart();
            bMQTTStarted = true;
        }

        if(WebUIMqttRepublishPending())
        {
            MQTTAppRepublish();
        }

        //
        // Wi-Fi provisioning requests from the setup page (the CGIs run on the
        // tcpip_thread and only set a flag).  Apply here so the HTTP response
        // flushes first, then switch role live - a warm reboot would wedge the
        // NWP.  A provision updates the active credentials (pcSsid/pcPass) so the
        // background reconnect below uses them; a forget returns to the setup AP.
        //
        if(WebUIWifiProvisionPending(pcSsid, (int)sizeof(pcSsid),
                                     pcPass, (int)sizeof(pcPass)))
        {
            vTaskDelay(pdMS_TO_TICKS(300));
            WifiStoreSave(pcSsid, pcPass);
            PalLog("wifi: provisioning '%s', switching to station\n", pcSsid);
            NetWifiSwitchToSta(pcSsid, pcPass);
            bMQTTStarted = false;   // (re)start MQTT once the new link has an IP
            ui32RetryMs = 0;
            bStaHadIp = false;      // arm the no-IP AP fallback for the new creds
            ui32NoIpMs = 0;
        }
        else if(WebUIWifiForgetPending())
        {
            vTaskDelay(pdMS_TO_TICKS(300));
            WifiStoreClear();
            pcSsid[0] = '\0';
            pcPass[0] = '\0';
            PalLog("wifi: credentials forgotten, starting setup AP\n");
            NetWifiStaDown();
            NetWifiApUp();
            bMQTTStarted = false;
        }

        //
        // Station link maintenance.  In the setup AP there is nothing to retry.
        // In station mode: on an IP, remember we have had a working link this
        // session; with no IP, keep re-issuing the association, and - if we have
        // NEVER acquired an IP with the current credentials (a bad password or
        // wrong network entered at provisioning) - fall back to the setup AP after
        // WIFI_FALLBACK_MS so it can be re-provisioned live, no power-cycle.  A
        // link that has worked before is left to reconnect rather than dropping to
        // AP on a transient outage.
        //
        if(NetWifiIsAp())
        {
            /* setup AP active - nothing to retry */
        }
        else if(NetWifiIsIpAcquired())
        {
            bStaHadIp = true;
            ui32NoIpMs = 0;
        }
        else
        {
            ui32RetryMs += SYSTICKMS;
            if(ui32RetryMs >= WIFI_RETRY_MS)
            {
                ui32RetryMs = 0;
                NetWifiReconnect(pcSsid, pcPass);
            }

            if(!bStaHadIp)
            {
                ui32NoIpMs += SYSTICKMS;
                if(ui32NoIpMs >= WIFI_FALLBACK_MS)
                {
                    ui32NoIpMs = 0;
                    PalLog("wifi: no IP for %us, falling back to setup AP\n",
                           (unsigned)(WIFI_FALLBACK_MS / 1000U));
                    NetWifiStaDown();
                    NetWifiApUp();
                    bMQTTStarted = false;
                }
            }
        }

        //
        // Pure-logic / GPIO timers (no lwIP) run outside the core lock.
        //
        // Poll the input chain and relay fault line: publish changes over MQTT,
        // run bindings, log transitions.  Bit-bangs GPIO only (no lwIP), so it
        // stays outside the core lock alongside the other GPIO timers.  (The
        // relay-actuation CGI handlers bit-bang the same chains on tcpip_thread;
        // that cross-thread GPIO race is the deferred concurrency-hardening step.)
        //
        IOScanTick();
        InputEventsTick(SYSTICKMS);
        RelayPulseTick(SYSTICKMS);
        OutputCtrlTick(SYSTICKMS);

        //
        // Net-touching timers (SNTP UDP, MQTT keep-alive/reconnect) under lock.
        //
        LOCK_TCPIP_CORE();
        SntpTick(SYSTICKMS);
        MQTTAppTick(SYSTICKMS);
        UNLOCK_TCPIP_CORE();
    }
}
