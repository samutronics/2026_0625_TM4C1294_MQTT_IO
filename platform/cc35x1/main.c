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
#include "pal_sys.h"

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
#include "buttons.h"
#include "temp_sensor.h"

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

//
// On-board temperature sensor: poll the TMP1075 and publish over MQTT this often.
//
#define TEMP_PUBLISH_MS   5000U

//
// AP-mode fallback reboot watchdog: if we fell to the provisioning AP because
// the join failed (creds exist but router is down), reboot after 5 minutes to
// retry the preset Wi-Fi.
//
#define AP_REBOOT_MS      (5U*60U*1000U)

//
// Wi-Fi candidate for ranked join: both configured slots plus scan results.
//
typedef struct
{
    char    ssid[WIFI_SSID_MAX + 1];
    char    pass[WIFI_PASS_MAX + 1];
    int8_t  rssi;
    bool    present;
    bool    valid;
}
tWifiCand;

//*****************************************************************************
//
// wifi_rank_candidates - load credentials from both slots and rank by RSSI.
//
// Returns the number of valid (stored) candidates.  If <= 1, no scan is needed
// (preserve fast single-cred boot).  If == 2, scans for RSSI and sorts
// present-first, then RSSI descending (slot order tiebreak).
//
// The ranking fills pcSsid/pcPass (output) with the top candidate, and bHaveCreds
// is set false if validCount == 0.
//
//*****************************************************************************
static int
wifi_rank_candidates(char *pcSsid, char *pcPass, bool *pbHaveCreds)
{
    tWifiCand   aCand[WIFI_STORE_SLOTS];
    int         nValid = 0;
    uint32_t    ui32ScanCount = 0;
    int         iTop;
    int         i;
    int         j;
    char        pcScannedSsid[WIFI_SSID_MAX + 1];
    int8_t      i8Rssi;
    bool        bTopPresent;
    bool        bCandPresent;

    memset(aCand, 0, sizeof(aCand));

    for(i = 0; i < WIFI_STORE_SLOTS; i++)
    {
        if(WifiStoreLoad(i, aCand[i].ssid, aCand[i].pass))
        {
            aCand[i].valid = true;
            aCand[i].rssi = -100;
            aCand[i].present = false;
            nValid++;
        }
    }

    if(nValid == 0)
    {
        *pbHaveCreds = false;
        pcSsid[0] = '\0';
        pcPass[0] = '\0';
        return(0);
    }

    if(nValid <= 1)
    {
        *pbHaveCreds = true;
        if(aCand[0].valid)
        {
            strncpy(pcSsid, aCand[0].ssid, WIFI_SSID_MAX);
            pcSsid[WIFI_SSID_MAX] = '\0';
            strncpy(pcPass, aCand[0].pass, WIFI_PASS_MAX);
            pcPass[WIFI_PASS_MAX] = '\0';
        }
        return(nValid);
    }

    NetWifiScanCache();
    ui32ScanCount = NetWifiScanCount();

    for(i = 0; i < WIFI_STORE_SLOTS; i++)
    {
        if(!aCand[i].valid) continue;
        for(j = 0; j < (int)ui32ScanCount; j++)
        {
            if(NetWifiScanGet(j, pcScannedSsid, (int)(WIFI_SSID_MAX + 1), &i8Rssi) &&
               (strcmp(pcScannedSsid, aCand[i].ssid) == 0))
            {
                aCand[i].present = true;
                aCand[i].rssi = i8Rssi;
                break;
            }
        }
    }

    iTop = 0;
    for(i = 1; i < WIFI_STORE_SLOTS; i++)
    {
        if(!aCand[i].valid) continue;
        bTopPresent = aCand[iTop].present;
        bCandPresent = aCand[i].present;
        if(bCandPresent && !bTopPresent)
        {
            iTop = i;
        }
        else if(bCandPresent && bTopPresent && (aCand[i].rssi > aCand[iTop].rssi))
        {
            iTop = i;
        }
    }

    strncpy(pcSsid, aCand[iTop].ssid, WIFI_SSID_MAX);
    pcSsid[WIFI_SSID_MAX] = '\0';
    strncpy(pcPass, aCand[iTop].pass, WIFI_PASS_MAX);
    pcPass[WIFI_PASS_MAX] = '\0';

    return(nValid);
}

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
    uint32_t ui32TempMs = 0;
    uint32_t ui32ApRebootMs = 0;  // AP fallback watchdog accumulator
    bool     bMQTTStarted = false;
    bool     bTrialChecked = false;
    char     pcSsid[WIFI_SSID_MAX + 1];
    char     pcPass[WIFI_PASS_MAX + 1];
    bool     bHaveCreds;
    bool       bStaHadIp = false;  // a working STA link has been seen this session
    bool       bNoIpTiming = false; // the no-IP -> AP fallback timer is running
    TickType_t xNoIpStart = 0;     // wall-clock tick when the no-IP wait began

    (void)pvArg0;

    PalLog("\n\n[BOOT] CC35x1 MQTT-IO Firmware built %s %s\n\n", __DATE__, __TIME__);

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

    //
    // Load and rank credentials from both slots; prefer the stored SSID with the
    // strongest RSSI.  Try candidates in ranked order; if all fail, fall to AP.
    // If no stored credentials exist but compile-time credentials are available,
    // use those as a fallback (dev/bench use only).
    //
    {
        int nCandidates = wifi_rank_candidates(pcSsid, pcPass, &bHaveCreds);

#if defined(WIFI_SSID) && defined(WIFI_PASS)
        if(nCandidates == 0)
        {
            strncpy(pcSsid, WIFI_SSID, WIFI_SSID_MAX); pcSsid[WIFI_SSID_MAX] = '\0';
            strncpy(pcPass, WIFI_PASS, WIFI_PASS_MAX); pcPass[WIFI_PASS_MAX] = '\0';
            bHaveCreds = true;
            nCandidates = 1;
            PalLog("wifi: using compile-time dev credentials for '%s'\n", pcSsid);
        }
#endif

        if(nCandidates > 0)
        {
            uint32_t ui32Attempt;

            //
            // Try candidates in ranked order; for each, attempt to join as a
            // station up to WIFI_MAX_ATTEMPTS times with DHCP polling.  If all
            // candidates fail, fall back to provisioning AP.
            //
            for(int iCand = 0; iCand < nCandidates; iCand++)
            {
                char pcCandSsid[WIFI_SSID_MAX + 1];
                char pcCandPass[WIFI_PASS_MAX + 1];

                if(iCand == 0)
                {
                    strncpy(pcCandSsid, pcSsid, WIFI_SSID_MAX);
                    pcCandSsid[WIFI_SSID_MAX] = '\0';
                    strncpy(pcCandPass, pcPass, WIFI_PASS_MAX);
                    pcCandPass[WIFI_PASS_MAX] = '\0';
                }
                else
                {
                    if(!WifiStoreLoad(iCand, pcCandSsid, pcCandPass))
                    {
                        continue;
                    }
                }

                NetWifiStaUp(pcCandSsid, pcCandPass);

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
                    NetWifiReconnect(pcCandSsid, pcCandPass);
                }

                if(NetWifiIsIpAcquired())
                {
                    strncpy(pcSsid, pcCandSsid, WIFI_SSID_MAX);
                    pcSsid[WIFI_SSID_MAX] = '\0';
                    strncpy(pcPass, pcCandPass, WIFI_PASS_MAX);
                    pcPass[WIFI_PASS_MAX] = '\0';
                    break;
                }

                PalLog("wifi: could not join '%s'; trying next candidate\n", pcCandSsid);
                NetWifiStaDown();
            }

            if(!NetWifiIsIpAcquired())
            {
                PalLog("wifi: all candidates failed; starting setup AP\n");
                NetWifiScanCache();     // scan (STA role) + drop STA, ready for AP
                NetWifiApUp();
            }
        }
        else
        {
            PalLog("wifi: no stored credentials; starting setup AP\n");
            NetWifiScanCache();         // scan (STA role) + drop STA, ready for AP
            NetWifiApUp();
        }
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
    ButtonsInit();          // on-board SW1/SW2, exposed as inputs after the chain
    TempSensorInit();       // on-board TMP1075 (bit-banged I2C on GPIO10/11)
    OutputCtrlReload();
    PalLog("io: %u input dev, %u relay dev, %d local btn\n",
           ConfigGetDinDevices(), ConfigGetRelayDevices(),
           WebPlatformLocalInputCount());

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
            char    pcIp[16];
            int32_t i32Temp = 0;
            bool    bTemp = TempSensorGet(&i32Temp);

            ui32HeartbeatMs = 0;
            NetWifiGetIp(pcIp, (int)sizeof(pcIp));
            PalLog("hb: up %us ip %s mqtt %d conn %d disc %d rsn %d temp %d.%02dC\n",
                   (unsigned)(ui32UptimeMs / 1000U), pcIp, (int)bMQTTStarted,
                   g_iNetConnects, g_iNetDisconnects, g_iLastDiscReason,
                   bTemp ? (int)(i32Temp / 100) : 0,
                   bTemp ? (int)((i32Temp < 0 ? -i32Temp : i32Temp) % 100) : 0);
        }

        //
        // Apply a deferred MQTT start once DHCP completes after boot.
        //
        if(!bMQTTStarted && NetWifiIsIpAcquired())
        {
            LOCK_TCPIP_CORE();
            MQTTAppStart();
            UNLOCK_TCPIP_CORE();
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
            LOCK_TCPIP_CORE();
            MQTTAppStart();
            UNLOCK_TCPIP_CORE();
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
        {
            int iSlot = 0;
            if(WebUIWifiProvisionPending(pcSsid, (int)sizeof(pcSsid),
                                         pcPass, (int)sizeof(pcPass), &iSlot))
            {
                vTaskDelay(pdMS_TO_TICKS(300));
                WifiStoreSave(iSlot, pcSsid, pcPass);
                PalLog("wifi: provisioning '%s', switching to station\n", pcSsid);
                NetWifiSwitchToSta(pcSsid, pcPass);
                bMQTTStarted = false;   // (re)start MQTT once the new link has an IP
                ui32RetryMs = 0;
                ui32ApRebootMs = 0;     // reset AP watchdog when provisioning
                bStaHadIp = false;      // arm the no-IP AP fallback for the new creds
                bNoIpTiming = false;
            }
            else if(WebUIWifiForgetPending())
            {
                vTaskDelay(pdMS_TO_TICKS(300));
                WifiStoreClear(0);
                pcSsid[0] = '\0';
                pcPass[0] = '\0';
                bHaveCreds = false;     // intentional setup AP - never auto-reboot
                ui32ApRebootMs = 0;     // reset AP watchdog
                PalLog("wifi: credentials forgotten, starting setup AP\n");
                NetWifiScanCache();     // scan (STA role) + drop STA, ready for AP
                NetWifiApUp();
                bMQTTStarted = false;
            }
            else if(WebUIWifiScanPending())
            {
                // User clicked "Scan Networks" button. Refresh the cached network list.
                // NetWifiScanCache() brings STA up, scans, and tears STA down.
                vTaskDelay(pdMS_TO_TICKS(300));
                bool bWasAp = NetWifiIsAp();
                if(bWasAp)
                {
                    PalLog("wifi: user scan requested (provisioning AP mode)\n");
                }
                else
                {
                    PalLog("wifi: user scan requested (STA mode - will disconnect/reconnect during scan)\n");
                }
                NetWifiScanCache();

                // After scan, if we were in STA mode before, re-establish the connection
                // with the saved credentials (NetWifiScanCache tore down the STA role).
                if(!bWasAp && pcSsid[0] != '\0')
                {
                    vTaskDelay(pdMS_TO_TICKS(300));
                    PalLog("wifi: reconnecting to '%s' after scan\n", pcSsid);
                    NetWifiStaUp(pcSsid, pcPass);
                    bMQTTStarted = false;   // re-trigger MQTT once IP is acquired
                    ui32RetryMs = 0;        // reset retry timer
                    bStaHadIp = false;      // arm the no-IP AP fallback
                    bNoIpTiming = false;
                }
            }
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
            if(bHaveCreds)
            {
                ui32ApRebootMs += SYSTICKMS;
                if(ui32ApRebootMs >= AP_REBOOT_MS)
                {
                    PalLog("wifi: AP fallback 5 min, rebooting to retry preset Wi-Fi\n");
                    vTaskDelay(pdMS_TO_TICKS(50));
                    PalReboot();
                }
            }
            else
            {
                ui32ApRebootMs = 0;
            }
        }
        else if(NetWifiIsIpAcquired())
        {
            bStaHadIp = true;
            bNoIpTiming = false;
            ui32ApRebootMs = 0;         // reset AP watchdog on IP acquired
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
                //
                // Time the no-IP interval in wall-clock ticks, not accumulated
                // SYSTICKMS: a blocking Wlan retry can stall this loop so the
                // tick-count underruns real time and the fallback fires minutes
                // late.  xTaskGetTickCount() reflects true elapsed time and its
                // unsigned subtraction is wrap-safe.
                //
                TickType_t xNow = xTaskGetTickCount();

                if(!bNoIpTiming)
                {
                    bNoIpTiming = true;
                    xNoIpStart = xNow;
                }
                else if((xNow - xNoIpStart) >= pdMS_TO_TICKS(WIFI_FALLBACK_MS))
                {
                    bNoIpTiming = false;
                    PalLog("wifi: no IP for %us, falling back to setup AP\n",
                           (unsigned)(WIFI_FALLBACK_MS / 1000U));
                    NetWifiScanCache(); // scan (STA role) + drop STA, ready for AP
                    NetWifiApUp();
                    bMQTTStarted = false;
                }
            }
        }

        //
        // Input/relay scan + event + output timers.  These bit-bang GPIO, but
        // on a state change they ALSO publish over MQTT (input events/state in
        // io_scan.c, cover state in output_ctrl.c), which touches lwIP.  With
        // LWIP_TCPIP_CORE_LOCKING every lwIP call must hold the core lock, so
        // this whole group runs inside LOCK/UNLOCK -- otherwise the first input
        // event (e.g. an on-board button press) trips the "Function called
        // without core lock" assertion.  They still run every tick regardless of
        // IP so local relay bindings keep working offline; the publishes simply
        // no-op while MQTT is disconnected.  (The relay-actuation CGI handlers
        // bit-bang the same chains on tcpip_thread -- that cross-thread GPIO
        // race is the deferred concurrency-hardening step.)
        //
        LOCK_TCPIP_CORE();
        IOScanTick();
        InputEventsTick(SYSTICKMS);
        RelayPulseTick(SYSTICKMS);
        OutputCtrlTick(SYSTICKMS);
        UNLOCK_TCPIP_CORE();

        //
        // On-board temperature: every 5 s bit-bang the TMP1075 read (GPIO only,
        // no lwIP -> outside the core lock, like the other GPIO timers).  The MQTT
        // publish of the fresh reading happens below, inside the core lock.
        //
        ui32TempMs += SYSTICKMS;
        bool bTempTick = (ui32TempMs >= TEMP_PUBLISH_MS);
        if(bTempTick)
        {
            ui32TempMs = 0;
            TempSensorPoll();
        }

        //
        // Net-touching timers (SNTP UDP, MQTT keep-alive/reconnect) under lock.
        // Only run them once DHCP has given us a route: without an IP (setup AP,
        // or STA still associating) the MQTT reconnect attempt fails with -13
        // (no route) every tick and spams the log, and SNTP requests cannot be
        // sent either.  MQTT/SNTP resume automatically when the lease arrives.
        //
        if(NetWifiIsIpAcquired())
        {
            LOCK_TCPIP_CORE();
            SntpTick(SYSTICKMS);
            MQTTAppTick(SYSTICKMS);
            if(bTempTick)
            {
                int32_t i32CentiC = 0;
                bool    bValid = TempSensorGet(&i32CentiC);
                MQTTAppPublishTemp(i32CentiC, bValid);
            }
            UNLOCK_TCPIP_CORE();
        }
    }
}
