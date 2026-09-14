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
#include "buttons.h"
#include "temp_sensor.h"
#include "product_api.h"    // Plan 11: product_poll() (home-auto tick group)

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
// One stored Wi-Fi credential set, used to rank the boot join order (F1).
//
typedef struct
{
    char pcSsid[WIFI_SSID_MAX + 1];
    char pcPass[WIFI_PASS_MAX + 1];
}
tWifiCand;

//*****************************************************************************
//
// wifi_rank_candidates - load the stored credential slots and return, in
// pCand[0..N-1], the networks to try at boot in preference order (N = return).
//
// Whenever at least one slot is stored, refresh the scan cache once first.  With
// BOTH slots stored (dual credentials) the scan also orders the two present-in-
// scan first, then by RSSI (strongest first), so the device joins the best
// currently-reachable of its known networks and cascades to the other if that
// fails.  With ONE slot the scan does not affect the join (the single credential
// is returned as-is) but still populates the Settings-page "Detected Networks"
// dropdown so a backup can be picked from the list later.  With none, return 0
// (no scan here) and the caller scans + brings up the setup AP.  NetWifiWaitReady()
// gates the scan so it does not race the cold-boot NWP init.
//
//*****************************************************************************
static int
wifi_rank_candidates(tWifiCand *pCand)
{
    tWifiCand sSlot[WIFI_STORE_SLOTS];
    bool      bValid[WIFI_STORE_SLOTS];
    int8_t    i8Rssi[WIFI_STORE_SLOTS];
    int       iValid = 0;
    int       i;

    for(i = 0; i < WIFI_STORE_SLOTS; i++)
    {
        bValid[i] = WifiStoreLoad(i, sSlot[i].pcSsid, sSlot[i].pcPass);
        if(bValid[i])
        {
            iValid++;
        }
    }

    //
    // Nothing stored: the caller scans and brings up the setup AP.
    //
    if(iValid == 0)
    {
        return(0);
    }

    //
    // At least one stored: refresh the scan cache before joining.  With two
    // networks this decides the join order below; with one it is run purely to
    // populate the Settings-page "Detected Networks" dropdown for adding a backup
    // later.  Gate on the NWP settle so the scan does not race the cold-boot init.
    //
    NetWifiWaitReady();
    NetWifiScanCache();

    //
    // One stored: return it directly (the scan above was only to fill the dropdown).
    //
    if(iValid == 1)
    {
        for(i = 0; i < WIFI_STORE_SLOTS; i++)
        {
            if(bValid[i])
            {
                pCand[0] = sSlot[i];
                return(1);
            }
        }
    }

    //
    // Two stored: score each by the RSSI of its match in the cache (absent
    // networks get a floor below any real RSSI so they sort last).
    //
    for(i = 0; i < WIFI_STORE_SLOTS; i++)
    {
        int iCount = NetWifiScanCount();
        int j;

        i8Rssi[i] = -128;                       // absent floor
        for(j = 0; j < iCount; j++)
        {
            char   pcScan[WIFI_SSID_MAX + 1];
            int8_t i8;

            if(NetWifiScanGet(j, pcScan, (int)sizeof(pcScan), &i8) &&
               (strcmp(pcScan, sSlot[i].pcSsid) == 0))
            {
                i8Rssi[i] = i8;
                break;
            }
        }
    }

    //
    // Emit strongest first (only two slots, so a single compare orders them).
    //
    if(i8Rssi[1] > i8Rssi[0])
    {
        pCand[0] = sSlot[1];
        pCand[1] = sSlot[0];
    }
    else
    {
        pCand[0] = sSlot[0];
        pCand[1] = sSlot[1];
    }
    return(WIFI_STORE_SLOTS);
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
    bool     bMQTTStarted = false;
    bool     bTrialChecked = false;
    char     pcSsid[WIFI_SSID_MAX + 1];
    char     pcPass[WIFI_PASS_MAX + 1];
    tWifiCand  pCand[WIFI_STORE_SLOTS];
    int        iNumCand;
    int        iJoined = -1;       // index of the candidate that acquired an IP, or -1
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
    // Rank the stored credentials (F1): with any saved network the scan cache is
    // refreshed (also feeding the Settings "Detected Networks" dropdown); with two
    // saved networks they are ordered present-first / strongest-RSSI so we join the
    // best reachable and cascade to the other; with none, iNumCand == 0.  A
    // compile-time wifi_credentials.h seeds a single candidate when nothing stored.
    //
    iNumCand = wifi_rank_candidates(pCand);
#if defined(WIFI_SSID) && defined(WIFI_PASS)
    if(iNumCand == 0)
    {
        strncpy(pCand[0].pcSsid, WIFI_SSID, WIFI_SSID_MAX);
        pCand[0].pcSsid[WIFI_SSID_MAX] = '\0';
        strncpy(pCand[0].pcPass, WIFI_PASS, WIFI_PASS_MAX);
        pCand[0].pcPass[WIFI_PASS_MAX] = '\0';
        iNumCand = 1;
        PalLog("wifi: using compile-time dev credentials for '%s'\n",
               pCand[0].pcSsid);
    }
#endif

    if(iNumCand > 0)
    {
        int iCand;

        //
        // Let the NWP finish its CME station-flow init before the first connect:
        // issuing it immediately after NetWifiDriverStart races that init on a
        // cold boot and the first attempt is deauthed (reason 15).  No-op if the
        // two-candidate ranking scan above already waited.
        //
        NetWifiWaitReady();

        //
        // Try each ranked candidate in turn; DHCP starts on link-up.  Wait for the
        // lease, re-issuing the association up to WIFI_MAX_ATTEMPTS times, and drop
        // the STA role before cascading to the next network.  If none join, fall
        // back to the setup AP so the device can be re-provisioned without JTAG.
        //
        for(iCand = 0; (iCand < iNumCand) && (iJoined < 0); iCand++)
        {
            uint32_t ui32Attempt;

            PalLog("wifi: joining '%s' (candidate %d/%d)\n",
                   pCand[iCand].pcSsid, iCand + 1, iNumCand);
            NetWifiStaUp(pCand[iCand].pcSsid, pCand[iCand].pcPass);

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
                NetWifiReconnect(pCand[iCand].pcSsid, pCand[iCand].pcPass);
            }

            if(NetWifiIsIpAcquired())
            {
                iJoined = iCand;
            }
            else if((iCand + 1) < iNumCand)
            {
                NetWifiStaDown();       // drop before trying the next candidate
            }
        }
    }

    if(iJoined >= 0)
    {
        //
        // Adopt the joined network as the active credentials the runtime
        // reconnect / no-IP-fallback paths below operate on.
        //
        strncpy(pcSsid, pCand[iJoined].pcSsid, WIFI_SSID_MAX);
        pcSsid[WIFI_SSID_MAX] = '\0';
        strncpy(pcPass, pCand[iJoined].pcPass, WIFI_PASS_MAX);
        pcPass[WIFI_PASS_MAX] = '\0';
    }
    else
    {
        pcSsid[0] = '\0';
        pcPass[0] = '\0';
        PalLog("wifi: no known network joined; starting setup AP\n");
        NetWifiScanCache();         // scan (STA role) + drop STA, ready for AP
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
    // Bring up the home-auto product: field-I/O chains (relays power up off,
    // outputs enabled), per-output modes + shutter table, and the input-scan /
    // binding engine.
    //
    product_init();
    ButtonsInit();          // on-board SW1/SW2, exposed as inputs after the chain
    TempSensorInit();       // on-board TMP1075 (bit-banged I2C on GPIO10/11)
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
        // flushes first, then act - a warm reboot would wedge the NWP.  Slot 0
        // (primary) is saved AND switched to live, adopting it as the active
        // credentials the background reconnect uses; slot 1 (backup) is saved only
        // and the current link is left untouched.  A forget returns to the setup AP.
        //
        {
            char pcNewSsid[WIFI_SSID_MAX + 1];
            char pcNewPass[WIFI_PASS_MAX + 1];
            int  iSlot = 0;

            if(WebUIWifiProvisionPending(&iSlot, pcNewSsid, (int)sizeof(pcNewSsid),
                                         pcNewPass, (int)sizeof(pcNewPass)))
            {
                vTaskDelay(pdMS_TO_TICKS(300));
                WifiStoreSave(iSlot, pcNewSsid, pcNewPass);

                if(iSlot == 0)
                {
                    //
                    // Primary: adopt as the active credentials and switch live.
                    //
                    strncpy(pcSsid, pcNewSsid, WIFI_SSID_MAX);
                    pcSsid[WIFI_SSID_MAX] = '\0';
                    strncpy(pcPass, pcNewPass, WIFI_PASS_MAX);
                    pcPass[WIFI_PASS_MAX] = '\0';
                    PalLog("wifi: provisioning primary '%s', switching to station\n",
                           pcSsid);
                    NetWifiSwitchToSta(pcSsid, pcPass);
                    bMQTTStarted = false; // (re)start MQTT once the new link has an IP
                    ui32RetryMs = 0;
                    bStaHadIp = false;    // arm the no-IP AP fallback for the new creds
                    bNoIpTiming = false;
                }
                else
                {
                    //
                    // Backup: saved for boot-time ranking; the running link (or the
                    // setup AP) is left as-is.
                    //
                    PalLog("wifi: saved backup network '%s' (slot 1); link unchanged\n",
                           pcNewSsid);
                }
            }
            else if(WebUIWifiForgetPending())
            {
                vTaskDelay(pdMS_TO_TICKS(300));
                WifiStoreClear(0);
                WifiStoreClear(1);
                pcSsid[0] = '\0';
                pcPass[0] = '\0';
                PalLog("wifi: credentials forgotten (both slots), starting setup AP\n");
                NetWifiScanCache();     // scan (STA role) + drop STA, ready for AP
                NetWifiApUp();
                bMQTTStarted = false;
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
            /* setup AP active - nothing to retry */
        }
        else if(NetWifiIsIpAcquired())
        {
            bStaHadIp = true;
            bNoIpTiming = false;
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
        product_poll(SYSTICKMS);
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
