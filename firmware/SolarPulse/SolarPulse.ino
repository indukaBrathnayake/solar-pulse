/*
 * ============================================================
 *  SolarPulse v4 - ESP32 solar, battery and load controller
 *  JK BMS (BLE, JK02_32S, hw v11) -> Firebase RTDB + local API
 *
 *  ------------------------------------------------------------
 *  v3 -> v4: THE THREE DEFECTS THIS RELEASE FIXES
 *  ------------------------------------------------------------
 *
 *  1. BMS DROPPED THE LINK EVERY NIGHT AROUND MIDNIGHT.
 *
 *     Root cause: parseCellInfo() runs inside the NimBLE notify
 *     callback, i.e. on the BLE host task. v3 called
 *     integrateEnergy() -> rolloverCheck() from there, and at
 *     00:00 rolloverCheck() did a blocking TLS push to Firebase
 *     (1-10 s), a full LittleFS rewrite of daily.csv, a history
 *     directory prune and eleven NVS writes -- all on the BLE
 *     host task. The host could not service the link for several
 *     seconds, the supervision timer expired and the BMS dropped
 *     the connection. TLS also needs several kB of stack that the
 *     NimBLE callback does not have, which is the second way this
 *     showed up.
 *
 *     Fix: the notify callback now ONLY parses bytes into a
 *     struct. Energy integration moved to the control tick, all
 *     network I/O to netTask, all flash I/O to the control task.
 *     Nothing blocking is reachable from a BLE callback.
 *
 *  2. A SILENT BMS WAS NEVER RECOVERED.
 *
 *     Root cause: v3 only reconnected when NimBLE reported a
 *     disconnect. If the ACL link stayed up but the BMS stopped
 *     notifying, bleConnected was still true, bleConnectTask()
 *     returned immediately, and the wake-frame resend was gated
 *     on !bms.lastFrameMs so it only ever fired before the FIRST
 *     frame of a session. The result was a live-but-dead link
 *     that needed a power cycle -- exactly the reported symptom.
 *
 *     Fix: bleTask() supervises DATA, not just the socket. No
 *     frame for BMS_FRAME_TIMEOUT_MS is treated as link failure
 *     and forces a full teardown and reconnect, with exponential
 *     backoff and a periodic keep-alive request.
 *
 *  3. MONTHLY / YEARLY TOTALS WERE WRONG.
 *
 *     Root cause A: pushDaily(closing) named the day being closed
 *     as "now minus one hour". That is only correct if the
 *     rollover is noticed within an hour of midnight. After a
 *     reboot, a WiFi outage or a BLE outage the rollover fires
 *     whenever the code next runs -- so yesterday's totals were
 *     written under TODAY's date and then double counted.
 *     Root cause B: a rollover during a WiFi outage lost the
 *     /daily write completely; there was no retry.
 *
 *     Fix: the day being closed is named from dayStamp, never
 *     from the clock. daily.csv is the source of truth and a
 *     durable "last day synced" marker in NVS lets netTask push
 *     any day that has not reached Firebase yet, however late.
 *
 *  ------------------------------------------------------------
 *  ALSO IN v4
 *  ------------------------------------------------------------
 *   - 3 s dashboard latency: netTask keeps one TLS session open
 *     and reuses it instead of handshaking on every push.
 *   - Passive buzzer: warns at SOC_BUZZER_WARN, load relay opens
 *     at SOC_LOAD_CUTOFF. Non-blocking, LEDC hardware tone.
 *   - Relays are single pole / LIVE only; the neutral relays are
 *     gone and the interlock is now safety critical.
 *   - Task watchdog on all three tasks.
 *
 *  ------------------------------------------------------------
 *  TASK LAYOUT   (all app tasks on core 1; core 0 runs the radios)
 *  ------------------------------------------------------------
 *   loopTask  prio 1   control: energy, relays, buzzer, travel,
 *                      flash logging, day rollover. Never blocks.
 *   bleTask   prio 3   BMS link only. May block on connect().
 *   netTask   prio 1   WiFi, NTP, Firebase. May block on TLS.
 *
 *  Shared state is guarded by dataMux. The BLE callback is the
 *  only writer of the raw frame; everything else reads a snapshot.
 *
 *  ------------------------------------------------------------
 *  KEPT FROM v2/v3 (do not "fix" -- these are load bearing)
 *   - UUID 0xFFE1 is the only characteristic used; it both
 *     accepts the wake commands and delivers notifications.
 *   - Connect with an explicit BLE_ADDR_PUBLIC address type.
 *   - The two 20-byte wake frames are a fixed key captured from
 *     the real app. Zero padding is silently ignored by the BMS.
 *   - BLE starts before WiFi; no setMTU().
 *   - WiFi modem sleep stays ENABLED. Disabling it starves BLE
 *     of airtime and the BMS stops delivering frames.
 *   - The web server starts only after the BMS link is up.
 *
 *  Board    : ESP32 Dev Module
 *  Partition: Huge APP (3MB No OTA/1MB SPIFFS)
 *  Core     : esp32 by Espressif 2.0.17
 *  Libraries: NimBLE-Arduino 1.4.x
 *             ESP Async WebServer (me-no-dev)
 *             AsyncTCP (me-no-dev)
 *
 *  Protocol offsets follow syssi/esphome-jk-bms (JK02_32S). [1]
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <ESPAsyncWebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include <esp_system.h>        // esp_reset_reason()
#if __has_include("esp_coexist.h")
  #include "esp_coexist.h"      // WiFi/BLE radio arbitration, see setup()
#endif
// OLED. Included unconditionally and guarded at the CALL sites, not
// with #if around the #include: the Arduino IDE inserts its generated
// prototypes after the LAST #include line, and if that line sat inside
// a conditional block the prototypes would vanish when it is disabled.
#include <Wire.h>
// U8g2 in FULL BUFFER mode ("_F_"). The partial-buffer variants
// cannot hold a whole frame, so an animation drawn across the full
// height would tear; the full buffer costs 1 kB of RAM and is the
// only mode in which the arrow lane can be redrawn atomically.
#include <U8g2lib.h>
#include <time.h>
#include "config.h"

// ============================================================
//  SECTION 1 - SHARED DATA MODEL
// ============================================================

// struct BmsData and struct Note live in config.h: the Arduino IDE
// inserts auto-generated prototypes right after the last #include,
// so any type named in a function signature must be visible by then.
static BmsData          bmsShared;                 // guarded by dataMux
static SemaphoreHandle_t dataMux = nullptr;
static SemaphoreHandle_t fsLock  = nullptr;

#define DATA_LOCK()   do { if (dataMux) xSemaphoreTake(dataMux, portMAX_DELAY); } while (0)
#define DATA_UNLOCK() do { if (dataMux) xSemaphoreGive(dataMux); } while (0)
#define FS_LOCK()     do { if (fsLock)  xSemaphoreTake(fsLock,  portMAX_DELAY); } while (0)
#define FS_UNLOCK()   do { if (fsLock)  xSemaphoreGive(fsLock);  } while (0)

// Atomic snapshot for readers. Copying 90-odd bytes under the mutex
// is far cheaper than every consumer racing individual fields.
static BmsData bmsGet() {
  BmsData copy;
  DATA_LOCK();
  copy = bmsShared;
  DATA_UNLOCK();
  return copy;
}

// ---- energy accounting (written only by the control task) ----
static float   todayChgWh = 0, todayDisWh = 0, todayPeakW = 0;
static float   todayPvWh  = 0;            // only used when PV_ADC_ENABLE
static double  lifeChgWh  = 0, lifeDisWh  = 0;
static int32_t dayStamp   = 0;            // YYYYMMDD of the day in progress
static int32_t daySynced  = 0;            // last YYYYMMDD confirmed in Firebase
static uint8_t todayMinSoc = 100;
static bool    ntpSynced  = false;

static float   pvW = 0;                   // instantaneous array power, W
static float   loadW = 0;                 // instantaneous house draw, W

// harvest = what actually went into storage, or true PV power when
// a PV-side meter is fitted. See config.h section 6.
static inline float harvestWhToday() {
#if PV_ADC_ENABLE
  return todayPvWh;
#else
  return todayChgWh;
#endif
}

// ---- link + control state ----
static volatile BleState bleState = BLE_OFF;
static volatile uint32_t bleReconnects = 0;

static Source   srcActual   = SRC_NONE;   // what is closed right now
static Source   srcTarget   = SRC_NONE;   // what the changeover is heading to
static Source   manualSrc   = SRC_NONE;   // web override, SRC_NONE = automatic
static uint32_t manualSince = 0;
static uint32_t srcSince    = 0;
static uint32_t deadUntil   = 0;
static bool     inChangeover = false;
static const char* srcReason = "boot";

// ---- measured PV history (v7: replaces the online forecast) ----
// Declared here rather than beside the predictor because
// buildLiveJson() reports it and is defined much earlier; only
// functions get auto-generated prototypes, variables do not.
//
// Indexed BY DATE, not by position: a reboot or a missed day
// cannot shift the alignment and silently attribute one day's
// generation to another.
static PvDay    pvHist[PV_HIST_DAYS];

// ---- real-time PV evidence (control task only) ----
// Cumulative minutes today with net pack power >= PV_GOOD_W inside
// the counting window. Monotonic within a day, so the verdict it
// feeds cannot oscillate. Persisted with the other counters so a
// midday reboot does not throw the day's evidence away.
static int      pvGoodMin      = 0;
static uint32_t pvAccumMs      = 0;       // sub-minute remainder

// ---- online forecast cache (netTask only, read anywhere) ----
// The SECOND opinion, subordinate to the PV history above. See
// config.h 8d/8k. Indexed by date for the same reason pvHist is.
static WxDay    wxCache[WEATHER_DAYS];
static int32_t  wxFetchedStamp = 0;       // date of the last good fetch
static uint32_t wxNextTry      = 0;       // millis() of the next attempt
static uint32_t wxFails        = 0;
static bool     wxEverOk       = false;
static int32_t  wxLastMorning  = 0;       // date of the last morning refresh

// ---- learned forecast bias (section 11d, DISPLAY ONLY) ----
// One entry per WxCond. Never read by section 12.
static WxBias   wxBias[8];

// ---- battery ETA smoothing (section 11e, DISPLAY ONLY) ----
// A rolling mean of the ALREADY FILTERED pvFilt / loadFilt, so the
// remaining-time figure moves at reading speed. No new measurement.
static float    etaRingL[ETA_AVG_MINUTES] = {0};
static float    etaRingC[ETA_AVG_MINUTES] = {0};
static uint8_t  etaRingN = 0, etaRingHead = 0;
static float    etaAccL = 0, etaAccC = 0;
static uint16_t etaAccN = 0;
static uint32_t etaAccMs = 0;

// ---- v7 measurement stabilisation (control task only) ----
//
// Nothing in the source state machine reads a raw sample any more.
// See config.h section 8j for why: parseCellInfo() writes soc
// straight from a frame byte, and one bad byte used to be enough
// to move a mains relay.
static int      socStableVal = -1;        // validated SoC, -1 = unknown
static int      socPending   = -1;        // candidate failing validation
static uint8_t  socPendCount = 0;
static int      socTrendRing[SOC_TREND_SPAN] = {0};
static uint8_t  socTrendN    = 0;
static uint8_t  socTrendHead = 0;
static uint32_t socTrendMs   = 0;
static int      socTrend     = 0;         // -1 falling, 0 flat, +1 rising

static float    pvFilt   = 0;             // low-pass array watts
static float    loadFilt = 0;             // low-pass house draw
static bool     filtInit = false;

// ---- CEB source state machine ----
static bool     cebOn      = false;       // authoritative CEB request
static uint32_t cebSince   = 0;           // millis() when CEB engaged
static uint16_t cebRelMin  = 0;           // today's release deadline, minutes

// Persistence gates. Every engage/release condition passes through
// one of these, so no transition can come from a single sample.
static Persist  pEngage, pRelease, pCritical, pLostBms, pUrgent;
static bool     cebLowArmed = false;      // Schmitt state of the CEB floor
static SrcPhase srcPhase   = SRCP_WAIT;   // startup stabilisation
static bool     srcEverSet = false;       // has a source ever been commanded
static uint32_t srcLastRej = 0;           // rejected-transition log throttle

// ---- boot diagnostics + source persistence (config.h 8o) ----
static esp_reset_reason_t bootReason = ESP_RST_UNKNOWN;
static bool     clockTrusted = false;     // RTC survived the reset
static bool     srcRestored  = false;     // NVS state re-applied at boot
static char     srcRestoreMsg[128] = "";  // printed once Serial is up
static SrcReason srcLastReason = SR_NONE;
static uint32_t relayOps = 0;             // lifetime transfer count
static bool     sysFault = false;         // panic / brownout / watchdog boot
static ResetRec resetLog[RESET_LOG_N];

// Heap / stack trend. Reporting only; nothing here can move a relay.
static uint32_t heapMinEver = 0xFFFFFFFF;
static uint32_t heapLastLog = 0;
static uint32_t loopLastMs  = 0;

// Transfer-burst diagnostic (config.h 8o).
static uint32_t xferTimes[XFER_BURST_N] = {0};
static uint8_t  xferHead = 0;
static bool     xferBurst = false;

// WiFi/BLE coexistence gate. bleTask sets it while it is walking the
// GATT table; netTask declines to start a TLS handshake meanwhile.
// A flag, not a delay -- neither task ever blocks on the other.
static volatile bool bleBusy = false;


static bool     travelMode  = false;
static bool     lightOn     = false;      // switched load circuit state
static LightMode lightMode  = LIGHT_AUTO; // web override, AUTO by default
static bool     lightsLow   = false;      // latched low-battery lockout
static uint32_t lightSecToday = 0;        // lights runtime today, seconds
static bool     loadCutoff  = false;      // true = protection has opened the load
static bool     emergAlarm  = false;      // latched 10% + discharging
static BuzzTone buzzMode    = BUZZ_SILENT;
static uint32_t utilitySecToday = 0;

// ---- plumbing ----
static Preferences prefs;
static bool     fsOk = false;
static uint32_t queuePos = 0;
static int32_t  clockAdj = 0;
static uint16_t bufferedLines = 0;
static AsyncWebServer server(80);

// ============================================================
//  SECTION 2 - SMALL HELPERS
// ============================================================
static uint16_t u16(const uint8_t* d, int i) { return d[i] | (d[i + 1] << 8); }
static uint32_t u32(const uint8_t* d, int i) {
  return (uint32_t)d[i] | ((uint32_t)d[i+1] << 8) | ((uint32_t)d[i+2] << 16) | ((uint32_t)d[i+3] << 24);
}
static int16_t s16(const uint8_t* d, int i) { return (int16_t)u16(d, i); }
static int32_t s32(const uint8_t* d, int i) { return (int32_t)u32(d, i); }

static uint8_t sumCrc(const uint8_t* d, uint16_t n) {
  uint8_t c = 0;
  for (uint16_t i = 0; i < n; i++) c += d[i];
  return c;
}

time_t nowEpoch() { time_t t; time(&t); return t; }
bool   timeReady() { return nowEpoch() > 1700000000L; }

// ---- boot diagnostics (config.h 8o) ------------------------------
const char* resetReasonName(uint8_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

// A reset that indicates something went wrong, as opposed to a
// deliberate power-up or an upload.
static bool resetIsFault(uint8_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_BROWNOUT ||
         r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

// Does the RTC keep counting across this kind of reset? Only a loss
// of power stops it -- and that is exactly the case where the age of
// a stored record cannot be measured. See config.h 8o.
static bool resetKeepsClock(uint8_t r) {
  return !(r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || r == ESP_RST_UNKNOWN);
}

bool localNow(struct tm* out) {
  time_t t = nowEpoch();
  if (t < 1700000000L) return false;
  localtime_r(&t, out);
  return true;
}

int32_t dateStamp(time_t t) {
  struct tm tmv; localtime_r(&t, &tmv);
  return (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
}
void dateString(time_t t, char* out) {
  struct tm tmv; localtime_r(&t, &tmv);
  strftime(out, 12, "%Y-%m-%d", &tmv);
}
// 20260801 -> "2026-08-01"
void stampToString(int32_t s, char* out) {
  // Clamped so the 12-byte destination is provably large enough.
  // Real stamps are always in range; this only pins the bound.
  int y = (int)(s / 10000), m = (int)((s / 100) % 100), d = (int)(s % 100);
  if (y < 0) y = 0; else if (y > 9999) y = 9999;
  if (m < 0) m = 0; else if (m > 99)   m = 99;
  if (d < 0) d = 0; else if (d > 99)   d = 99;
  snprintf(out, 12, "%04d-%02d-%02d", y, m, d);
}

// Link health is judged on DATA, never on the socket. See defect 2.
static bool bmsFresh() {
  uint32_t last = bmsShared.lastFrameMs;     // 32-bit read is atomic
  return last && (millis() - last) < BMS_FRAME_TIMEOUT_MS;
}
static bool bmsLost() {
  uint32_t last = bmsShared.lastFrameMs;
  return !last || (millis() - last) > 300000UL;
}

const char* srcName(Source s) {
  return s == SRC_SOLAR ? "solar" : s == SRC_UTILITY ? "utility" : "none";
}
const char* bleStateName(BleState s) {
  switch (s) {
    case BLE_IDLE:       return "idle";
    case BLE_CONNECTING: return "connecting";
    case BLE_DISCOVER:   return "discovering";
    case BLE_WAIT_DATA:  return "waiting";
    case BLE_STREAMING:  return "streaming";
    default:             return "off";
  }
}

// tiny JSON scalar reader, enough for our own single-level records
static String jsonRaw(const String& s, const char* key) {
  String k = String("\"") + key + "\":";
  int p = s.indexOf(k);
  if (p < 0) return String();
  p += k.length();
  int e = p;
  while (e < (int)s.length() && s[e] != ',' && s[e] != '}') e++;
  return s.substring(p, e);
}
static long  jsonLong (const String& s, const char* key) { return jsonRaw(s, key).toInt(); }
static float jsonFloat(const String& s, const char* key) { return jsonRaw(s, key).toFloat(); }
static String withTs(const String& line, long ts) {
  int p = line.indexOf("\"t\":");
  if (p < 0) return line;
  int e = line.indexOf(',', p);
  if (e < 0) return line;
  return line.substring(0, p + 4) + String(ts) + line.substring(e);
}

// ============================================================
//  SECTION 3 - JK BMS PROTOCOL
//
//  parseCellInfo() runs on the NimBLE host task. It must stay a
//  pure memcpy-and-scale routine: no flash, no network, no NVS,
//  no logging beyond a counter. See defect 1 in the header.
// ============================================================
static uint8_t  frameBuf[400];
static uint16_t frameLen = 0;
static volatile uint8_t rawLogLeft = 10;

static void parseCellInfo(const uint8_t* d) {
  DATA_LOCK();
  for (int i = 0; i < 4; i++) bmsShared.cell[i] = u16(d, 6 + i * 2) * 0.001f;
  bmsShared.mosT      = s16(d, 144) * 0.1f;
  bmsShared.packV     = u32(d, 150) * 0.001f;
  bmsShared.packI     = s32(d, 158) * 0.001f;
  bmsShared.packW     = bmsShared.packV * bmsShared.packI;
  bmsShared.t1        = s16(d, 162) * 0.1f;
  bmsShared.t2        = s16(d, 164) * 0.1f;
  bmsShared.errBits   = u32(d, 166);
  bmsShared.balI      = s16(d, 170) * 0.001f;
  bmsShared.balancing = d[172] != 0x00;
  bmsShared.soc       = d[173];
  bmsShared.remainAh  = u32(d, 174) * 0.001f;
  bmsShared.fullAh    = u32(d, 178) * 0.001f;
  bmsShared.cycles    = u32(d, 182);
  bmsShared.soh       = d[190];
  bmsShared.chgMos    = d[198] != 0;
  bmsShared.disMos    = d[199] != 0;
  bmsShared.lastFrameMs = millis();
  bmsShared.frameCount++;
  DATA_UNLOCK();
  rawLogLeft = 0;                       // stop raw dumping once parsing works
}

static void assembleFrame(const uint8_t* data, size_t len) {
  if (len >= 4 && data[0]==0x55 && data[1]==0xAA && data[2]==0xEB && data[3]==0x90)
    frameLen = 0;
  if (frameLen + len > sizeof(frameBuf)) { frameLen = 0; return; }
  memcpy(frameBuf + frameLen, data, len);
  frameLen += len;

  if (frameLen >= 300) {
    if (sumCrc(frameBuf, 299) == frameBuf[299]) {
      if (frameBuf[4] == 0x02) parseCellInfo(frameBuf);
    }
    frameLen = 0;
  }
}

static void notifyCB(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool) {
  if (rawLogLeft) {
    rawLogLeft--;
    Serial.printf("[BLE] notify h=0x%02X len=%u first8=", c->getHandle(), (unsigned)len);
    for (size_t i = 0; i < len && i < 8; i++) Serial.printf("%02X ", data[i]);
    Serial.println();
  }
  assembleFrame(data, len);
}

// ---------------------------------------------------------------
// Reverse-engineered from a real JK app BLE capture (btsnoop) on
// this exact BMS (hw V21H). These two 20-byte frames are sent
// verbatim by the official app before it will stream cell data.
// The trailing bytes are NOT random/session-based: they were
// identical across two separate connection sessions captured
// 14 minutes apart, so they are a fixed key this firmware relies
// on. Zero-padding is silently ignored by the BMS.
// Both go to UUID 0xFFE1 only.
// ---------------------------------------------------------------
static const uint8_t CMD_DEVICE_INFO[20] = {
  0xAA,0x55,0x90,0xEB,0x97,0x00,
  0x97,0xA2,0x55,0x53,0xBE,0xF1,0xFC,0xF9,0x79,0x6B,0x52,0x14,
  0x13,0xF3
};
static const uint8_t CMD_CELL_INFO[20] = {
  0xAA,0x55,0x90,0xEB,0x96,0x00,
  0xE9,0xE2,0x2D,0x51,0x8E,0x1F,0x56,0x08,0x57,0x27,0xA7,0x05,
  0xD4,0x62
};

// ============================================================
//  SECTION 4 - BLE LINK TASK
//
//  Owns the whole BMS connection lifecycle. This is the only
//  task allowed to call NimBLE connect/disconnect, so there is
//  no way for two contexts to fight over the client handle.
// ============================================================
static NimBLEClient* bleClient = nullptr;
static NimBLERemoteCharacteristic* chrFfe1 = nullptr;
static volatile bool bleLinkUp = false;        // ACL state from callbacks

class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override { bleLinkUp = true; }
  void onDisconnect(NimBLEClient*) override {
    bleLinkUp = false;
    chrFfe1 = nullptr;                          // handle is dead, never reuse
    Serial.println("[BLE] disconnected");
  }
};
static ClientCB clientCb;                       // static: never leaks

static void bmsSend(const uint8_t* frame) {
  NimBLERemoteCharacteristic* c = chrFfe1;
  if (!c || !bleLinkUp) return;
  c->writeValue((uint8_t*)frame, 20, false);    // write without response
}

// Walk the GATT table and subscribe. Returns false on any problem;
// the caller then tears the link down rather than limping on.
static bool bleDiscover() {
  chrFfe1 = nullptr;
  rawLogLeft = 10;

  NimBLERemoteService* svc = bleClient->getService(NimBLEUUID((uint16_t)0xFFE0));
  if (!svc) { Serial.println("[BLE] service 0xffe0 missing"); return false; }

  NimBLERemoteCharacteristic* c = svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFE1));
  if (!c) { Serial.println("[BLE] characteristic 0xffe1 missing"); return false; }

  Serial.printf("[BLE] ffe1 handle=0x%02X notify=%d writeNR=%d\n",
                c->getHandle(), c->canNotify(), c->canWriteNoResponse());

  if (!c->canNotify() || !c->subscribe(true, notifyCB, true)) {
    Serial.println("[BLE] subscribe to 0xffe1 failed");
    return false;
  }
  chrFfe1 = c;
  Serial.println("[BLE] subscribed to 0xffe1");
  return true;
}

// Full teardown. Called on every failure path so a half-open link
// or a stale characteristic handle can never survive into the next
// attempt -- that was one of the ways v3 got stuck.
static void bleTeardown(const char* why) {
  Serial.printf("[BLE] teardown: %s\n", why);
  chrFfe1 = nullptr;
  if (bleClient && bleClient->isConnected()) bleClient->disconnect();
  // give the stack a moment to run the disconnect callback
  for (int i = 0; i < 20 && bleLinkUp; i++) vTaskDelay(pdMS_TO_TICKS(25));
  bleLinkUp = false;
}

static void bleTask(void*) {
  esp_task_wdt_add(NULL);

  uint32_t backoff      = BLE_BACKOFF_MIN_MS;
  uint32_t nextAttempt  = 0;
  uint32_t stateSince   = millis();
  uint32_t lastKeepAlive = 0;
  // frameCount at the moment we subscribed. "Has this session
  // produced data?" must be judged against THIS connection, not
  // against lastFrameMs, which survives a reconnect and would make
  // every retry look instantly healthy.
  uint32_t framesAtSubscribe = 0;

  bleClient = NimBLEDevice::createClient();
  bleClient->setClientCallbacks(&clientCb, false);
  bleClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_S);
  bleState = BLE_IDLE;

  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();

    switch (bleState) {

      case BLE_IDLE:
        if ((int32_t)(now - nextAttempt) >= 0) {
          bleState = BLE_CONNECTING;
          stateSince = now;
        }
        break;

      case BLE_CONNECTING: {
        Serial.printf("[BLE] connecting (backoff %lus)...\n", (unsigned long)(backoff / 1000));
        bool ok = bleClient->connect(NimBLEAddress(BMS_MAC, BLE_ADDR_PUBLIC));
        if (!ok) {
          Serial.println("[BLE] connect failed (JK app still open?)");
          bleTeardown("connect failed");
          backoff = min(backoff * 2, (uint32_t)BLE_BACKOFF_MAX_MS);
          nextAttempt = millis() + backoff;
          bleState = BLE_IDLE;
          break;
        }
        Serial.println("[BLE] connected");
        bleState = BLE_DISCOVER;
        stateSince = millis();
        break;
      }

      case BLE_DISCOVER: {
        vTaskDelay(pdMS_TO_TICKS(300));         // let the stack settle
        if (!bleDiscover()) {
          bleTeardown("discovery failed");
          backoff = min(backoff * 2, (uint32_t)BLE_BACKOFF_MAX_MS);
          nextAttempt = millis() + backoff;
          bleState = BLE_IDLE;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        bmsSend(CMD_DEVICE_INFO);
        vTaskDelay(pdMS_TO_TICKS(300));
        bmsSend(CMD_CELL_INFO);
        Serial.println("[BLE] wake sequence sent");
        framesAtSubscribe = bmsShared.frameCount;
        lastKeepAlive = millis();
        stateSince    = millis();
        bleState      = BLE_WAIT_DATA;
        break;
      }

      case BLE_WAIT_DATA: {
        if (!bleLinkUp) {
          bleTeardown("link dropped while waiting for data");
          backoff = min(backoff * 2, (uint32_t)BLE_BACKOFF_MAX_MS);
          nextAttempt = millis() + backoff;
          bleState = BLE_IDLE;
          break;
        }
        if (bmsShared.frameCount != framesAtSubscribe) {   // data on THIS link
          Serial.println("[BLE] streaming");
          // Backoff is NOT forgiven here. One frame proves the link
          // came up, not that it will stay up; BLE_STREAMING clears
          // the backoff only after BLE_STABLE_AFTER_MS of real data.
          stateSince = millis();
          bleState   = BLE_STREAMING;
          break;
        }
        // Still waiting for the first frame of this session. Re-poking
        // here is legitimate -- the stream has not started yet -- but
        // each write chirps the BMS buzzer, so keep it slow.
        if (now - lastKeepAlive >= BMS_NUDGE_AFTER_MS) {
          lastKeepAlive = now;
          Serial.println("[BLE] no frames yet, resending wake sequence");
          bmsSend(CMD_DEVICE_INFO);
          vTaskDelay(pdMS_TO_TICKS(150));
          bmsSend(CMD_CELL_INFO);
        }
        if (now - stateSince > BMS_HANDSHAKE_MS) {
          bleTeardown("subscribed but BMS never sent data");
          bleReconnects++;
          backoff = min(backoff * 2, (uint32_t)BLE_BACKOFF_MAX_MS);
          nextAttempt = millis() + backoff;
          bleState = BLE_IDLE;
        }
        break;
      }

      case BLE_STREAMING: {
        // THE FIX FOR DEFECT 2. A live socket is not a live link:
        // supervise the data, and treat silence as failure.
        //
        // Backoff is only forgiven once the session has actually held
        // up for BLE_STABLE_AFTER_MS. Resetting it on the first frame
        // (v4.0) meant a link that connected, delivered one frame and
        // dropped kept retrying at the 3 s floor forever -- a BLE
        // connection, and therefore a BMS buzzer chirp, every 3 s.
        if (backoff != BLE_BACKOFF_MIN_MS && now - stateSince >= BLE_STABLE_AFTER_MS) {
          backoff = BLE_BACKOFF_MIN_MS;
          Serial.println("[BLE] link stable, backoff reset");
        }

        if (!bleLinkUp) {
          bleTeardown("stack reported disconnect");
          bleReconnects++;
          backoff = min(backoff * 2, (uint32_t)BLE_BACKOFF_MAX_MS);
          nextAttempt = millis() + backoff;
          bleState = BLE_IDLE;
          break;
        }
        if (!bmsFresh()) {
          bleTeardown("no frame for 20 s, link is dead");
          bleReconnects++;
          backoff = min(backoff * 2, (uint32_t)BLE_BACKOFF_MAX_MS);
          nextAttempt = millis() + backoff;
          bleState = BLE_IDLE;
          break;
        }

        // NO unconditional keep-alive. The BMS chirps its buzzer on
        // every command write it accepts, and it does not need the
        // request repeated -- the notify subscription streams on its
        // own. Only re-prime if the stream has actually gone quiet,
        // and then at most once per BMS_NUDGE_AFTER_MS.
        uint32_t quietFor = millis() - bmsShared.lastFrameMs;
        if (quietFor >= BMS_NUDGE_AFTER_MS && now - lastKeepAlive >= BMS_NUDGE_AFTER_MS) {
          lastKeepAlive = now;
          Serial.printf("[BLE] stream quiet %lu ms, re-priming\n", (unsigned long)quietFor);
          bmsSend(CMD_CELL_INFO);
        }
        break;
      }

      default:
        bleState = BLE_IDLE;
        break;
    }

    // Coexistence gate (config.h 8o), set from one place rather than
    // scattered through the cases so it cannot be left raised on an
    // error path. It is true exactly while the radio is busy bringing
    // the link up -- connect() and GATT discovery -- which is the
    // window that contends with a TLS handshake. netTask reads it and
    // skips its cloud work for one 100 ms pass. Neither task ever
    // blocks on the other and there is no delay involved.
    bleBusy = (bleState == BLE_CONNECTING || bleState == BLE_DISCOVER);

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================================================
//  SECTION 5 - PV METERING + ENERGY INTEGRATION
// ============================================================
static float readPvWatts(const BmsData& b) {
#if PV_ADC_ENABLE
  (void)b;
  float mv = analogReadMilliVolts(PV_VOLT_PIN) * PV_VOLT_DIVIDER;
  float v  = mv / 1000.0f;
  float i  = (analogReadMilliVolts(PV_CURR_PIN) - PV_CURR_ZERO_MV) / PV_CURR_MV_PER_A;
  if (i < 0) i = 0;
  return v * i;
#else
  // no PV meter: everything that flows into the pack is harvest
  return b.packW > 0 ? b.packW : 0;
#endif
}

// v7: reads the FILTERED array power, not the instantaneous value.
// pvW crosses SOLAR_OK_W every time an appliance starts, which used
// to be enough to flip the deep-discharge backstop.
static bool solarProducing() {
  struct tm tmv;
  bool haveTime = localNow(&tmv);
  bool daylight = !haveTime || (tmv.tm_hour >= PV_HOUR_START && tmv.tm_hour < PV_HOUR_END);
  return bmsFresh() && daylight && pvFilt > SOLAR_OK_W;
}

// ---- v7: measurement stabilisation (config.h section 8j) --------
//
// One-pole low-pass on the two power signals the controller reads.
// dt comes from integrateEnergy(), which already rejects gaps, so
// the filter cannot be stepped by a stalled link.
static void filterUpdate(float dt) {
  if (dt <= 0 || dt > 5.0f) return;
  if (!filtInit) { pvFilt = pvW; loadFilt = loadW; filtInit = true; return; }
  float a = dt / (PV_FILTER_TC_S + dt);
  pvFilt   += a * (pvW   - pvFilt);
  loadFilt += a * (loadW - loadFilt);
}

// Validate one raw SoC byte and maintain the short-term trend.
//
// THE ROOT CAUSE OF THE REPORTED OSCILLATION. bmsShared.soc is frame
// byte 173 with no sanity check anywhere between the radio and the
// relay, so a single corrupt or transient frame reading 0% satisfied
// "soc <= SOC_CEB_ON", latched CEB, and held the house on mains for
// CEB_MIN_ON_MS before handing it back. Everything downstream now
// reads socStableVal instead, and a lone bad sample never reaches it.
//
// A sustained change IS still honoured: SOC_OUTLIER_CONFIRM repeats
// of the same implausible value are accepted, so a genuine BMS
// recalibration gets through within a few control ticks.
static void socUpdate(const BmsData& b, uint32_t now) {
  if (!bmsFresh()) {
    // No data is UNKNOWN. It is never 0%, and nothing downstream may
    // turn it into 0% and then act on it.
    socStableVal = -1;
    socPending   = -1;
    socPendCount = 0;
    socTrendN    = 0;
    socTrend     = 0;
    return;
  }

  int raw = (int)b.soc;
  if (raw < SOC_VALID_MIN || raw > SOC_VALID_MAX) {
    socPending   = -1;                 // impossible value: drop it silently
    socPendCount = 0;
  } else if (socStableVal < 0) {
    socStableVal = raw;                // first reading establishes the value
    socPending   = -1;
    socPendCount = 0;
  } else if (raw - socStableVal <= SOC_MAX_JUMP &&
             socStableVal - raw <= SOC_MAX_JUMP) {
    socStableVal = raw;                // plausible step: accept
    socPending   = -1;
    socPendCount = 0;
  } else {
    int d = raw - socPending;
    if (socPending >= 0 && d <= 1 && d >= -1) socPendCount++;
    else { socPending = raw; socPendCount = 1; }
    if (socPendCount >= SOC_OUTLIER_CONFIRM) {
      Serial.printf("[soc] %d%% -> %d%% accepted after %u confirmations\n",
                    socStableVal, raw, (unsigned)socPendCount);
      socStableVal = raw;
      socPending   = -1;
      socPendCount = 0;
    } else {
      Serial.printf("[soc] rejected implausible sample %d%% (stable %d%%)\n",
                    raw, socStableVal);
    }
  }

  // Short-term trend over SOC_TREND_SPAN samples. Used for reporting
  // and diagnostics; the decision gates use persistence, not trend,
  // so a trend cannot on its own move a relay.
  if (socStableVal >= 0 &&
      (socTrendMs == 0 || (uint32_t)(now - socTrendMs) >= SOC_TREND_PERIOD_MS)) {
    socTrendMs = now;
    int oldest = socTrendRing[socTrendHead];     // slot about to be overwritten
    socTrendRing[socTrendHead] = socStableVal;
    socTrendHead = (uint8_t)((socTrendHead + 1) % SOC_TREND_SPAN);
    if (socTrendN < SOC_TREND_SPAN) socTrendN++;
    else socTrend = socStableVal > oldest ? 1 : socStableVal < oldest ? -1 : 0;
  }
}

const char* socTrendName() {
  if (socTrendN < SOC_TREND_SPAN) return "settling";
  return socTrend > 0 ? "rising" : socTrend < 0 ? "falling" : "flat";
}

// Time-based integration on the control tick. v3 integrated inside
// the BLE callback, so the sample rate followed the BMS frame rate
// and a stalled link silently froze the counters.
static void integrateEnergy(const BmsData& b) {
  static uint32_t lastMs = 0;
  uint32_t nowMs = millis();

  if (!bmsFresh()) {                 // no trustworthy data: pause cleanly
    lastMs = 0;
    pvW = 0;
    loadW = 0;
    filtInit = false;                // do not drag the filters across a gap
    return;
  }

  pvW   = readPvWatts(b);
  loadW = b.packW < 0 ? -b.packW : 0;

  if (lastMs == 0) { lastMs = nowMs; return; }
  float dt = (nowMs - lastMs) / 1000.0f;
  lastMs = nowMs;
  if (dt <= 0 || dt > 5.0f) return;  // clamp: a gap is not energy

  filterUpdate(dt);                  // v7: filtered PV/load for the controller

  float wh = b.packW * dt / 3600.0f;
  if (b.packI >  CURRENT_DEADBAND) { todayChgWh += wh;  lifeChgWh += wh; }
  if (b.packI < -CURRENT_DEADBAND) { todayDisWh += -wh; lifeDisWh += -wh; }
  if (b.packW > todayPeakW) todayPeakW = b.packW;
  if (b.soc && b.soc < todayMinSoc) todayMinSoc = b.soc;
  todayPvWh += pvW * dt / 3600.0f;

  // ---- real-time PV evidence ----
  // Count MINUTES the array spends genuinely working, inside the
  // daylight window. This is the corrective signal that lets today's
  // measurement override a wrong forecast (config.h 8f). It only
  // ever rises within a day, so the verdict it feeds cannot chatter.
  {
    struct tm tmv;
    if (localNow(&tmv) &&
        tmv.tm_hour >= PV_WINDOW_START_HOUR && tmv.tm_hour < PV_WINDOW_END_HOUR &&
        b.packW >= PV_GOOD_W) {
      pvAccumMs += (uint32_t)(dt * 1000.0f);
      while (pvAccumMs >= 60000UL) { pvAccumMs -= 60000UL; pvGoodMin++; }
    }
  }
}

// ============================================================
//  SECTION 6 - PERSISTENCE
// ============================================================
static void saveCounters() {
  prefs.putFloat("tc", todayChgWh);
  prefs.putFloat("td", todayDisWh);
  prefs.putFloat("tp", todayPeakW);
  prefs.putFloat("tv", todayPvWh);
  prefs.putDouble("lc", lifeChgWh);
  prefs.putDouble("ld", lifeDisWh);
  prefs.putInt("day", dayStamp);
  prefs.putInt("dsyn", daySynced);
  prefs.putUChar("mso", todayMinSoc);
  prefs.putUInt("qp", queuePos);
  prefs.putInt("adj", clockAdj);
  prefs.putUInt("us", utilitySecToday);
  prefs.putUInt("ls", lightSecToday);
  prefs.putInt("pvg", pvGoodMin);      // PV evidence survives a midday reboot
  prefs.putLong64("ep", (int64_t)nowEpoch());
}

// ---- source persistence (config.h 8o) ----------------------------
//
// Written ONLY from a completed transfer, so the write count equals
// the transfer count -- roughly twice a day on this system.
static void sourceSave(Source s, SrcReason why) {
  prefs.putUChar(SRC_NVS_STATE,  (uint8_t)s);
  prefs.putLong64(SRC_NVS_EPOCH, (int64_t)nowEpoch());
  prefs.putUChar(SRC_NVS_REASON, (uint8_t)why);
  prefs.putChar(SRC_NVS_SOC,     (int8_t)(socStableVal < 0 ? -1 : socStableVal));
  prefs.putUInt(SRC_NVS_OPS,     relayOps);
  srcLastReason = why;
}

static void resetLogLoad() {
  size_t n = prefs.getBytesLength(RESET_LOG_NVS);
  if (n == sizeof(resetLog)) prefs.getBytes(RESET_LOG_NVS, resetLog, sizeof(resetLog));
  else                       memset(resetLog, 0, sizeof(resetLog));
}

// Push this boot onto the ring. One write per boot.
static void resetLogPush(uint8_t reason, int32_t stamp, uint16_t prevUpMin) {
  for (int i = RESET_LOG_N - 1; i > 0; i--) resetLog[i] = resetLog[i - 1];
  resetLog[0].stamp  = stamp;
  resetLog[0].upMin  = prevUpMin;
  resetLog[0].reason = reason;
  resetLog[0].pad    = 0;
  prefs.putBytes(RESET_LOG_NVS, resetLog, sizeof(resetLog));
}

static void loadCounters() {
  todayChgWh = prefs.getFloat("tc", 0);
  todayDisWh = prefs.getFloat("td", 0);
  todayPeakW = prefs.getFloat("tp", 0);
  todayPvWh  = prefs.getFloat("tv", 0);
  lifeChgWh  = prefs.getDouble("lc", 0);
  lifeDisWh  = prefs.getDouble("ld", 0);
  dayStamp   = prefs.getInt("day", 0);
  daySynced  = prefs.getInt("dsyn", 0);
  todayMinSoc = prefs.getUChar("mso", 100);
  clockAdj   = prefs.getInt("adj", 0);
  utilitySecToday = prefs.getUInt("us", 0);
  lightSecToday   = prefs.getUInt("ls", 0);
  pvGoodMin = prefs.getInt("pvg", 0);
  // Restore an approximate clock from NVS -- but ONLY if the RTC is
  // not already running. After a soft reset the RTC survived and is
  // strictly newer; overwriting it with the last saved epoch would
  // step the clock BACKWARDS and make the source-record age test in
  // section 8o read as older than it is.
  int64_t ep = prefs.getLong64("ep", 0);
  if (ep > 1700000000LL && !timeReady()) {
    struct timeval tv = { .tv_sec = (time_t)ep, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
  }
}

// ============================================================
//  SECTION 7 - JSON BUILDERS
// ============================================================
// The boot ring as a compact array: [["TASK_WDT",20260909,142],...]
// Written into a caller-supplied buffer with snprintf -- no String,
// so this cannot fragment the heap however long the box runs.
static int buildResetJson(char* out, size_t cap) {
  int n = snprintf(out, cap, "[");
  bool first = true;
  for (int i = 0; i < RESET_LOG_N && n < (int)cap; i++) {
    if (!resetLog[i].reason && !resetLog[i].stamp) continue;
    n += snprintf(out + n, cap - n, "%s[\"%s\",%ld,%u]",
                  first ? "" : ",", resetReasonName(resetLog[i].reason),
                  (long)resetLog[i].stamp, (unsigned)resetLog[i].upMin);
    first = false;
  }
  if (n < (int)cap) n += snprintf(out + n, cap - n, "]");
  return n;
}

static int buildLiveJson(char* out, size_t cap) {
  char resets[340];
  buildResetJson(resets, sizeof(resets));
  BmsData b = bmsGet();
  time_t t = nowEpoch();
  return snprintf(out, cap,
    "{\"ts\":%ld,\"ntp\":%s,"
    "\"v\":%.2f,\"i\":%.2f,\"p\":%.1f,\"soc\":%u,\"remAh\":%.1f,"
    "\"c1\":%.3f,\"c2\":%.3f,\"c3\":%.3f,\"c4\":%.3f,"
    "\"t1\":%.1f,\"t2\":%.1f,\"mosT\":%.1f,\"balI\":%.3f,\"bal\":%s,"
    "\"chgMos\":%s,\"disMos\":%s,\"err\":%lu,\"soh\":%u,\"cyc\":%lu,"
    "\"todayChg\":%.1f,\"todayDis\":%.1f,\"peakW\":%.1f,"
    "\"lifeChg\":%.0f,\"lifeDis\":%.0f,"
    "\"pvW\":%.1f,\"loadW\":%.1f,\"gridW\":%.1f,\"harvestWh\":%.1f,"
    "\"src\":\"%s\",\"relayU\":%s,\"relayS\":%s,\"why\":\"%s\","
    "\"manual\":%s,\"travel\":%s,\"light\":%s,\"utilMin\":%lu,"
    "\"rainy\":%s,\"nightFloor\":%d,"
    "\"ceb\":%s,\"cebOn\":%d,\"cebOff\":%d,\"cebRel\":\"%02d:%02d\","
    "\"house\":\"%s\","
    "\"wx\":\"%s\",\"wxOk\":%s,\"wxIcon\":\"%s\",\"wxText\":\"%s\","
    "\"wxSource\":\"%s\",\"wxScore\":%d,\"wxDays\":%d,"
    "\"fcCond\":\"%s\",\"fcText\":\"%s\",\"fcClass\":\"%s\","
    "\"fcAgeDays\":%d,\"fcConf\":%d,\"pvConf\":%d,\"wxAgree\":\"%s\","
    "\"fcCloud\":%d,\"fcRain\":%.1f,"
    "\"baseWh\":%.0f,\"predTodayWh\":%.0f,"
    "\"boot\":\"%s\",\"sysFault\":%s,\"relayOps\":%lu,"
    "\"srcRestored\":%s,\"xferBurst\":%s,\"heapMin\":%lu,\"resets\":%s,"
    "\"lightMode\":\"%s\",\"lightMin\":%lu,\"lightsLow\":%s,"
    "\"lightOn\":\"%02d:%02d\",\"lightOff\":\"%02d:%02d\",\"lightSunset\":%s,"
    "\"tomCond\":\"%s\",\"tomText\":\"%s\",\"tomConf\":\"%s\","
    "\"tomWh\":%.0f,\"tomPct\":%d,"
    "\"pvGoodMin\":%d,\"pvVerdict\":\"%s\","
    "\"socStable\":%d,\"socTrend\":\"%s\",\"pvFilt\":%.1f,"
    "\"loadFilt\":%.1f,\"srcPhase\":\"%s\",\"srcDwellS\":%lu,"
    "\"cutoff\":%s,\"buzz\":%u,\"emerg\":%s,"
    "\"bmsLink\":%s,\"ble\":\"%s\",\"reconn\":%lu,"
    "\"rssi\":%d,\"buffered\":%u,\"heap\":%lu,\"up\":%lu}",
    (long)t, ntpSynced ? "true" : "false",
    b.packV, b.packI, b.packW, b.soc, b.remainAh,
    b.cell[0], b.cell[1], b.cell[2], b.cell[3],
    b.t1, b.t2, b.mosT, b.balI, b.balancing ? "true" : "false",
    b.chgMos ? "true" : "false", b.disMos ? "true" : "false",
    (unsigned long)b.errBits, b.soh, (unsigned long)b.cycles,
    todayChgWh, todayDisWh, todayPeakW, lifeChgWh, lifeDisWh,
    pvW, loadW, srcActual == SRC_UTILITY ? loadW : 0.0f, harvestWhToday(),
    srcName(srcActual),
    srcActual == SRC_UTILITY ? "true" : "false",
    srcActual == SRC_SOLAR   ? "true" : "false",
    srcReason,
    manualSrc != SRC_NONE ? "true" : "false",
    travelMode ? "true" : "false", lightOn ? "true" : "false",
    (unsigned long)(utilitySecToday / 60),
    wxEffectiveClass() == WX_HEAVY ? "true" : "false",   /* legacy key */
    SOC_CEB_ON,                                       /* legacy key */
    cebOn ? "true" : "false", SOC_CEB_ON, SOC_CEB_OFF,
    cebRelMin / 60, cebRelMin % 60,
    srcActual == SRC_UTILITY ? "CEB" : "Pack",
    wxName(wxEffectiveClass()), wxTrusted() ? "true" : "false",
    wxIconKey(wxIconNow()), wxIconName(wxIconNow()),
    wxDecisionSrc(), pvExpectedScore(), pvHistDays(),
    wxCondKey(wxCondNow()), wxCondName(wxCondNow()),
    wxName(wxForecastClass()), wxAgeDays(), fcConfidence(), pvConfidence(),
    wxAgreement(),
    wxRelevantCloud(), wxRelevantRain(),
    predToday().baseWh, predToday().predWh,
    resetReasonName((uint8_t)bootReason), sysFault ? "true" : "false",
    (unsigned long)relayOps,
    srcRestored ? "true" : "false", xferBurst ? "true" : "false",
    (unsigned long)ESP.getMinFreeHeap(), resets,
    lightModeName(), (unsigned long)(lightSecToday / 60),
    lightsLow ? "true" : "false",
    lightsOnMinute() / 60, lightsOnMinute() % 60,
    lightsOffMinute() / 60, lightsOffMinute() % 60,
    lightsHaveSunset() ? "true" : "false",
    wxCondKey((WxCond)predTomorrow().cond), predCondWord((WxCond)predTomorrow().cond),
    predConfWord(predTomorrow()),
    predTomorrow().predWh, predTomorrow().pct,
    pvGoodMin, pvVerdictName(),
    socStableVal, socTrendName(), pvFilt, loadFilt,
    srcPhase == SRCP_WAIT ? "starting" : "running",
    (unsigned long)((srcEverSet && millis() - srcSince < SOURCE_MIN_DWELL_MS)
                    ? (SOURCE_MIN_DWELL_MS - (millis() - srcSince)) / 1000UL : 0UL),
    loadCutoff ? "true" : "false", (unsigned)buzzMode, emergAlarm ? "true" : "false",
    bmsFresh() ? "true" : "false",
    bleStateName(bleState), (unsigned long)bleReconnects,
    WiFi.RSSI(), bufferedLines,
    (unsigned long)ESP.getFreeHeap(), (unsigned long)(millis() / 1000));
}

static int buildSampleJson(char* out, size_t cap, time_t t, const BmsData& b) {
  return snprintf(out, cap,
    "{\"t\":%ld,\"v\":%.2f,\"i\":%.2f,\"p\":%.1f,\"soc\":%u,"
    "\"pv\":%.1f,\"src\":%u,\"approx\":%s}",
    (long)t, b.packV, b.packI, b.packW, b.soc,
    pvW, (unsigned)srcActual, ntpSynced ? "false" : "true");
}

// ============================================================
//  SECTION 8 - FIREBASE OVER A PERSISTENT TLS SESSION
//
//  A fresh handshake costs ~1.5 s and ~40 kB, which is why v3
//  could not push faster than every 15 s. One session is opened
//  and reused; it is dropped and rebuilt on error, when it goes
//  stale, or when free heap falls below HEAP_TLS_FLOOR.
//
//  Only netTask may call these.
// ============================================================
static WiFiClientSecure* tls = nullptr;
static uint32_t tlsLastUse = 0;

static void tlsDrop(const char* why) {
  if (!tls) return;
  Serial.printf("[fb] TLS session dropped: %s\n", why);
  tls->stop();
  delete tls;
  tls = nullptr;
}

static bool tlsEnsure() {
  if (tls) {
    bool stale = (millis() - tlsLastUse) > TLS_IDLE_MAX_MS;
    if (stale || !tls->connected()) tlsDrop(stale ? "idle timeout" : "peer closed");
  }
  if (!tls && ESP.getFreeHeap() < HEAP_TLS_FLOOR) return false;
  if (!tls) {
    tls = new WiFiClientSecure();
    if (!tls) return false;
    tls->setInsecure();               // see README known limits
    tls->setTimeout(8);
  }
  return true;
}

static bool fbRequest(const char* method, const String& path, const String& body) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!tlsEnsure()) { Serial.println("[fb] low heap, push skipped"); return false; }

  // The path MUST start with '/'. An empty path used to yield
  //   https://<host>.json
  // which is a different HOSTNAME, not the database root -- DNS fails,
  // sendRequest returns <= 0, and the caller sees a permanent failure.
  // That is what silently broke every backlog upload: the root
  // multi-path PATCH is the one call that passes a root path.
  String p = path;
  if (p.length() == 0 || p[0] != '/') p = "/" + p;

  String url = "https://" + String(FIREBASE_HOST) + p + ".json";
  if (strlen(FIREBASE_AUTH)) url += "?auth=" + String(FIREBASE_AUTH);

  HTTPClient http;
  http.setReuse(true);                // keep the socket for the next push
  http.setConnectTimeout(6000);
  http.setTimeout(8000);
  if (!http.begin(*tls, url)) { tlsDrop("begin failed"); return false; }
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, body);
  http.end();

  if (code <= 0) { tlsDrop("transport error"); return false; }
  tlsLastUse = millis();
  return code >= 200 && code < 300;
}

static void pushLive() {
  static char buf[3200];
  buildLiveJson(buf, sizeof(buf));
  fbRequest("PUT", "/live", buf);
}

// Write one day's totals under an EXPLICIT date. Never derives the
// date from the current clock -- that was defect 3, root cause A.
static bool pushDailyFor(int32_t stamp, float chg, float dis, float harvest,
                         float peak, uint8_t minSoc, uint32_t utilMin, bool closed) {
  if (!stamp) return false;
  char date[12]; stampToString(stamp, date);
  char body[280];
  snprintf(body, sizeof(body),
    "{\"chgWh\":%.1f,\"disWh\":%.1f,\"harvestWh\":%.1f,\"peakW\":%.1f,"
    "\"minSoc\":%u,\"utilMin\":%lu,\"closed\":%s}",
    chg, dis, harvest, peak, minSoc, (unsigned long)utilMin,
    closed ? "true" : "false");
  char path[24]; snprintf(path, sizeof(path), "/daily/%s", date);
  return fbRequest("PATCH", path, body);
}

// today's running totals, refreshed while the day is still open
static void pushDailyToday() {
  if (!dayStamp) return;
  pushDailyFor(dayStamp, todayChgWh, todayDisWh, harvestWhToday(),
               todayPeakW, todayMinSoc, utilitySecToday / 60, false);
}

// ============================================================
//  SECTION 9 - FLASH LOG STORE
// ============================================================
static void savePos() {
  File f = LittleFS.open(QUEUE_POS_FILE, "w");
  if (!f) return;
  f.print(queuePos);
  f.close();
}

static void loadPos() {
  File f = LittleFS.open(QUEUE_POS_FILE, "r");
  if (!f) { queuePos = prefs.getUInt("qp", 0); return; }
  queuePos = (uint32_t)f.readString().toInt();
  f.close();
}

static void countQueue() {
  bufferedLines = 0;
  if (!fsOk) return;
  FS_LOCK();
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (f) {
    if (queuePos > f.size()) queuePos = f.size();
    f.seek(queuePos);
    while (f.available()) { f.readStringUntil('\n'); bufferedLines++; }
    f.close();
  }
  FS_UNLOCK();
}

static void resetQueue() {
  LittleFS.remove(QUEUE_FILE);
  queuePos = 0;
  savePos();
  bufferedLines = 0;
}

static void compactQueue() {
  File in = LittleFS.open(QUEUE_FILE, "r");
  if (!in) return;
  if (queuePos >= in.size()) { in.close(); resetQueue(); return; }
  in.seek(queuePos);
  File out = LittleFS.open("/queue.tmp", "w");
  if (!out) { in.close(); return; }
  uint8_t buf[512];
  int r;
  while ((r = in.read(buf, sizeof(buf))) > 0) out.write(buf, r);
  out.close();
  in.close();
  LittleFS.remove(QUEUE_FILE);
  LittleFS.rename("/queue.tmp", QUEUE_FILE);
  queuePos = 0;
  savePos();
  Serial.println("[queue] compacted");
}

static void dropOldest() {
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) return;
  size_t target = f.size() / 4;
  f.seek(target);
  f.readStringUntil('\n');
  uint32_t cut = f.position();
  f.close();
  if (cut > queuePos) queuePos = cut;
  Serial.println("[queue] full, oldest quarter dropped");
  compactQueue();
}

static void appendHistoryCsv(time_t t, const BmsData& b) {
  char path[32];
  struct tm tmv; localtime_r(&t, &tmv);
  snprintf(path, sizeof(path), HISTORY_DIR "/%04d%02d%02d.csv",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  bool isNew = !LittleFS.exists(path);
  File f = LittleFS.open(path, "a");
  if (!f) return;
  if (isNew) f.println("t,v,i,p,soc,pv,src");
  f.printf("%ld,%.2f,%.2f,%.1f,%u,%.1f,%u\n",
           (long)t, b.packV, b.packI, b.packW, b.soc, pvW, (unsigned)srcActual);
  f.close();
}

static void pruneHistory() {
  if (!timeReady()) return;
  int32_t cutoff = dateStamp(nowEpoch() - (time_t)LOCAL_HISTORY_DAYS * 86400L);

  String doomed[8];
  int nDoomed = 0;

  File dir = LittleFS.open(HISTORY_DIR);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  File e = dir.openNextFile();
  while (e && nDoomed < 8) {
    String name = String(e.name());
    e.close();
    int slash = name.lastIndexOf('/');
    String base = slash >= 0 ? name.substring(slash + 1) : name;
    if (base.endsWith(".csv") && base.length() == 12) {
      int32_t stamp = base.substring(0, 8).toInt();
      if (stamp && stamp < cutoff) doomed[nDoomed++] = String(HISTORY_DIR "/") + base;
    }
    e = dir.openNextFile();
  }
  if (e) e.close();
  dir.close();

  for (int i = 0; i < nDoomed; i++) {
    LittleFS.remove(doomed[i]);
    Serial.printf("[hist] pruned %s\n", doomed[i].c_str());
  }
}

// daily.csv is the SOURCE OF TRUTH for monthly and yearly totals.
// It is written first, before anything tries to reach the network,
// so a rollover during an outage still produces a correct record.
static void appendDailyRow(int32_t stamp) {
  if (!fsOk || !stamp) return;
  char date[12]; stampToString(stamp, date);

  FS_LOCK();
  // Idempotence: never write the same day twice, however many times
  // a rollover is retried after a reboot.
  bool already = false;
  File chk = LittleFS.open(DAILY_FILE, "r");
  if (chk) {
    while (chk.available()) {
      String row = chk.readStringUntil('\n');
      if (row.startsWith(date)) { already = true; break; }
    }
    chk.close();
  }

  if (!already) {
    bool isNew = !LittleFS.exists(DAILY_FILE);
    File f = LittleFS.open(DAILY_FILE, "a");
    if (f) {
      if (isNew) f.println("date,chgWh,disWh,harvestWh,peakW,minSoc,utilMin");
      f.printf("%s,%.1f,%.1f,%.1f,%.1f,%u,%lu\n",
               date, todayChgWh, todayDisWh, harvestWhToday(), todayPeakW,
               todayMinSoc, (unsigned long)(utilitySecToday / 60));
      f.close();
      Serial.printf("[day] %s written to daily.csv\n", date);
    }
  }

  // prune: keep the header plus the newest DAILY_LOG_DAYS rows
  {
    int rows = 0;
    File in = LittleFS.open(DAILY_FILE, "r");
    if (in) {
      while (in.available()) { in.readStringUntil('\n'); rows++; }
      in.close();
    }
    if (rows > DAILY_LOG_DAYS + 1) {
      in = LittleFS.open(DAILY_FILE, "r");
      File out = LittleFS.open("/daily.tmp", "w");
      if (in && out) {
        out.println(in.readStringUntil('\n'));            // header
        int skip = rows - 1 - DAILY_LOG_DAYS;
        for (int i = 0; i < skip; i++) in.readStringUntil('\n');
        while (in.available()) {
          String row = in.readStringUntil('\n');
          row.trim();
          if (row.length()) out.println(row);
        }
      }
      if (in) in.close();
      if (out) {
        out.close();
        LittleFS.remove(DAILY_FILE);
        LittleFS.rename("/daily.tmp", DAILY_FILE);
      }
    }
  }
  pruneHistory();
  FS_UNLOCK();
}

static void logSample() {
  if (!fsOk) return;
  BmsData b = bmsGet();
  if (!b.lastFrameMs) return;
  time_t t = nowEpoch();
  char line[220];
  buildSampleJson(line, sizeof(line), t, b);

  FS_LOCK();
  File f = LittleFS.open(QUEUE_FILE, "a");
  if (f) {
    if (f.size() > QUEUE_MAX_BYTES) { f.close(); dropOldest(); f = LittleFS.open(QUEUE_FILE, "a"); }
    if (f) { f.println(line); f.close(); bufferedLines++; }
  }
  appendHistoryCsv(t, b);
  FS_UNLOCK();
}

// ============================================================
//  SECTION 10 - DAY ROLLOVER
//
//  Runs on the control task only. The day being closed is always
//  named by dayStamp, never by the clock, so a rollover noticed
//  late (reboot, outage, NTP step) still lands on the right date.
// ============================================================
static void rolloverCheck() {
  if (!timeReady()) return;
  int32_t ds = dateStamp(nowEpoch());
  if (dayStamp == 0) { dayStamp = ds; return; }
  if (ds == dayStamp) return;

  Serial.printf("[day] rollover %ld -> %ld, harvest %.1f Wh\n",
                (long)dayStamp, (long)ds, harvestWhToday());

  // 1. durable record first. netTask picks it up from here whenever
  //    the network allows, so an outage cannot lose the day.
  appendDailyRow(dayStamp);

  // 2. score the finished day against the label that was issued for
  //    it, BEFORE the history store moves on. Display-only: this
  //    updates the outlook screen's bias table and nothing else.
  predLearn(dayStamp, harvestWhToday());

  // 3. today's measured PV evidence becomes part of tomorrow's
  //    expectation. This is the only writer of the history store,
  //    and it runs once per day boundary.
  pvHistPut(dayStamp, pvGoodMin, harvestWhToday());

  // 4. reset for the new day
  todayChgWh = todayDisWh = todayPeakW = todayPvWh = 0;
  todayMinSoc = 100;
  utilitySecToday = 0;
  lightSecToday = 0;
  pvGoodMin = 0;                   // today's PV evidence starts empty
  pvAccumMs = 0;
  // CEB is NOT reset here. It is a live electrical decision:
  // if the pack is still under SOC_CEB_ON at midnight the house
  // must stay on mains across the date boundary. decideSource()
  // releases it on its own terms once the pack recovers.
  dayStamp = ds;
  saveCounters();
}

// Push the oldest finished day that has not reached Firebase yet.
// Returns false when there is nothing left to send.
//
// This is what makes monthly and yearly totals survive an outage:
// daily.csv is written at rollover regardless of the network, and
// daySynced (in NVS) only advances on a confirmed 2xx, so every day
// eventually lands under its own correct date exactly once.
static bool syncOneDay() {
  if (!fsOk || !timeReady()) return false;

  int32_t bestStamp = 0;
  float chg = 0, dis = 0, har = 0, peak = 0;
  int   minSoc = 100;
  long  utilMin = 0;

  FS_LOCK();
  File f = LittleFS.open(DAILY_FILE, "r");
  if (f) {
    f.readStringUntil('\n');                       // header
    while (f.available()) {
      String row = f.readStringUntil('\n');
      row.trim();
      if (row.length() < 12) continue;
      int32_t stamp = row.substring(0, 4).toInt() * 10000 +
                      row.substring(5, 7).toInt() * 100 +
                      row.substring(8, 10).toInt();
      if (stamp <= daySynced) continue;            // already in the cloud
      if (bestStamp && stamp >= bestStamp) continue;
      int c[6], ci = 0;
      for (int i = 0; i < (int)row.length() && ci < 6; i++)
        if (row[i] == ',') c[ci++] = i;
      if (ci < 6) continue;
      bestStamp = stamp;                           // oldest unsynced wins
      chg  = row.substring(c[0] + 1, c[1]).toFloat();
      dis  = row.substring(c[1] + 1, c[2]).toFloat();
      har  = row.substring(c[2] + 1, c[3]).toFloat();
      peak = row.substring(c[3] + 1, c[4]).toFloat();
      minSoc  = row.substring(c[4] + 1, c[5]).toInt();
      utilMin = row.substring(c[5] + 1).toInt();
    }
    f.close();
  }
  FS_UNLOCK();

  if (!bestStamp) return false;
  if (!pushDailyFor(bestStamp, chg, dis, har, peak,
                    (uint8_t)minSoc, (uint32_t)utilMin, true)) return false;

  daySynced = bestStamp;
  prefs.putInt("dsyn", daySynced);
  Serial.printf("[day] %ld synced to Firebase\n", (long)bestStamp);
  return true;
}

// A few per call, so a first boot against an existing daily.csv (or
// a long outage) backfills in minutes rather than hours, without
// ever monopolising netTask.
#define DAILY_SYNC_PER_PASS 4
static void syncPendingDaily() {
  for (int i = 0; i < DAILY_SYNC_PER_PASS; i++) {
    if (!syncOneDay()) return;
    esp_task_wdt_reset();
  }
}

// ============================================================
//  SECTION 11 - BULK UPLOAD (history backlog)
// ============================================================
static void uploadTask() {
  if (!fsOk || WiFi.status() != WL_CONNECTED) return;
  if (!timeReady()) return;

  String body = "{";
  int n = 0;
  size_t consumed = 0;

  FS_LOCK();
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) { FS_UNLOCK(); return; }
  size_t size = f.size();
  if (queuePos > size) queuePos = size;
  if (queuePos >= size) {
    f.close();
    if (size) resetQueue();
    FS_UNLOCK();
    return;
  }
  f.seek(queuePos);

  while (f.available() && n < QUEUE_BATCH) {
    String line = f.readStringUntil('\n');
    consumed += line.length() + 1;
    line.trim();
    if (line.length() < 10) continue;
    long ts = jsonLong(line, "t");
    if (line.indexOf("\"approx\":true") >= 0 && clockAdj) {
      ts += clockAdj;
      line = withTs(line, ts);
    }
    if (ts < 1700000000L) continue;
    char date[12]; dateString((time_t)ts, date);
    if (n) body += ",";
    body += "\"history/" + String(date) + "/" + String(ts) + "\":" + line;
    n++;
  }
  f.close();
  FS_UNLOCK();
  body += "}";

  if (!consumed) return;
  // Root-level multi-path PATCH: https://<host>/.json with a body of
  // {"history/<date>/<ts>": {...}, ...}. The path is "/", not "" --
  // see the normalisation note in fbRequest().
  bool ok = n ? fbRequest("PATCH", "/", body) : true;
  if (!ok) return;

  queuePos += consumed;
  if (bufferedLines >= n) bufferedLines -= n; else bufferedLines = 0;

  FS_LOCK();
  savePos();
  if (queuePos >= QUEUE_COMPACT_BYTES) compactQueue();
  FS_UNLOCK();

  if (n) Serial.printf("[queue] uploaded %d, %u still waiting\n", n, bufferedLines);
}

// ============================================================
//  SECTION 11b - EXPECTED SOLAR  (measured PV only, no internet)
//
//  v7 deleted the Open-Meteo client entirely: the HTTP fetch, the
//  API host/coordinates, the WMO code mapping, the precipitation
//  and cloud thresholds, the 7-day NVS forecast cache and the
//  netTask hook that drove it. Nothing in this file opens a socket
//  to a weather service any more, and no weather failure can reach
//  the source state machine because there is no weather service.
//
//  What remains is the part that was working: the array itself.
//  integrateEnergy() already counted the minutes the array spends
//  above PV_GOOD_W (config.h 8f). That counter is now
//
//    - scored 0..100 against a full-strength day,
//    - kept for PV_HIST_DAYS finished days in NVS (8g),
//    - and combined with today's running count to give an
//      expectation that exists before the day has developed.
//
//  Everything the OLED and the dashboard label as SUNNY / PARTLY
//  CLOUDY / CLOUDY / LOW SOLAR comes from here. They are expected
//  solar conditions inferred from generation, not meteorological
//  observations, and the wording in the UI says so.
//
//  This module can produce no answer at all -- a fresh install has
//  no history -- and the source state machine still works, just
//  with WX_UNKNOWN, which selects the solar-first 08:30 deadline.
//  Measured SoC and measured charge current decide what matters.
// ============================================================

const char* wxName(WeatherClass w) {
  return w == WX_CLEAR ? "clear" : w == WX_HALF ? "half"
       : w == WX_HEAVY ? "heavy" : "unknown";
}

const char* wxIconName(WxIcon i) {
  switch (i) {
    case WI_SUNNY:  return "SUNNY";
    case WI_PARTLY: return "PARTLY CLOUDY";
    case WI_CLOUDY: return "CLOUDY";
    case WI_LOWSUN: return "LOW SOLAR";
    default:        return "UNKNOWN";
  }
}

// Short form for the 128 px OLED at text size 2 (10 chars max).
const char* wxIconShort(WxIcon i) {
  switch (i) {
    case WI_SUNNY:  return "SUNNY";
    case WI_PARTLY: return "PARTLY";
    case WI_CLOUDY: return "CLOUDY";
    case WI_LOWSUN: return "LOW SUN";
    default:        return "--";
  }
}

// Machine-readable key for the dashboard, so the web UI and the
// OLED render the SAME condition from the SAME firmware field.
const char* wxIconKey(WxIcon i) {
  switch (i) {
    case WI_SUNNY:  return "sunny";
    case WI_PARTLY: return "partly";
    case WI_CLOUDY: return "cloudy";
    case WI_LOWSUN: return "lowsolar";
    default:        return "unknown";
  }
}

// ---- the history store -----------------------------------------
// One blob in the Preferences namespace the energy counters already
// use. No new storage subsystem, no second database.
static void pvHistSave() {
  prefs.putBytes(PV_HIST_NVS_KEY, pvHist, sizeof(pvHist));
}

static void pvHistLoad() {
  size_t n = prefs.getBytesLength(PV_HIST_NVS_KEY);
  if (n == sizeof(pvHist)) {
    prefs.getBytes(PV_HIST_NVS_KEY, pvHist, sizeof(pvHist));
    int have = 0;
    for (int i = 0; i < PV_HIST_DAYS; i++) if (pvHist[i].stamp) have++;
    Serial.printf("K: PV history restored, %d day(s)\n", have);
    return;
  }
  memset(pvHist, 0, sizeof(pvHist));
  Serial.println("K: PV history empty, expectation stays UNKNOWN until it fills");
}

// Minutes above PV_GOOD_W -> 0..100 strength. See config.h 8f for
// the measured days this scale was fitted to.
static uint8_t pvScoreOf(int mins) {
  if (mins <= 0) return 0;
  long v = (long)mins * 100L / (long)PV_SCORE_FULL_MIN;
  return v > 100 ? 100 : (uint8_t)v;
}

// YYYYMMDD -> whole days, so ages can be compared across months.
static long stampToDays(int32_t st) {
  if (st <= 0) return 0;
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = (int)(st / 10000) - 1900;
  t.tm_mon  = (int)((st / 100) % 100) - 1;
  t.tm_mday = (int)(st % 100);
  t.tm_hour = 12;                       // midday: immune to DST edges
  time_t e = mktime(&t);
  return e <= 0 ? 0 : (long)(e / 86400L);
}

// Record a finished day. Called once, from rolloverCheck().
static void pvHistPut(int32_t stamp, int mins, float wh) {
  if (stamp <= 0) return;
  int same = -1, empty = -1, oldest = 0;
  for (int i = 0; i < PV_HIST_DAYS; i++) {
    if (pvHist[i].stamp == stamp) same = i;
    if (empty < 0 && pvHist[i].stamp == 0) empty = i;
    if (pvHist[i].stamp < pvHist[oldest].stamp) oldest = i;
  }
  int slot = same >= 0 ? same : (empty >= 0 ? empty : oldest);
  if (mins < 0)   mins = 0;
  if (mins > 255) mins = 255;
  if (wh < 0)      wh = 0;
  if (wh > 65535.0f) wh = 65535.0f;
  pvHist[slot].stamp = stamp;
  pvHist[slot].mins  = (uint8_t)mins;
  pvHist[slot].score = pvScoreOf(mins);
  pvHist[slot].wh    = (uint16_t)(wh + 0.5f);
  pvHistSave();
  Serial.printf("[pv] %ld logged: %d min above %.0f W -> score %u, %u Wh\n",
                (long)stamp, mins, PV_GOOD_W, (unsigned)pvHist[slot].score,
                (unsigned)pvHist[slot].wh);
}

// How many trustworthy days the store holds right now.
static int pvHistDays() {
  if (!timeReady()) return 0;
  long today = stampToDays(dateStamp(nowEpoch()));
  int n = 0;
  for (int i = 0; i < PV_HIST_DAYS; i++) {
    if (pvHist[i].stamp == 0) continue;
    long age = today - stampToDays(pvHist[i].stamp);
    if (age >= 1 && age <= PV_HIST_TRUST_DAYS) n++;
  }
  return n;
}

// Median daily harvest over the trusted history, in Wh. The MEDIAN
// rather than the mean because one washout or one exceptional day
// should not drag the baseline the whole outlook rests on.
//
// Returns false when the store holds nothing usable.
static bool pvHistMedianWh(float* out, int* daysOut) {
  if (!timeReady()) return false;
  long today = stampToDays(dateStamp(nowEpoch()));
  uint16_t v[PV_HIST_DAYS];
  int n = 0;
  for (int i = 0; i < PV_HIST_DAYS; i++) {
    if (pvHist[i].stamp == 0) continue;
    long age = today - stampToDays(pvHist[i].stamp);
    if (age < 1 || age > PV_HIST_TRUST_DAYS) continue;
    v[n++] = pvHist[i].wh;
  }
  if (daysOut) *daysOut = n;
  if (n < 1) return false;
  for (int i = 1; i < n; i++) {                 // insertion sort, n <= 14
    uint16_t k = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  *out = (n & 1) ? (float)v[n / 2]
                 : (float)(v[n / 2 - 1] + v[n / 2]) / 2.0f;
  return *out > 0;
}

// ---- the prediction --------------------------------------------
//
// Baseline: yesterday weighted against the recent mean. Solar
// conditions are strongly autocorrelated day to day -- a monsoon
// spell is a spell, not a coin flip -- which is the entire basis
// for predicting anything without an external forecast, and it is
// why the weight on yesterday is only half rather than all.
//
// Returns false when the store is too thin to say anything. That
// is reported as UNKNOWN, not guessed at.
static bool pvBaseline(int* out) {
  if (!timeReady()) return false;
  int32_t today = dateStamp(nowEpoch());
  long    tD    = stampToDays(today);
  long sum = 0; int n = 0, yest = -1;
  for (int i = 0; i < PV_HIST_DAYS; i++) {
    if (pvHist[i].stamp == 0 || pvHist[i].stamp >= today) continue;
    long age = tD - stampToDays(pvHist[i].stamp);
    if (age < 1 || age > PV_HIST_TRUST_DAYS) continue;
    sum += pvHist[i].score;
    n++;
    if (age == 1) yest = pvHist[i].score;
  }
  if (n < PV_PRED_MIN_DAYS) return false;
  int mean = (int)(sum / n);
  *out = (yest >= 0)
       ? (PV_PRED_YEST_PCT * yest + (100 - PV_PRED_YEST_PCT) * mean) / 100
       : mean;
  return true;
}

// How far through the PV counting window we are, 0..100.
static int pvWindowPct() {
  struct tm t;
  if (!localNow(&t)) return 0;
  int nowMin = t.tm_hour * 60 + t.tm_min;
  int a = PV_WINDOW_START_HOUR * 60, b = PV_WINDOW_END_HOUR * 60;
  if (nowMin <= a) return 0;
  if (nowMin >= b) return 100;
  return (nowMin - a) * 100 / (b - a);
}

// Today's evidence so far, extrapolated to a whole window.
static int pvLiveScore() {
  int pct = pvWindowPct();
  if (pct <= 0) return 0;
  long full = (long)pvGoodMin * 100L / (long)pct;
  return (int)pvScoreOf((int)full);
}

// The expected-solar score the whole system reports, -1 = unknown.
// Pure baseline before the window opens, pure measurement once it
// has closed, a straight-line blend in between -- so there is no
// step change at any hour and no second calculation anywhere.
static int pvExpectedScore() {
  int  base = 0;
  bool haveBase = pvBaseline(&base);
  int  pct = pvWindowPct();
  if (pct <= 0) return haveBase ? base : -1;
  int live = pvLiveScore();
  if (!haveBase) return pct >= 50 ? live : -1;   // half a window is evidence
  return (live * pct + base * (100 - pct)) / 100;
}

// After 20:00 the overnight CEB period belongs to TOMORROW's solar
// day, so the morning handover must be judged on tomorrow. Today's
// finished measurement has not reached the history store yet, so it
// is folded in here with the same weight yesterday normally gets.
static int pvTomorrowScore() {
  int  base = 0;
  bool haveBase = pvBaseline(&base);
  int  today = (int)pvScoreOf(pvGoodMin);
  if (!haveBase) return pvWindowPct() >= 100 ? today : -1;
  return (PV_PRED_YEST_PCT * today + (100 - PV_PRED_YEST_PCT) * base) / 100;
}

static WxIcon pvIconFromScore(int sc) {
  if (sc < 0)                 return WI_UNKNOWN;
  if (sc >= PV_SCORE_SUNNY)   return WI_SUNNY;
  if (sc >= PV_SCORE_PARTLY)  return WI_PARTLY;
  if (sc >= PV_SCORE_CLOUDY)  return WI_CLOUDY;
  return WI_LOWSUN;
}

// The icon the OLED draws and the dashboard renders. One function,
// one source, so the two ends cannot disagree.
WxIcon wxIconNow() { return pvIconFromScore(pvExpectedScore()); }

// PREDICTED class from MEASURED history, for the CEB decision in
// progress. The mapping to WeatherClass is deliberately unchanged
// from v6 so the release deadlines keep their existing meaning:
// only LOW SOLAR earns the 18:15 evening handover, everything else
// hands back at 08:30.
//
// Named for its evidence, not its role: wxForecastClass() below is
// the ONLINE forecast, and the two must never be confused.
static WeatherClass pvPredictedClass() {
  struct tm t;
  int sc = (localNow(&t) && t.tm_hour >= 20) ? pvTomorrowScore() : pvExpectedScore();
  if (sc < 0)                return WX_UNKNOWN;
  if (sc >= PV_SCORE_PARTLY) return WX_CLEAR;
  if (sc >= PV_SCORE_CLOUDY) return WX_HALF;
  return WX_HEAVY;
}

// Is the PV prediction backed by enough measured days to be worth
// anything? Reported to the dashboard so the UI can say so.
static bool wxTrusted() { int x; return pvBaseline(&x); }


// ============================================================
//  SECTION 11c - ONLINE FORECAST  (the subordinate second opinion)
//
//  This module answers one question -- "what does a weather service
//  think the sky will do?" -- and it is never allowed to answer any
//  other. In particular it cannot reach a relay: wxEffectiveClass()
//  below is the only consumer, it only ever picks a release
//  DEADLINE, and today's measured generation outranks it.
//
//  Everything here is confined to netTask. No socket is opened from
//  the control loop, a BLE callback or a web handler, the Firebase
//  TLS session is never touched, and a total failure of the weather
//  service costs the system nothing but a stale cache entry that
//  then decays to zero influence on its own (config.h 8k).
//
//  See config.h 8d for why it is subordinate and 8k for the
//  arithmetic that keeps it there.
// ============================================================

const char* wxCondName(WxCond c) {
  switch (c) {
    case WC_SUNNY:     return "SUNNY";
    case WC_PARTLY:    return "PARTLY CLOUDY";
    case WC_CLOUDY:    return "CLOUDY";
    case WC_FOG:       return "FOG";
    case WC_RAIN:      return "RAIN";
    case WC_HEAVYRAIN: return "HEAVY RAIN";
    case WC_STORM:     return "THUNDERSTORM";
    default:           return "UNKNOWN";
  }
}

// Short form for the 128 px OLED at text size 2 (10 chars max).
const char* wxCondShort(WxCond c) {
  switch (c) {
    case WC_SUNNY:     return "SUNNY";
    case WC_PARTLY:    return "PARTLY";
    case WC_CLOUDY:    return "CLOUDY";
    case WC_FOG:       return "FOG";
    case WC_RAIN:      return "RAIN";
    case WC_HEAVYRAIN: return "HVY RAIN";
    case WC_STORM:     return "STORM";
    default:           return "--";
  }
}

const char* wxCondKey(WxCond c) {
  switch (c) {
    case WC_SUNNY:     return "sunny";
    case WC_PARTLY:    return "partly";
    case WC_CLOUDY:    return "cloudy";
    case WC_FOG:       return "fog";
    case WC_RAIN:      return "rain";
    case WC_HEAVYRAIN: return "heavyrain";
    case WC_STORM:     return "storm";
    default:           return "unknown";
  }
}

// WMO code -> condition. Snow codes land on CLOUDY: they cannot
// occur here, and inventing a snow icon for a tropical valley would
// be decoration rather than information.
static WxCond wxCondFromCode(int code) {
  switch (code) {
    case 0: case 1:                       return WC_SUNNY;
    case 2:                               return WC_PARTLY;
    case 3:                               return WC_CLOUDY;
    case 45: case 48:                     return WC_FOG;
    case 51: case 53: case 55:
    case 56: case 57:
    case 61: case 63: case 66:
    case 80: case 81:                     return WC_RAIN;
    case 65: case 67: case 82:            return WC_HEAVYRAIN;
    case 95: case 96: case 99:            return WC_STORM;
    case 71: case 73: case 75: case 77:
    case 85: case 86:                     return WC_CLOUDY;
    default:                              return WC_UNKNOWN;
  }
}

// Nominal cloud cover for a condition, used only when the API omits
// cloud_cover_mean. Keeps the class rule below total rather than
// having it fall through a hole in the response.
static uint8_t wxCloudFromCond(WxCond c) {
  switch (c) {
    case WC_SUNNY:     return 10;
    case WC_PARTLY:    return 45;
    case WC_CLOUDY:    return 90;
    case WC_FOG:       return 90;
    case WC_RAIN:      return 90;
    case WC_HEAVYRAIN: return 95;
    case WC_STORM:     return 95;
    default:           return 50;
  }
}

// ---- the cache -------------------------------------------------
static void wxSave() {
  prefs.putBytes(WEATHER_NVS_KEY, wxCache, sizeof(wxCache));
  prefs.putInt("wxf", wxFetchedStamp);
}

static void wxLoad() {
  size_t n = prefs.getBytesLength(WEATHER_NVS_KEY);
  if (n == sizeof(wxCache)) {
    prefs.getBytes(WEATHER_NVS_KEY, wxCache, sizeof(wxCache));
    wxFetchedStamp = prefs.getInt("wxf", 0);
    int have = 0;
    for (int i = 0; i < WEATHER_DAYS; i++) if (wxCache[i].stamp) have++;
    Serial.printf("K: forecast cache restored, %d day(s), fetched %ld\n",
                  have, (long)wxFetchedStamp);
    return;
  }
  memset(wxCache, 0, sizeof(wxCache));
  wxFetchedStamp = 0;
  Serial.println("K: forecast cache empty");
}

// Date-indexed lookup. A reboot or a missed refresh cannot shift
// the alignment and apply one day's forecast to another.
static const WxDay* wxFind(int32_t stamp) {
  if (stamp <= 0) return nullptr;
  for (int i = 0; i < WEATHER_DAYS; i++)
    if (wxCache[i].stamp == stamp) return &wxCache[i];
  return nullptr;
}

// Whole days since the cache was last successfully refreshed.
// -1 when it has never been filled. Age is measured from the FETCH,
// not from the day: a 7-day forecast pulled this morning knows
// about Friday at age 0.
int wxAgeDays() {
  if (!wxFetchedStamp || !timeReady()) return -1;
  long a = stampToDays(dateStamp(nowEpoch()));
  long b = stampToDays(wxFetchedStamp);
  if (!a || !b) return -1;
  long d = a - b;
  return d < 0 ? 0 : (int)d;
}

// The forecast record that applies to the solar day the CEB machine
// is currently reasoning about -- tomorrow once the overnight CEB
// period has begun, today otherwise. Mirrors pvTomorrowScore().
static const WxDay* wxRelevant() {
  if (!timeReady()) return nullptr;
  struct tm t;
  if (!localNow(&t)) return nullptr;
  time_t base = nowEpoch();
  if (t.tm_hour >= 20) base += 86400;
  return wxFind(dateStamp(base));
}

// ---- confidence (config.h 8k) ----------------------------------
//
// Linear decay to zero at WEATHER_TRUST_DAYS. Requirement 19 falls
// straight out of this: five days without WiFi and the forecast
// carries no weight at all, so real-time PV and the measured
// history are all that remain.
int fcConfidence() {
  const WxDay* d = wxRelevant();
  if (!d || d->cond == WC_UNKNOWN) return 0;
  int age = wxAgeDays();
  if (age < 0 || age >= WEATHER_TRUST_DAYS) return 0;
  return WX_CONF_FRESH * (WEATHER_TRUST_DAYS - age) / WEATHER_TRUST_DAYS;
}

// Grows with the evidence behind it: trusted history days, plus how
// much of today's counting window has actually been measured.
int pvConfidence() {
  int base;
  if (!pvBaseline(&base)) return 0;
  int days = pvHistDays();
  if (days > PV_CONF_DAY_CAP) days = PV_CONF_DAY_CAP;
  int c = PV_CONF_BASE + PV_CONF_PER_DAY * days
        + PV_CONF_WINDOW * pvWindowPct() / 100;
  return c > 100 ? 100 : c;
}

// ---- forecast -> WeatherClass ----------------------------------
// The same precipitation and cloud thresholds v6 used, so the
// release-deadline policy is unchanged. Only its standing changed.
static WeatherClass wxClassOf(const WxDay* d) {
  if (!d || d->cond == WC_UNKNOWN) return WX_UNKNOWN;
  float mm    = d->precip10 / 10.0f;
  int   cloud = d->cloud;
  if (mm >= WX_HEAVY_PRECIP_MM || cloud >= WX_HEAVY_CLOUD_PCT) return WX_HEAVY;
  if (cloud <= WX_CLEAR_CLOUD_PCT && mm < 1.0f)                 return WX_CLEAR;
  return WX_HALF;
}

// The ONLINE forecast's opinion about the solar day in progress.
static WeatherClass wxForecastClass() { return wxClassOf(wxRelevant()); }

// The condition to display. WC_UNKNOWN when there is no forecast
// worth showing, in which case the UI falls back to the measured
// expectation rather than inventing weather.
WxCond wxCondNow() {
  if (fcConfidence() <= 0) return WC_UNKNOWN;
  const WxDay* d = wxRelevant();
  return d ? (WxCond)d->cond : WC_UNKNOWN;
}

// Cloud cover and rainfall behind the forecast, for the dashboard.
// -1 / 0 when there is no trustworthy forecast to quote.
int wxRelevantCloud() {
  const WxDay* d = (fcConfidence() > 0) ? wxRelevant() : nullptr;
  return d ? (int)d->cloud : -1;
}

float wxRelevantRain() {
  const WxDay* d = (fcConfidence() > 0) ? wxRelevant() : nullptr;
  return d ? d->precip10 / 10.0f : 0.0f;
}

// Do the two opinions agree? Reported, never acted on.
const char* wxAgreement() {
  WeatherClass f = wxForecastClass(), p = pvPredictedClass();
  if (f == WX_UNKNOWN || p == WX_UNKNOWN) return "n/a";
  return f == p ? "agree" : "differ";
}

// Which evidence actually decided the class in force right now.
const char* wxDecisionSrc() {
  if (pvVerdictGood() || pvVerdictPoor()) return "measured";
  int pc = pvConfidence(), fc = fcConfidence();
  if (pc == 0 && fc == 0) return "none";
  return fc > pc ? "forecast" : "pv-history";
}

// ---- the fetch (netTask only) ----------------------------------
//
// A hand-rolled reader rather than a JSON library: the response is
// three flat arrays of known shape, and 40 kB of parser for that
// would be heap this device does not have to spare.

// Numeric element i of the array that follows "key".
static bool jsonArrNum(const String& body, const char* key, int i, float* out) {
  int k = body.indexOf(key);
  if (k < 0) return false;
  int a = body.indexOf('[', k);
  int b = body.indexOf(']', a);
  if (a < 0 || b < 0) return false;
  int p = a + 1;
  for (int n = 0; n < i; n++) {
    p = body.indexOf(',', p);
    if (p < 0 || p > b) return false;
    p++;
  }
  while (p < b && (body[p] == ' ' || body[p] == '"')) p++;
  if (p >= b) return false;
  if (body[p] == 'n') return false;                 // JSON null
  *out = body.substring(p, b).toFloat();
  return true;
}

// "....THH:MM" element i of a string array -> minutes past local
// midnight, 0 when absent. Open-Meteo returns sunset as a local
// ISO timestamp, so only the clock part is needed.
static uint16_t jsonArrHHMM(const String& body, const char* key, int i) {
  int k = body.indexOf(key);
  if (k < 0) return 0;
  int a = body.indexOf('[', k);
  int b = body.indexOf(']', a);
  if (a < 0 || b < 0) return 0;
  int p = a + 1;
  for (int n = 0; n < i; n++) {
    p = body.indexOf(',', p);
    if (p < 0 || p > b) return 0;
    p++;
  }
  int t = body.indexOf('T', p);
  if (t < 0 || t + 6 > b) return 0;
  int hh = body.substring(t + 1, t + 3).toInt();
  int mm = body.substring(t + 4, t + 6).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return 0;
  int v = hh * 60 + mm;
  return v ? (uint16_t)v : 1;          // 0 is the "absent" sentinel
}

// "YYYY-MM-DD" element i of the "time" array -> YYYYMMDD.
static int32_t jsonArrDate(const String& body, int i) {
  int k = body.indexOf("\"time\"");
  if (k < 0) return 0;
  int a = body.indexOf('[', k);
  int b = body.indexOf(']', a);
  if (a < 0 || b < 0) return 0;
  int p = a + 1;
  for (int n = 0; n < i; n++) {
    p = body.indexOf(',', p);
    if (p < 0 || p > b) return 0;
    p++;
  }
  int q = body.indexOf('"', p);
  if (q < 0 || q + 11 > b) return 0;
  String d = body.substring(q + 1, q + 11);         // YYYY-MM-DD
  if (d.length() != 10) return 0;
  return d.substring(0, 4).toInt() * 10000 +
         d.substring(5, 7).toInt() * 100 +
         d.substring(8, 10).toInt();
}

// One HTTPS GET on its own short-lived client. The Firebase session
// object is deliberately not reused and not dropped -- requirement
// 35: the upload path is untouched by anything in this module.
static bool weatherFetch() {
#if !WEATHER_ENABLE
  return false;
#else
  if (WiFi.status() != WL_CONNECTED || !timeReady()) return false;
  // A forecast is never worth crowding the heap the BLE stack and
  // the Firebase session need. Skip and try again later.
  if (ESP.getFreeHeap() < HEAP_TLS_FLOOR + 20000UL) {
    Serial.println("[wx] skipped, heap low");
    return false;
  }

  String url = String("https://") + WEATHER_HOST +
               "/v1/forecast?latitude=" + WEATHER_LAT +
               "&longitude=" + WEATHER_LON +
               "&daily=weather_code,cloud_cover_mean,precipitation_sum,sunset"
               "&timezone=" + WEATHER_TZ +
               "&forecast_days=" + String(WEATHER_DAYS);

  WiFiClientSecure* c = new WiFiClientSecure();
  if (!c) return false;
  c->setInsecure();                       // same posture as Firebase
  c->setTimeout(WEATHER_HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(WEATHER_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(WEATHER_HTTP_TIMEOUT_MS);
  bool ok = false;
  String body;
  if (http.begin(*c, url)) {
    int code = http.GET();
    if (code == 200) { body = http.getString(); ok = body.length() > 40; }
    else Serial.printf("[wx] HTTP %d\n", code);
    http.end();
  }
  c->stop();
  delete c;
  if (!ok) return false;

  // Parse into a SCRATCH array and commit only if something real
  // came back. A truncated response can never overwrite a good
  // cache with holes.
  WxDay tmp[WEATHER_DAYS];
  memset(tmp, 0, sizeof(tmp));
  int filled = 0;
  for (int i = 0; i < WEATHER_DAYS; i++) {
    int32_t st = jsonArrDate(body, i);
    if (st <= 0) break;
    float code = 0, cloud = -1, precip = 0;
    if (!jsonArrNum(body, "\"weather_code\"", i, &code)) break;
    jsonArrNum(body, "\"cloud_cover_mean\"", i, &cloud);
    jsonArrNum(body, "\"precipitation_sum\"", i, &precip);
    WxCond cd = wxCondFromCode((int)code);
    tmp[i].stamp    = st;
    tmp[i].cond     = (uint8_t)cd;
    tmp[i].cloud    = (cloud < 0) ? wxCloudFromCond(cd)
                                  : (uint8_t)(cloud > 100 ? 100 : cloud);
    tmp[i].precip10 = (int16_t)(precip * 10.0f + 0.5f);
    tmp[i].sunsetMin = jsonArrHHMM(body, "\"sunset\"", i);
    filled++;
  }
  if (filled <= 0) { Serial.println("[wx] response unusable, cache kept"); return false; }

  memcpy(wxCache, tmp, sizeof(wxCache));
  wxFetchedStamp = dateStamp(nowEpoch());
  wxEverOk = true;
  wxSave();
  const WxDay* t0 = wxFind(wxFetchedStamp);
  Serial.printf("[wx] %d day(s) cached; today %s, %u%% cloud, %.1f mm, sunset %02u:%02u\n",
                filled, t0 ? wxCondName((WxCond)t0->cond) : "?",
                t0 ? t0->cloud : 0, t0 ? t0->precip10 / 10.0f : 0.0f,
                t0 ? t0->sunsetMin / 60 : 0, t0 ? t0->sunsetMin % 60 : 0);
  return true;
#endif
}

// Scheduler. Called from netTask only. Nothing here blocks the
// control loop and nothing here can run twice concurrently.
static void weatherTask() {
#if WEATHER_ENABLE
  uint32_t now = millis();

  // Without a clock a response cannot be filed against a date, and
  // the morning rule below has no meaning. Come back shortly rather
  // than burning a retry slot before NTP has landed.
  if (!timeReady()) { wxNextTry = now + 30000UL; return; }

  int32_t today = dateStamp(nowEpoch());
  bool due = (int32_t)(now - wxNextTry) >= 0;

  // One refresh each morning before the 08:30 handover decision, so
  // the day is never judged on yesterday's pull.
  //
  // wxLastMorning is stamped on the ATTEMPT, not on success. Marking
  // it only when the fetch worked would leave this condition true
  // all day after one failure, and since it bypasses wxNextTry the
  // task would then re-enter on every netTask pass -- a 10 Hz retry
  // storm against a public API. A failed morning attempt falls back
  // to the ordinary WEATHER_RETRY_MS schedule below, which still
  // gets the forecast in well before 08:30.
  if (!due) {
    struct tm t;
    if (localNow(&t) && t.tm_hour >= WEATHER_MORNING_HOUR && wxLastMorning != today)
      due = true;
  }
  if (!due) return;
  wxLastMorning = today;

  if (weatherFetch()) {
    wxFails = 0;
    wxNextTry = now + WEATHER_REFRESH_MS;
  } else {
    wxFails++;
    wxNextTry = now + WEATHER_RETRY_MS;
    Serial.printf("[wx] fetch failed (%lu in a row), retry in %lu min\n",
                  (unsigned long)wxFails, (unsigned long)(WEATHER_RETRY_MS / 60000UL));
  }
#endif
}

// ---- real-time PV verdict (unchanged from v6) --------------------
//
// The corrective signal that lets today's measured generation
// override the expectation in BOTH directions. Both verdicts read a
// counter that only rises within a day, so neither can oscillate and
// neither can chatter the relay.

// Enough sustained generation to call today genuinely good.
// Latches for the rest of the day.
static bool pvVerdictGood() { return pvGoodMin >= PV_GOOD_MINUTES; }

// Past the verdict hour with almost nothing in the bank: today is
// genuinely poor, whatever was expected.
static bool pvVerdictPoor() {
  struct tm tmv;
  if (!localNow(&tmv)) return false;
  return tmv.tm_hour >= PV_VERDICT_HOUR && pvGoodMin < PV_POOR_MINUTES;
}

const char* pvVerdictName() {
  return pvVerdictGood() ? "good" : pvVerdictPoor() ? "poor" : "pending";
}

// The class the CEB machine actually acts on. Three tiers, in
// order, exactly as config.h 8k describes them:
//
//   1. TODAY'S MEASUREMENT, once it has something to say. An array
//      that has genuinely worked for 90 minutes is not "expecting"
//      anything, it is reporting, and no label outranks that --
//      in either direction (requirement 16).
//   2. THE MORE CONFIDENT OF THE TWO PREDICTIONS. The PV history
//      normally wins; a fresh forecast wins on a young install
//      whose history cannot yet say anything (config.h 8k has the
//      worked numbers).
//   3. UNKNOWN when neither has any confidence, which selects the
//      conservative solar-first 08:30 deadline.
//
// This is the ONLY consumer of either prediction, and all it ever
// changes is the release DEADLINE. No branch here can move a relay.
static WeatherClass wxEffectiveClass() {
  if (pvVerdictGood()) return WX_CLEAR;   // real sun beats a gloomy label
  if (pvVerdictPoor()) return WX_HEAVY;   // real gloom beats a sunny label

  const int pc = pvConfidence();
  const int fc = fcConfidence();
  if (pc == 0 && fc == 0) return WX_UNKNOWN;
  if (fc > pc) {
    WeatherClass w = wxForecastClass();
    if (w != WX_UNKNOWN) return w;
  }
  return pvPredictedClass();
}


// ============================================================
//  SECTION 11d - TOMORROW'S OUTLOOK  (DISPLAY ONLY)
//
//  ###########################################################
//  #  NOTHING HERE IS CALLED FROM SECTION 12.                #
//  #  decideSource() reads wxEffectiveClass() and nothing    #
//  #  else. This module exists so a person can look at the   #
//  #  panel at 20:00 and know whether to expect a good day;  #
//  #  it has no vote in what the relay does.                 #
//  ###########################################################
//
//  See config.h 8m for the model. In one line:
//
//      predicted_Wh = median(history) x factor[label] x bias[label]
//
//  where factor is the label's nominal share of a clear day and
//  bias is what that label has actually delivered HERE, learned one
//  observation per night.
// ============================================================

// Nominal share of a clear day for a forecast label, x1000.
static uint16_t predFactor(WxCond c) {
  switch (c) {
    case WC_SUNNY:     return PRED_F_SUNNY;
    case WC_PARTLY:    return PRED_F_PARTLY;
    case WC_CLOUDY:    return PRED_F_CLOUDY;
    case WC_FOG:       return PRED_F_FOG;
    case WC_RAIN:      return PRED_F_RAIN;
    case WC_HEAVYRAIN: return PRED_F_HEAVYRAIN;
    case WC_STORM:     return PRED_F_STORM;
    default:           return 1000;             // no label: no adjustment
  }
}

// The words the brief asks for, which are not the same words the
// weather screen uses -- "MOSTLY SUN" reads better than "PARTLY"
// when it is paired with a solar verdict underneath.
const char* predCondWord(WxCond c) {
  switch (c) {
    case WC_SUNNY:     return "SUNNY";
    case WC_PARTLY:    return "MOSTLY SUN";
    case WC_CLOUDY:    return "CLOUDY";
    case WC_FOG:       return "CLOUDY";
    case WC_RAIN:      return "RAIN";
    case WC_HEAVYRAIN: return "HEAVY RAIN";
    case WC_STORM:     return "STORM";
    default:           return "NO DATA";
  }
}

// ---- the learned bias table ------------------------------------
static void predBiasSave() { prefs.putBytes(PRED_BIAS_NVS_KEY, wxBias, sizeof(wxBias)); }

static void predBiasLoad() {
  size_t n = prefs.getBytesLength(PRED_BIAS_NVS_KEY);
  if (n == sizeof(wxBias)) {
    prefs.getBytes(PRED_BIAS_NVS_KEY, wxBias, sizeof(wxBias));
    Serial.print("K: forecast bias restored:");
    for (int i = 1; i < 8; i++)
      if (wxBias[i].n) Serial.printf(" %s=%.2f(n%u)", wxCondName((WxCond)i),
                                     wxBias[i].f1000 / 1000.0f, (unsigned)wxBias[i].n);
    Serial.println();
    return;
  }
  for (int i = 0; i < 8; i++) { wxBias[i].f1000 = 1000; wxBias[i].n = 0; }
  Serial.println("K: forecast bias table empty, every label starts unbiased");
}

// The nightly observation. Called ONCE per rollover, from
// rolloverCheck(), before the day's counters are cleared.
//
// Scores the day that just ended against the label that was issued
// for it. A day with no cached label teaches nothing and is skipped
// rather than being folded in as if the forecast had said "clear".
static void predLearn(int32_t stamp, float actualWh) {
  const WxDay* d = wxFind(stamp);
  if (!d || d->cond == WC_UNKNOWN) {
    Serial.printf("[pred] %ld: no label was issued, nothing learned\n", (long)stamp);
    return;
  }
  float base;
  int days = 0;
  if (!pvHistMedianWh(&base, &days) || base <= 1.0f) {
    Serial.printf("[pred] %ld: no baseline yet (%d day(s)), nothing learned\n",
                  (long)stamp, days);
    return;
  }

  WxCond c   = (WxCond)d->cond;
  float expect = base * predFactor(c) / 1000.0f;
  if (expect < 1.0f) return;

  long ratio = (long)(actualWh * 1000.0f / expect);
  if (ratio < PRED_RATIO_MIN) ratio = PRED_RATIO_MIN;   // clamp: one freak
  if (ratio > PRED_RATIO_MAX) ratio = PRED_RATIO_MAX;   // day cannot dominate

  WxBias& b = wxBias[c];
  uint8_t n = b.n < PRED_BIAS_MAX ? b.n : PRED_BIAS_MAX;
  // Running mean, with the count capped so the table keeps tracking
  // a changing season instead of freezing after a month.
  b.f1000 = (uint16_t)(((long)b.f1000 * n + ratio) / (n + 1));
  if (b.n < 255) b.n++;
  predBiasSave();

  Serial.printf("[pred] %ld label=%s expected=%.0fWh actual=%.0fWh ratio=%.2f "
                "-> bias[%s]=%.2f (n=%u, baseline=%.0fWh over %d days)\n",
                (long)stamp, wxCondName(c), expect, actualWh, ratio / 1000.0f,
                wxCondName(c), b.f1000 / 1000.0f, (unsigned)b.n, base, days);
}

// ---- tomorrow's estimate ---------------------------------------
//
// Everything the outlook screen needs, computed in one place so the
// OLED and the dashboard cannot disagree.
// PredOut is declared in config.h; see the note there.
//
// dayOffset 0 = today, 1 = tomorrow. Today's estimate is what the
// dashboard's accuracy card compares against what the array
// actually produced; tomorrow's is what the OLED shows in the
// evening. One code path, so the two can never disagree.
static PredOut predFor(int dayOffset) {
  PredOut o;
  o.ok = false; o.learning = false; o.starred = false;
  o.cond = (uint8_t)WC_UNKNOWN; o.baseWh = 0; o.predWh = 0; o.pct = 0;

  int days = 0;
  if (!pvHistMedianWh(&o.baseWh, &days)) return o;
  o.ok = true;
  o.learning = days < PRED_MIN_DAYS;

  // The label for TOMORROW, and how much it is still worth. The
  // weight is the existing section-8k confidence: full when fresh,
  // zero once the cache passes WEATHER_TRUST_DAYS. So a forecast
  // older than a day blends its factor back toward 1.0 on its own,
  // and a five-day outage drops the label entirely -- no second
  // staleness rule anywhere in this module.
  const WxDay* t = nullptr;
  if (timeReady()) t = wxFind(dateStamp(nowEpoch() + (time_t)dayOffset * 86400));
  int w = fcConfidence();                        // 0..WX_CONF_FRESH

  if (!t || t->cond == WC_UNKNOWN || w <= 0) {
    o.starred = true;                            // historical median alone
    o.predWh  = o.baseWh;
    o.cond    = t ? t->cond : (uint8_t)WC_UNKNOWN;
  } else {
    o.cond = t->cond;
    long f    = predFactor((WxCond)o.cond);
    long bias = wxBias[o.cond].f1000 ? wxBias[o.cond].f1000 : 1000;
    long eff  = f * bias / 1000L;                // corrected factor
    // Blend toward "no opinion" as the forecast ages.
    long blended = 1000 + (eff - 1000) * w / WX_CONF_FRESH;
    o.predWh = o.baseWh * blended / 1000.0f;
  }

  o.pct = (int)(o.predWh * 100.0f / (o.baseWh > 1.0f ? o.baseWh : 1.0f));
  return o;
}

static PredOut predTomorrow() { return predFor(1); }
static PredOut predToday()    { return predFor(0); }

// Confidence word from predicted/baseline. Thresholds in config 8m.
const char* predConfWord(const PredOut& o) {
  if (!o.ok)       return "NO DATA";
  if (o.learning)  return "LEARNING";
  int r = o.pct * 10;                            // -> x1000
  if (r >= PRED_STRONG) return o.starred ? "STRONG SOLAR*" : "STRONG SOLAR";
  if (r >= PRED_GOOD)   return o.starred ? "GOOD SOLAR*"   : "GOOD SOLAR";
  if (r >= PRED_WEAK)   return o.starred ? "WEAK SOLAR*"   : "WEAK SOLAR";
  return o.starred ? "POOR SOLAR*" : "POOR SOLAR";
}

// ============================================================
//  SECTION 11e - BATTERY ETA  (DISPLAY ONLY)
//
//  Reads socStableVal and the pvFilt/loadFilt the controller
//  already maintains, takes a further ETA_AVG_MINUTES rolling mean
//  of them, and turns that into a time. It writes nothing any other
//  module reads and, like 11d, has no vote in the source decision.
// ============================================================

// Sampled on the control tick. One slot per minute, ETA_AVG_MINUTES
// slots, so the mean covers exactly the advertised window.
static void etaUpdate(uint32_t now) {
  if (!bmsFresh()) {                       // a gap must not average in as 0 W
    etaRingN = 0; etaRingHead = 0;
    etaAccL = etaAccC = 0; etaAccN = 0; etaAccMs = now;
    return;
  }
  etaAccL += loadFilt;
  etaAccC += pvFilt;
  etaAccN++;
  if (!etaAccMs) etaAccMs = now;
  if ((uint32_t)(now - etaAccMs) < 60000UL) return;

  etaRingL[etaRingHead] = etaAccN ? etaAccL / etaAccN : 0;
  etaRingC[etaRingHead] = etaAccN ? etaAccC / etaAccN : 0;
  etaRingHead = (uint8_t)((etaRingHead + 1) % ETA_AVG_MINUTES);
  if (etaRingN < ETA_AVG_MINUTES) etaRingN++;
  etaAccL = etaAccC = 0; etaAccN = 0; etaAccMs = now;
}

// Mean of the ring, falling back to the partial minute so the
// screen says something sensible in the first 60 s after boot.
static float etaMean(const float* ring, float partial) {
  if (etaRingN == 0) return partial;
  float sum = 0;
  for (uint8_t i = 0; i < etaRingN; i++) sum += ring[i];
  return sum / etaRingN;
}

// EtaKind and EtaOut are declared in config.h; see the note there.
static EtaOut etaCompute() {
  EtaOut e;
  e.kind = ETA_NONE; e.hours = 0; e.hh = e.mm = 0;

  int soc = socStableVal;
  if (!bmsFresh() || soc < 0) return e;

  float load = etaMean(etaRingL, etaAccN ? etaAccL / etaAccN : loadFilt);
  float chg  = etaMean(etaRingC, etaAccN ? etaAccC / etaAccN : pvFilt);

  if (chg >= ETA_MIN_W && chg > load) {
    if (soc >= ETA_FULL_SOC) { e.kind = ETA_FULL_NOW; return e; }
    float need = PACK_USABLE_WH * (100 - soc) / 100.0f;
    float h = need / chg;
    if (h <= 0 || h > ETA_MAX_HOURS) { e.hours = h; e.kind = ETA_NONE; return e; }
    struct tm t;
    if (!localNow(&t)) return e;                 // no clock, no clock time
    int mins = t.tm_hour * 60 + t.tm_min + (int)(h * 60.0f + 0.5f);
    e.hh = (mins / 60) % 24;
    e.mm = mins % 60;
    e.hours = h;
    e.kind = ETA_FULL;
    return e;
  }

  if (load >= ETA_MIN_W) {
    // Down to the CEB floor, not to zero: past that the controller
    // hands the house to mains, so the pack never spends it.
    if (soc <= ETA_RESERVE_SOC) { e.hours = 0; e.kind = ETA_EMPTY; return e; }
    float left = PACK_USABLE_WH * (soc - ETA_RESERVE_SOC) / 100.0f;
    e.hours = left / load;
    if (e.hours > ETA_MAX_HOURS) e.hours = ETA_MAX_HOURS + 1;   // ">24h"
    e.kind = ETA_EMPTY;
    return e;
  }
  return e;                                       // below ETA_MIN_W: unknowable
}

// ============================================================
//  SECTION 11f - SOURCE RESTORE  (config.h 8o)
//
//  Runs from setup() within milliseconds of boot, BEFORE Serial,
//  LittleFS, WiFi, BLE and the weather fetch. It never waits for a
//  network or for the BMS: a reboot must not leave the house
//  transferring while the device waits for something.
//
//  It re-applies the relay and seeds the state machine. It does not
//  decide anything -- decideSource() still owns every rule, with
//  every threshold, hysteresis band and dwell value unchanged.
// ============================================================
static void sourceRestore() {
  relayOps = prefs.getUInt(SRC_NVS_OPS, 0);

  uint8_t st  = prefs.getUChar(SRC_NVS_STATE, (uint8_t)SRC_NONE);
  int64_t when = prefs.getLong64(SRC_NVS_EPOCH, 0);
  uint8_t why = prefs.getUChar(SRC_NVS_REASON, (uint8_t)SR_NONE);
  int8_t  soc = prefs.getChar(SRC_NVS_SOC, -1);

  // Corrupt or absent record -> ordinary startup, logged, no crash.
  if (st != (uint8_t)SRC_UTILITY && st != (uint8_t)SRC_SOLAR) {
    snprintf(srcRestoreMsg, sizeof(srcRestoreMsg),
             "SOURCE RESTORE: none - no usable record - normal startup");
    return;
  }
  if (why > (uint8_t)SR_RESTORE) why = (uint8_t)SR_NONE;

  // Pack is what setup() already leaves the changeover on, so there
  // is nothing to re-apply and nothing to hold.
  if (st == (uint8_t)SRC_SOLAR) {
    snprintf(srcRestoreMsg, sizeof(srcRestoreMsg),
             "SOURCE RESTORE: Pack - already the boot state - normal startup");
    return;
  }

  // CEB. Decide whether the age test can be believed at all.
  long ageS = -1;
  if (clockTrusted && when > 1700000000LL && timeReady())
    ageS = (long)(nowEpoch() - (time_t)when);

  if (ageS >= 0 && ageS > (long)SOURCE_RESTORE_MAX_AGE_S) {
    snprintf(srcRestoreMsg, sizeof(srcRestoreMsg),
             "SOURCE RESTORE: none - record %ldm old (>%lum) - normal startup",
             ageS / 60, SOURCE_RESTORE_MAX_AGE_S / 60);
    return;
  }

  // Re-apply. Break-before-make is unnecessary here: setup() has
  // already put both coils in the safe state and nothing is closed.
  setSourceRelays(SRC_UTILITY);
  srcActual  = SRC_UTILITY;
  srcTarget  = SRC_UTILITY;
  srcSince   = millis();          // arms SOURCE_MIN_DWELL_MS
  cebOn      = true;
  cebSince   = millis();          // arms CEB_MIN_ON_MS
  cebLowArmed = true;
  srcEverSet = true;              // so the first dwell actually applies
  srcRestored = true;
  srcLastReason = (SrcReason)why;
  srcReason  = "restored after reset";

  const char* w = why == SR_SOC_LOW  ? "SOC_LOW"  : why == SR_CRITICAL ? "CRITICAL"
                : why == SR_BMS_LOST ? "BMS_LOST" : why == SR_MANUAL   ? "MANUAL"
                : why == SR_DEADLINE ? "DEADLINE" : why == SR_RECOVERED ? "RECOVERED"
                                                                       : "UNKNOWN";
  if (ageS >= 0)
    snprintf(srcRestoreMsg, sizeof(srcRestoreMsg),
             "SOURCE RESTORE: CEB - stored %ldm ago - reason %s - soc %d%% - dwell armed %lum",
             ageS / 60, w, (int)soc, SOURCE_MIN_DWELL_MS / 60000UL);
  else
    snprintf(srcRestoreMsg, sizeof(srcRestoreMsg),
             "SOURCE RESTORE: CEB - age unknown (%s lost the clock) - reason %s - "
             "soc %d%% - provisional, dwell armed %lum",
             resetReasonName(bootReason), w, (int)soc, SOURCE_MIN_DWELL_MS / 60000UL);
}

// ============================================================
//  SECTION 12 - RELAY CONTROL  (single pole, LIVE only)
//
//  With the neutral relays removed, break-before-make on live is
//  the ONLY thing keeping utility and inverter apart. Every write
//  to the source relays goes through setSourceRelays(), which
//  cannot express "both on".
// ============================================================
static inline void relayDrive(int pin, bool on) {
  if (pin < 0) return;
#if RELAY_ACTIVE_LOW
  digitalWrite(pin, on ? LOW : HIGH);
#else
  digitalWrite(pin, on ? HIGH : LOW);
#endif
}

// The interlock. One argument, so no caller can ever energise both
// source relays; SRC_NONE opens both.
static void setSourceRelays(Source s) {
  relayDrive(RELAY_UTILITY_PIN, s == SRC_UTILITY);
  relayDrive(RELAY_SOLAR_PIN,   s == SRC_SOLAR);
}

// ------------------------------------------------------------
//  THE CEB / SOLAR SOURCE STATE MACHINE
//
//  ONE authoritative controller. decideSource() is the only code
//  that forms an opinion about the source, relayTask() is the only
//  code that acts on it, and setSourceRelays() is the only code
//  that writes a source pin. The web UI, the OLED, Firebase and
//  the weather module all READ srcActual; none of them can set it.
//  A manual request from /api/relay sets manualSrc, which is an
//  INPUT to this machine, not a second machine.
//
//  Solar is the default; CEB is a rescue that must justify both
//  its engagement AND its continued use.
//
//  ENGAGE when the pack is at or below SOC_CEB_ON (18%) for
//  SRC_ENGAGE_HOLD_MS continuously, or when the BMS link has been
//  lost for the same period (SoC unknown -> do not gamble the pack).
//
//  RELEASE needs ALL of:
//    - a validated SoC
//    - CEB has run at least CEB_MIN_ON_MS        (anti-chatter)
//    - SoC >= SOC_CEB_OFF                        (hysteresis, 25%)
//    - and either
//        * SoC >= CEB_EARLY_RELEASE_SOC and charging   (early-out)
//        * or past today's release deadline AND
//          (charging, or SoC >= SOC_CEB_SAFE)
//    - all of the above held for SRC_RELEASE_HOLD_MS continuously
//
//  The deadline is the ONLY thing the expected-solar class
//  influences:
//    CLEAR / HALF / UNKNOWN -> 08:30, hand back to solar early
//    HEAVY (LOW SOLAR)      -> 18:15, ride out a poor-solar day
//
//  v7 ANTI-OSCILLATION. Nothing here reads a raw sample:
//    * SoC is socStableVal, validated in socUpdate()
//    * charge power is pvFilt, low-passed over PV_FILTER_TC_S
//    * every condition passes a Persist gate before it counts
//    * relayTask() then enforces SOURCE_MIN_DWELL_MS on top
//  A load step changes loadFilt and nothing else -- house load is
//  not an input to any engage condition, by design, because a
//  kettle is not a reason to start the grid.
// ------------------------------------------------------------

// Release deadline in minutes-from-midnight for a weather class.
static uint16_t cebReleaseMinute(WeatherClass w) {
  if (w == WX_HEAVY) return CEB_HEAVY_RELEASE_HOUR * 60 + CEB_HEAVY_RELEASE_MINUTE;
  return CEB_CLEAR_RELEASE_HOUR * 60 + CEB_CLEAR_RELEASE_MINUTE;
}

// Rate-limited diagnostic for a transition that was WANTED but held
// back. Requirement 1.12: enough to explain future behaviour,
// nowhere near enough to flood the console.
static void srcLogReject(uint32_t now, const char* why) {
  if (srcLastRej && (uint32_t)(now - srcLastRej) < SRC_LOG_THROTTLE_MS) return;
  srcLastRej = now;
  Serial.printf("[src] holding %s: %s (soc=%d%% pv=%.0fW load=%.0fW trend=%s)\n",
                srcName(srcActual), why, socStableVal, pvFilt, loadFilt,
                socTrendName());
}

// Returns the source the controller WANTS.
//
// SRC_NONE means "no opinion" -- during startup, before the data is
// good enough to decide. relayTask() then leaves the relays exactly
// where setup() put them, which on the v5 changeover is the
// inverter. It does NOT mean "open both relays".
static Source decideSource() {
  const uint32_t now = millis();
  // No BmsData snapshot here, deliberately: nothing in this function
  // may read a raw sample. SoC comes from socStableVal and array
  // power from pvFilt, both maintained on the control tick.

  // Set the reported deadline FIRST, before any early return, so the
  // dashboard shows the real handover time even while the BMS is down.
  const WeatherClass wx = wxEffectiveClass();
  cebRelMin = cebReleaseMinute(wx);

  // 1. a choice made in the web UI wins until it expires
  if (manualSrc != SRC_NONE) {
    if (MANUAL_OVERRIDE_MS == 0 || (uint32_t)(now - manualSince) < MANUAL_OVERRIDE_MS) {
      srcReason = "manual override";
      return manualSrc;
    }
    manualSrc = SRC_NONE;
    Serial.println("[src] manual override expired, back to automatic");
  }

  const bool haveSoc = bmsFresh() && socStableVal >= 0;

  // 2. STARTUP STABILISATION (requirement 1.8).
  //    setup() has already put the relays in their safe state. Until
  //    the BMS has produced a validated SoC -- or the grace period
  //    runs out -- issue no command at all, so a boot cannot produce
  //    a burst of transitions from half-initialised data. No stale
  //    source state is restored from RAM or NVS; the first decision
  //    is taken from live data only.
  if (srcPhase == SRCP_WAIT) {
    if (haveSoc && now >= SRC_STARTUP_MIN_MS) {
      // SEED the machine from the pack's actual state before the
      // first command. Without this, boot always lands on solar and
      // a pack that is already below the floor then needs a second
      // transition to correct it -- which SOURCE_MIN_DWELL_MS would
      // hold off for three minutes. One boot, one relay operation.
      //
      // A RESTORED state is not re-seeded: section 11f already set
      // cebOn/cebSince from the record, and overwriting them here
      // from a freshly-validated SoC is exactly the second transition
      // this patch exists to prevent. The ordinary release rules --
      // unchanged -- then decide when CEB hands back.
      if (!srcRestored) {
        cebOn = socStableVal <= SOC_CEB_ON;
        cebLowArmed = cebOn;
        if (cebOn) cebSince = now;
      }
      srcPhase = SRCP_RUN;
      Serial.printf("[src] startup complete at %lu ms, soc=%d%%, ceb=%s\n",
                    (unsigned long)now, socStableVal, cebOn ? "on" : "off");
    } else if (now >= SRC_STARTUP_MAX_MS) {
      srcPhase = SRCP_RUN;
      Serial.println("[src] startup grace expired without valid BMS data");
    } else {
      srcReason = "initialising";
      return SRC_NONE;
    }
  }

  // 3. BMS telemetry gone -> SoC UNKNOWN, and UNKNOWN is never 0%.
  //    Preserved v4 failsafe: keep the house alive on mains rather
  //    than flatten a pack we cannot see. Now debounced, so a brief
  //    BLE gap cannot move a mains relay. Deliberately does NOT
  //    touch cebOn, so the normal machine resumes cleanly.
  if (pLostBms.hold(bmsLost(), SRC_ENGAGE_HOLD_MS, now)) {
    srcReason = "BMS link lost, failsafe to CEB";
    return SRC_UTILITY;
  }
  if (!haveSoc) {
    // Stale or unvalidated: hold position. Never substitute 0%.
    srcReason = "waiting for valid BMS data";
    return srcActual;
  }

  const int  soc      = socStableVal;
  const bool charging = pvFilt >= CEB_RELEASE_CHARGE_W;

  struct tm tmv;
  const bool haveTime = localNow(&tmv);
  const int  nowMin   = haveTime ? tmv.tm_hour * 60 + tmv.tm_min : -1;

  // 4. ENGAGE. The low-SoC condition must hold CONTINUOUSLY.
  //    Note what is NOT here: discharge current, load power, PV
  //    power and time of day are all absent. A full pack supplying
  //    a large load satisfies nothing below, which is requirement
  //    1.5 -- discharging from 100% is normal, not an emergency.
  //
  //    The floor test itself is a Schmitt trigger. A pack parked at
  //    the floor reads 17/18/19/18 as the BMS quantises, and a plain
  //    "soc <= 18" would reset the persistence gate on every 19 --
  //    the gate would never fill and the pack would never be
  //    rescued. Arm at SOC_CEB_ON, disarm only once the SoC has
  //    climbed clear of it.
  if (cebLowArmed) {
    if (soc >= SOC_CEB_ON + SOC_ENGAGE_HYST) cebLowArmed = false;
  } else if (soc <= SOC_CEB_ON) {
    cebLowArmed = true;
  }

  if (!cebOn) {
    bool low = cebLowArmed;
    if (pEngage.hold(low, SRC_ENGAGE_HOLD_MS, now)) {
      cebOn    = true;
      cebSince = now;
      pEngage.reset();
      Serial.printf("[ceb] ON  soc=%d%% (<=%d, held %lus) wx=%s release=%02d:%02d\n",
                    soc, SOC_CEB_ON, (unsigned long)(SRC_ENGAGE_HOLD_MS / 1000),
                    wxName(wx), cebRelMin / 60, cebRelMin % 60);
    } else if (low) {
      srcLogReject(now, "SoC at the CEB floor but not yet persistent");
    }
  } else {
    pEngage.reset();
  }

  // 5. RELEASE. Hysteresis (SOC_CEB_OFF is 7 points above
  //    SOC_CEB_ON) plus CEB_MIN_ON_MS plus persistence. charging is
  //    the FILTERED array power, so a passing cloud does not reset
  //    the gate but a real loss of generation does.
  if (cebOn) {
    bool dwellOk  = (uint32_t)(now - cebSince) >= CEB_MIN_ON_MS;
    bool socOk    = soc >= SOC_CEB_OFF;
    bool earlyOut = soc >= CEB_EARLY_RELEASE_SOC && charging;
    bool pastDue  = haveTime && nowMin >= (int)cebRelMin;
    bool deadline = pastDue && (charging || soc >= SOC_CEB_SAFE);
    bool want     = socOk && (earlyOut || deadline);

    if (dwellOk && pRelease.hold(want, SRC_RELEASE_HOLD_MS, now)) {
      cebOn = false;
      pRelease.reset();
      Serial.printf("[ceb] OFF soc=%d%% %s (wx=%s, held %lus)\n", soc,
                    earlyOut ? "pack recovered, early handover" : "release window reached",
                    wxName(wx), (unsigned long)(SRC_RELEASE_HOLD_MS / 1000));
    } else if (want && !dwellOk) {
      srcLogReject(now, "release conditions met, waiting out CEB_MIN_ON_MS");
    }
  } else {
    pRelease.reset();
  }

  // 6. Deep-discharge backstop, independent of every schedule and
  //    debounced so a single sample cannot invoke it. solarProducing()
  //    now reads the filtered array power, so an appliance starting
  //    is no longer momentarily indistinguishable from nightfall.
  if (!cebOn) {
    bool critical = soc <= SOC_CRITICAL && !solarProducing();
    if (pCritical.hold(critical, SRC_CRITICAL_HOLD_MS, now)) {
      cebOn    = true;
      cebSince = now;
      pCritical.reset();
      Serial.printf("[ceb] ON  soc=%d%% CRITICAL, no sun\n", soc);
    }
  } else {
    pCritical.reset();
  }

  if (cebOn) {
    srcReason = (soc <= SOC_CEB_ON) ? "pack at the CEB floor"
              : (wx == WX_HEAVY)    ? "low-solar day, holding CEB to the evening handover"
                                    : "waiting for the morning handover";
    return SRC_UTILITY;
  }
  srcReason = charging ? "solar charging the pack" : "running from the pack";
  return SRC_SOLAR;
}

static void relayTask() {
  uint32_t now = millis();

  if (inChangeover) {
    if ((int32_t)(now - deadUntil) >= 0) {
      setSourceRelays(srcTarget);
      srcActual = srcTarget;
      srcSince = now;
      // CEB_MIN_ON_MS means "CEB has actually been carrying the
      // house this long", so the clock starts HERE, when the
      // contacts close -- not when decideSource() asked for it,
      // which can be up to SOURCE_MIN_DWELL_MS earlier.
      if (srcActual == SRC_UTILITY) cebSince = now;
      inChangeover = false;

      // The transfer is COMMITTED here, so this is where it is
      // counted and persisted -- one NVS write per transfer, never
      // on a timer and never from the control loop.
      relayOps++;
      SrcReason why = srcLastReason;
      sourceSave(srcActual, why);

      // Transfer-rate diagnostic. Counting only: it logs and pins
      // the EXISTING dwell to its maximum, and introduces no new
      // threshold into any decision.
      xferTimes[xferHead] = now;
      xferHead = (uint8_t)((xferHead + 1) % XFER_BURST_N);
      uint32_t oldest = xferTimes[xferHead];
      xferBurst = oldest && (uint32_t)(now - oldest) < XFER_BURST_WINDOW_MS;
      if (xferBurst)
        Serial.printf("[relay] WARNING %d transfers inside %lum - holding dwell at max\n",
                      XFER_BURST_N, XFER_BURST_WINDOW_MS / 60000UL);

      Serial.printf("[relay] %s -> %s (%s) op #%lu\n",
                    srcName(srcTarget == SRC_UTILITY ? SRC_SOLAR : SRC_UTILITY),
                    srcName(srcActual), srcReason, (unsigned long)relayOps);
    }
    return;
  }

  Source want = decideSource();

  // SRC_NONE from decideSource() is "no opinion", not "isolate".
  // Leave the relays exactly where they are.
  if (want == SRC_NONE || want == srcActual) return;

  // The ONLY dwell bypass, and it is a genuine battery emergency:
  // a VALIDATED SoC at or below SOC_CRITICAL, held for
  // SRC_CRITICAL_HOLD_MS. A single bad frame can no longer claim to
  // be an emergency, and safety still outranks the dwell timer
  // (requirement 1.3).
  bool critical = pUrgent.hold(want == SRC_UTILITY && bmsFresh() &&
                               socStableVal >= 0 && socStableVal <= SOC_CRITICAL,
                               SRC_CRITICAL_HOLD_MS, now);
  // The very first command after boot is not a re-transition, so it
  // is not subject to a dwell that has never started.
  bool urgent = critical || !srcEverSet;

  if (!urgent && (uint32_t)(now - srcSince) < SOURCE_MIN_DWELL_MS) {
    srcLogReject(now, "minimum source dwell has not elapsed");
    return;
  }

  // Requirement 1.12: log every ACTUAL transition with its reason
  // and the numbers behind it. Nothing is logged when nothing moves.
  // Classify the reason once, for the NVS record and the log.
  srcLastReason = manualSrc != SRC_NONE            ? SR_MANUAL
                : critical                          ? SR_CRITICAL
                : want == SRC_UTILITY && bmsLost()  ? SR_BMS_LOST
                : want == SRC_UTILITY               ? SR_SOC_LOW
                : socStableVal >= CEB_EARLY_RELEASE_SOC ? SR_RECOVERED
                                                    : SR_DEADLINE;

  Serial.printf("[src] %s -> %s | %s | soc=%d%% trend=%s pv=%.0fW load=%.0fW%s\n",
                srcName(srcActual), srcName(want), srcReason,
                socStableVal, socTrendName(), pvFilt, loadFilt,
                critical ? " [CRITICAL, dwell bypassed]" : "");

  setSourceRelays(SRC_NONE);            // break before make
  srcActual = SRC_NONE;
  srcTarget = want;
  deadUntil = now + RELAY_DEAD_TIME_MS;
  inChangeover = true;
  srcEverSet = true;
  pUrgent.reset();
}

static void utilityAccounting() {
  static uint32_t last = 0;
  static uint32_t carryMs = 0;
  uint32_t now = millis();
  if (last && srcActual == SRC_UTILITY) {
    carryMs += now - last;
    utilitySecToday += carryMs / 1000;
    carryMs %= 1000;
  }
  last = now;
}

// ============================================================
//  SECTION 13 - BUZZER  (passive, LEDC, fully non-blocking)
//
//  A melody is a list of {frequency, duration}. buzzerTick() walks
//  it on millis() and writes one LEDC register per step, so the
//  tone costs no CPU time and cannot delay relay or BLE work.
//  Frequency 0 is a rest; duration 0 ends the melody.
//
//  Split in two on purpose:
//    protectionUpdate()  policy, 250 ms control tick
//    buzzerTick()        output, every loop pass (~20 ms)
// ============================================================
// Warning: a gentle rising three-note chime. Meant to be noticed
// without being alarming -- shutdown is still 3 SoC points away.
static const Note MELODY_WARN[] = {
  { 784, 140 }, {   0,  70 },      // G5
  { 988, 140 }, {   0,  70 },      // B5
  {1175, 220 }, {   0,   0 },      // D6
};
// Alarm: urgent triple beep, load has been disconnected.
static const Note MELODY_ALARM[] = {
  {1568, 120 }, {   0,  80 },
  {1568, 120 }, {   0,  80 },
  {1568, 120 }, {   0,   0 },
};

// Emergency: a two-tone siren, deliberately NOTHING like the
// three-beep alarm above. Different rhythm, different pitch
// contour, so "battery is being eaten alive" cannot be confused
// with "load has been shed".
static const Note MELODY_EMERG[] = {
  {2093, 170 }, {1568, 170 },
  {2093, 170 }, {1568, 170 },
  {2093, 170 }, {1568, 170 },
  {   0, 200 }, {   0,   0 },
};

static const Note* melody     = nullptr;
static uint8_t     melodyLen  = 0;
static uint8_t     melodyIdx  = 0;
static uint32_t    noteUntil  = 0;
static uint32_t    lastMelody = 0;

static void buzzerOutput(uint16_t freq) {
  if (BUZZER_PIN < 0) return;
  if (freq == 0) ledcWrite(BUZZER_LEDC_CH, 0);
  else {
    ledcWriteTone(BUZZER_LEDC_CH, freq);
    ledcWrite(BUZZER_LEDC_CH, BUZZER_DUTY);
  }
}

static void melodyStart(const Note* m, uint8_t len) {
  melody    = m;
  melodyLen = len;
  melodyIdx = 0;
  noteUntil = millis();
  lastMelody = millis();
}

// ---- policy: runs on the 250 ms control tick ----
//
// Decides the two protection states from SoC. Hysteresis of
// SOC_ALARM_HYST on both thresholds, so a pack sitting exactly on
// 35% or 38% cannot chatter the load relay or stutter the buzzer.
//
//   SoC >  38          silent
//   SoC <= 38          warning melody
//   SoC <= 35          load relay opens, alarm melody
//   recover at +2 points above each threshold
static void protectionUpdate() {
  BmsData b = bmsGet();

  if (!bmsFresh()) {
    // Never act on a stale reading. Once the link has been gone
    // long enough to be called lost, silence the alarm too --
    // the load relay stays as-is until real data returns.
    if (bmsLost() && buzzMode != BUZZ_SILENT) {
      emergAlarm = false;            // no data = no basis to alarm
      buzzMode = BUZZ_SILENT;
      melody = nullptr;
      buzzerOutput(0);
    }
    return;
  }

  // v7: the VALIDATED SoC (config.h 8j). Every threshold and every
  // hysteresis band below is unchanged; this only stops one corrupt
  // frame from shedding the load or sounding the emergency siren.
  if (socStableVal < 0) return;
  int soc = socStableVal;

  if (loadCutoff) {
    if (soc >= SOC_LOAD_CUTOFF + SOC_ALARM_HYST) {
      loadCutoff = false;
      Serial.printf("[prot] SoC %d%%, load reconnected\n", soc);
    }
  } else if (soc <= SOC_LOAD_CUTOFF) {
    loadCutoff = true;
    Serial.printf("[prot] SoC %d%%, LOAD DISCONNECTED\n", soc);
  }

  // ---- emergency: the pack is being drained while critically low ----
  //
  // The scenario: the grid fails overnight while the controller
  // believes CEB is carrying the house. Nobody notices, and the
  // inverter keeps eating an already-flat battery.
  //
  // The trigger is DISCHARGE, not relay position, and that is a
  // deliberate choice. Gating on "srcActual != SRC_UTILITY" would
  // have missed the exact failure this exists for -- in that case
  // the relay IS commanded to CEB, it is the grid behind it that
  // is gone. Actual discharge current is direct evidence that the
  // battery is being consumed, whatever the relay believes, and it
  // is equally good at NOT firing when CEB genuinely has the house
  // (then the pack simply is not discharging).
  bool packDraining = b.packI < -CURRENT_DEADBAND;
  if (emergAlarm) {
    // re-arm only once the pack is genuinely out of danger
    if (soc >= SOC_EMERGENCY + SOC_ALARM_HYST || !packDraining) {
      emergAlarm = false;
      Serial.printf("[prot] emergency cleared (soc %d%%, %s)\n",
                    soc, packDraining ? "still on pack" : "pack no longer discharging");
    }
  } else if (soc <= SOC_EMERGENCY && packDraining) {
    emergAlarm = true;
    Serial.printf("[prot] EMERGENCY: soc %d%% and the pack is supplying %.0f W\n",
                  soc, -b.packW);
  }

  BuzzTone want;
  if (emergAlarm)                    want = BUZZ_EMERG;
  else if (loadCutoff)               want = BUZZ_ALARM;
  else if (soc <= SOC_BUZZER_WARN)   want = BUZZ_WARN;
  else if (buzzMode != BUZZ_SILENT && buzzMode != BUZZ_EMERG &&
           soc < SOC_BUZZER_WARN + SOC_ALARM_HYST) want = buzzMode;
  else                               want = BUZZ_SILENT;

  if (want != buzzMode) {
    buzzMode = want;
    lastMelody = 0;                  // sound the new state at once
    if (want == BUZZ_SILENT) { melody = nullptr; buzzerOutput(0); }
    Serial.printf("[buzz] %s (soc %d%%)\n",
                  want == BUZZ_EMERG ? "EMERGENCY" :
                  want == BUZZ_ALARM ? "ALARM" :
                  want == BUZZ_WARN  ? "warn" : "silent", soc);
  }
}

// ---- output: runs on every loop pass (~20 ms) ----
//
// Only touches the melody cursor and one LEDC register. Kept off
// the 250 ms tick because the shortest note is 70 ms and coarser
// quantisation would wreck the rhythm.
static void buzzerTick() {
  uint32_t now = millis();

  // start the next repetition when the gap has elapsed
  if (buzzMode != BUZZ_SILENT && !melody) {
    uint32_t period = (buzzMode == BUZZ_EMERG) ? BUZZER_EMERG_PERIOD_MS
                    : (buzzMode == BUZZ_ALARM) ? BUZZER_ALARM_PERIOD_MS
                                              : BUZZER_WARN_PERIOD_MS;
    if (now - lastMelody >= period) {
      if (buzzMode == BUZZ_EMERG)
        melodyStart(MELODY_EMERG, sizeof(MELODY_EMERG) / sizeof(Note));
      else if (buzzMode == BUZZ_ALARM)
        melodyStart(MELODY_ALARM, sizeof(MELODY_ALARM) / sizeof(Note));
      else
        melodyStart(MELODY_WARN, sizeof(MELODY_WARN) / sizeof(Note));
    }
  }

  // advance one step at a time, never waiting
  if (melody && (int32_t)(now - noteUntil) >= 0) {
    if (melodyIdx >= melodyLen) {
      melody = nullptr;
      buzzerOutput(0);
    } else {
      const Note& n = melody[melodyIdx++];
      buzzerOutput(n.freq);
      noteUntil = now + n.ms;
      if (n.ms == 0) { melody = nullptr; buzzerOutput(0); }
    }
  }
}

// ============================================================
//  SECTION 14 - TRAVEL MODE + LOAD RELAY
//
//  The load relay serves two masters. Battery protection wins:
//  below SOC_LOAD_CUTOFF the circuit is opened no matter what the
//  schedule or the web toggle say.
// ============================================================
static bool inTravelWindow(const struct tm& tmv) {
  int nowMin = tmv.tm_hour * 60 + tmv.tm_min;
  int onMin  = TRAVEL_ON_HOUR  * 60 + TRAVEL_ON_MIN;
  int offMin = TRAVEL_OFF_HOUR * 60 + TRAVEL_OFF_MIN;
  if (onMin <= offMin) return nowMin >= onMin && nowMin < offMin;
  return nowMin >= onMin || nowMin < offMin;
}

// ---- lights policy (config.h 8n) --------------------------------
//
// These are INPUTS to travelTask(), which remains the only writer of
// RELAY_LOAD_PIN. Nothing here touches the source state machine.

// AUTO switch-on: today's cached sunset, or the fixed fallback.
int lightsOnMinute() {
  if (timeReady()) {
    const WxDay* d = wxFind(dateStamp(nowEpoch()));
    if (d && d->sunsetMin) return d->sunsetMin;
  }
  return LIGHTS_FALLBACK_ON_HOUR * 60 + LIGHTS_FALLBACK_ON_MIN;
}

int lightsOffMinute() { return LIGHTS_OFF_HOUR * 60 + LIGHTS_OFF_MIN; }

// Is the cached sunset real, or are we on the fallback? Reported so
// the dashboard can say which.
bool lightsHaveSunset() {
  if (!timeReady()) return false;
  const WxDay* d = wxFind(dateStamp(nowEpoch()));
  return d && d->sunsetMin;
}

static bool lightsAutoWindow(const struct tm& tmv) {
  int nowMin = tmv.tm_hour * 60 + tmv.tm_min;
  int on = lightsOnMinute(), off = lightsOffMinute();
  if (on <= off) return nowMin >= on && nowMin < off;
  return nowMin >= on || nowMin < off;          // window crosses midnight
}

// Latched, with hysteresis: lighting is the one load a person
// notices flicking, and an unlatched threshold at a SoC sitting on
// the boundary would do exactly that.
static void lightsLowUpdate() {
  int soc = socStableVal;
  if (soc < 0) return;                          // unknown: hold the latch
  if (lightsLow) {
    if (soc >= LIGHTS_CUTOFF_SOC + LIGHTS_CUTOFF_HYST) {
      lightsLow = false;
      Serial.printf("[lights] battery recovered to %d%%, auto control resumed\n", soc);
    }
  } else if (soc < LIGHTS_CUTOFF_SOC) {
    lightsLow = true;
    Serial.printf("[lights] OFF, pack at %d%% (below %d%%)\n", soc, LIGHTS_CUTOFF_SOC);
  }
}

const char* lightModeName() {
  return lightMode == LIGHT_ON ? "on" : lightMode == LIGHT_OFF ? "off" : "auto";
}

// Runtime accounting, mirroring utilityAccounting() exactly. Kept
// separate so that function stays byte-for-byte what it was.
static void lightsAccounting() {
  static uint32_t last = 0;
  static uint32_t carryMs = 0;
  uint32_t now = millis();
  if (last && lightOn) {
    carryMs += now - last;
    lightSecToday += carryMs / 1000;
    carryMs %= 1000;
  }
  last = now;
}

static void travelTask() {
  static int lastRaw = -1;
  static uint32_t lastEdge = 0;
  int raw = digitalRead(TRAVEL_SWITCH_PIN);
  if (raw != lastRaw) { lastRaw = raw; lastEdge = millis(); }
  else if (millis() - lastEdge > TRAVEL_DEBOUNCE_MS) {
#if TRAVEL_SWITCH_ACTIVE_LOW
    bool closed = (raw == LOW);
#else
    bool closed = (raw == HIGH);
#endif
    if (closed != travelMode) {
      travelMode = closed;
      Serial.printf("[travel] %s\n", travelMode ? "ON - schedule owns the load"
                                                : "OFF - back to normal control");
    }
  }

  lightsLowUpdate();

  struct tm tmv;
  bool haveTime = localNow(&tmv);

  // Precedence, lowest first, so the guards below can only subtract.
  bool want;
  if (travelMode && haveTime)      want = inTravelWindow(tmv);   // unchanged
  else if (lightMode == LIGHT_ON)  want = true;
  else if (lightMode == LIGHT_OFF) want = false;
  else                             want = haveTime && lightsAutoWindow(tmv);

  const char* why = "";
  if (lightsLow) { want = false; why = " (low battery)"; }
  if (loadCutoff) { want = false; why = " (battery protection)"; }

  if (want != lightOn) {
    lightOn = want;
    relayDrive(RELAY_LOAD_PIN, lightOn);
    Serial.printf("[load] %s%s\n", lightOn ? "ON" : "OFF", why);
  }
}

// ============================================================
//  SECTION 14b - OLED  (SSD1306 128x64 over I2C, U8g2, optional)
//
//  THE REGION MAP (config.h 8i) IS THE WHOLE DESIGN.
//
//    Region A   x 0..107,   y 0..45    top content, the only part
//                                      that changes between screens
//    divider    y 46, x 0..107
//    Region B   x 0..107,   y 48..63   live power, always drawn
//    lane div   x 108, full height
//    Region C   x 110..127, y 0..63    arrow lane, always drawn,
//                                      never any text
//
//  WHAT WAS WRONG BEFORE, and why this is a rewrite rather than a
//  nudge:
//
//    * The SoC number was drawn from a hand-rolled seven-segment
//      routine at a fixed y, so it sat high and could not be
//      centred against a real font metric.
//    * The percent sign was three rectangles and a diagonal. It
//      looked broken because it was.
//    * The divider sat at y=49 with the digits ending at y=45 --
//      four pixels of clearance, which on glass reads as touching.
//    * The bottom row started at y=53 in a 13 px strip, hard
//      against the bottom edge.
//    * Adafruit_GFX's 5x7 face renders 'W' as a narrow glyph that
//      genuinely does read as 'U' at size 1.
//    * The arrow shuttled two pixels. That is not an animation.
//
//  All six are properties of the drawing layer, so the drawing
//  layer is what changed. Nothing outside this section moved: the
//  values displayed still come from bmsGet(), socStableVal,
//  srcActual and the section 11b/11c weather module, and this file
//  computes no power, no SoC and no source of its own.
//
//  FAILURE POLICY, unchanged: a missing or failed panel sets
//  oledOk = false and every later call becomes a no-op. Nothing
//  here can stall the control loop, the relays, BLE or the
//  watchdog.
// ============================================================
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C
       u8g2(U8G2_R0, /* reset= */ U8X8_PIN_NONE);
static bool oledOk = false;

// ---- fonts ------------------------------------------------------
//
// Chosen for legibility at arm's length on a 0.96" panel, and
// checked against the real font metrics rather than by eye:
//
//   logisoso38_tn   38 px ascent, 24 px per digit. DIGITS ONLY,
//                   which is exactly what the SoC screen needs and
//                   is why it costs under a kilobyte instead of 5.
//   logisoso32_tn   32 px ascent. The narrower fallback, used only
//                   if a three-digit reading will not fit.
//   profont17_mr    11 px ascent. Carries a clean, unclipped '%'.
//   fub20_tf        20 px ascent. A different family from the SoC
//                   face, so the source word does not read as more
//                   digits.
//   profont12_tf    8 px ascent, 6 px advance. Its 'W' is a proper
//                   four-stroke W -- this is the font that fixes
//                   the "W looks like U" complaint.
//   profont10_tf    6 px ascent, 5 px advance. Used only where a
//                   line would otherwise overrun region A.
#define FONT_SOC     u8g2_font_logisoso38_tn
#define FONT_SOC_SM  u8g2_font_logisoso32_tn
#define FONT_PCT     u8g2_font_profont17_mr
#define FONT_SRC     u8g2_font_fub20_tf
#define FONT_TXT     u8g2_font_profont12_tf
#define FONT_SMALL   u8g2_font_profont10_tf

// ---- 28x28 weather icons, XBM ----------------------------------
//
// XBM is LSB-first per byte with rows padded to whole bytes, which
// is the opposite bit order to Adafruit_GFX's drawBitmap -- the old
// 32x32 tables could not be reused and were regenerated rather than
// byte-swapped by hand.
static const unsigned char WX28_SUN[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00,
  0x00, 0xC0, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x60, 0xC0, 0x80, 0x01,
  0xE0, 0xC0, 0xC0, 0x01, 0xC0, 0x41, 0xE0, 0x00, 0x80, 0xFB, 0x73, 0x00,
  0x00, 0xFF, 0x37, 0x00, 0x00, 0xFE, 0x0F, 0x00, 0x00, 0xFF, 0x1F, 0x00,
  0x00, 0xFF, 0x1F, 0x00, 0x00, 0xFF, 0x1F, 0x00, 0xFE, 0xFF, 0xBF, 0x0F,
  0x7E, 0xFF, 0x9F, 0x0F, 0x00, 0xFF, 0x1F, 0x00, 0x00, 0xFF, 0x1F, 0x00,
  0x00, 0xFE, 0x0F, 0x00, 0x00, 0xFC, 0x07, 0x00, 0x00, 0xFB, 0x33, 0x00,
  0x80, 0x03, 0x70, 0x00, 0xC0, 0x01, 0xE0, 0x00, 0xE0, 0xC0, 0xC0, 0x01,
  0x60, 0xC0, 0x80, 0x01, 0x00, 0xC0, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00,
  0x00, 0xC0, 0x00, 0x00
};
static const unsigned char WX28_PARTLY[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x80, 0x0F, 0x00, 0x00, 0xC0, 0x1F, 0x00, 0x00, 0xC0, 0x1F, 0x00, 0x00,
  0xE0, 0x3F, 0x00, 0x00, 0xC0, 0x1F, 0x00, 0x00, 0xC0, 0x1F, 0x00, 0x00,
  0x80, 0x8F, 0x3F, 0x00, 0x00, 0xC6, 0x7F, 0x00, 0x80, 0xFF, 0xFF, 0x00,
  0xC0, 0xFF, 0xFF, 0x00, 0xC0, 0xFF, 0xFF, 0x00, 0xC0, 0xFF, 0xFF, 0x01,
  0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00,
  0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00,
  0x00, 0xF8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};
static const unsigned char WX28_CLOUD[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0xC0, 0x7F, 0x00,
  0x80, 0xFF, 0xFF, 0x00, 0xC0, 0xFF, 0xFF, 0x00, 0xC0, 0xFF, 0xFF, 0x00,
  0xC0, 0xFF, 0xFF, 0x01, 0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00,
  0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00,
  0xE0, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};
static const unsigned char WX28_FOG[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0x03,
  0xF8, 0xFF, 0xFF, 0x03, 0xF8, 0xFF, 0xFF, 0x03, 0x00, 0x00, 0x00, 0x00,
  0xC0, 0xFF, 0x7F, 0x00, 0xC0, 0xFF, 0x7F, 0x00, 0xC0, 0xFF, 0x7F, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0xFF, 0xFF, 0x07,
  0xFC, 0xFF, 0xFF, 0x07, 0xFC, 0xFF, 0xFF, 0x07, 0x00, 0x00, 0x00, 0x00,
  0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0x01,
  0xF8, 0xFF, 0xFF, 0x01, 0xF8, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};
static const unsigned char WX28_RAIN[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0xC0, 0x7F, 0x00,
  0x80, 0xFF, 0xFF, 0x00, 0xC0, 0xFF, 0xFF, 0x00, 0xC0, 0xFF, 0xFF, 0x00,
  0xC0, 0xFF, 0xFF, 0x01, 0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00,
  0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00,
  0xE0, 0xFF, 0xFF, 0x00, 0x00, 0xF8, 0x03, 0x00, 0x00, 0xC3, 0x30, 0x00,
  0x00, 0xC3, 0x30, 0x00, 0x80, 0xE3, 0x38, 0x00, 0x80, 0xE3, 0x38, 0x00,
  0x80, 0x61, 0x18, 0x00, 0xC0, 0x71, 0x1C, 0x00, 0xC0, 0x71, 0x1C, 0x00,
  0xC0, 0x30, 0x0C, 0x00
};
static const unsigned char WX28_HEAVYRAIN[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x00, 0x80, 0xFF, 0x00,
  0xC0, 0xDF, 0xFF, 0x01, 0xE0, 0xFF, 0xFF, 0x03, 0xF0, 0xFF, 0xFF, 0x03,
  0xF0, 0xFF, 0xFF, 0x03, 0xF0, 0xFF, 0xFF, 0x07, 0xF8, 0xFF, 0xFF, 0x03,
  0xF8, 0xFF, 0xFF, 0x03, 0xF8, 0xFF, 0xFF, 0x03, 0xF8, 0xFF, 0xFF, 0x03,
  0xF8, 0xFF, 0xFF, 0x03, 0xF8, 0xFF, 0xFF, 0x03, 0xF8, 0xFF, 0xFF, 0x03,
  0x00, 0xF8, 0x03, 0x00, 0x00, 0x40, 0x00, 0x00, 0x60, 0x8C, 0x31, 0x06,
  0x70, 0xCC, 0x31, 0x07, 0x70, 0xCE, 0x39, 0x07, 0x30, 0xC6, 0x18, 0x03,
  0x38, 0xE7, 0x9C, 0x03, 0x38, 0xE7, 0x9C, 0x03, 0x9C, 0x73, 0xCE, 0x01,
  0x9C, 0x73, 0xCE, 0x01
};
static const unsigned char WX28_STORM[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x80, 0x3F, 0x00, 0x00, 0xC0, 0x7F, 0x00, 0x80, 0xFF, 0xFF, 0x00,
  0xC0, 0xFF, 0xFF, 0x00, 0xC0, 0xFF, 0xFF, 0x00, 0xC0, 0xFF, 0xFF, 0x01,
  0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00,
  0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00, 0xE0, 0xFF, 0xFF, 0x00,
  0x00, 0xF8, 0x0F, 0x00, 0x00, 0xC0, 0x03, 0x00, 0x00, 0xC0, 0x0F, 0x00,
  0x00, 0xE0, 0x0F, 0x00, 0x00, 0xF0, 0x07, 0x00, 0x00, 0xF8, 0x03, 0x00,
  0x00, 0xF8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};
static const unsigned char WX28_LOWSUN[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x03, 0x00, 0x00, 0xBC, 0x07, 0x00,
  0x00, 0x0E, 0x0E, 0x00, 0x00, 0x06, 0x0C, 0x00, 0x00, 0x06, 0x0C, 0x00,
  0x00, 0x03, 0x18, 0x00, 0x00, 0x06, 0x7F, 0x00, 0x00, 0x86, 0xFF, 0x00,
  0xC0, 0xDF, 0xFF, 0x01, 0xE0, 0xFF, 0xFF, 0x03, 0xF0, 0xFF, 0xFF, 0x03,
  0xF0, 0xFF, 0xFF, 0x03, 0xF0, 0xFF, 0xFF, 0x07, 0xF8, 0xFF, 0xFF, 0x03,
  0xF8, 0xFF, 0xFF, 0x03, 0xF8, 0xFF, 0xFF, 0x03, 0xF8, 0xFF, 0xFF, 0x03,
  0xF8, 0xFF, 0xFF, 0x03, 0xF8, 0xFF, 0xFF, 0x03, 0xF8, 0xFF, 0xFF, 0x03,
  0x00, 0xF8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// The same eight icons at 24x24, for the outlook screen, which has
// to leave room for a label above and two text lines beside it.
static const unsigned char WX24_SUN[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x30, 0x00, 0x00, 0x30, 0x00,
  0x30, 0x30, 0x30, 0x70, 0x30, 0x38, 0xE0, 0x10, 0x1C, 0xC0, 0xFE, 0x0C,
  0x00, 0xFF, 0x01, 0x80, 0xFF, 0x03, 0x80, 0xFF, 0x03, 0x80, 0xFF, 0x03,
  0xFE, 0xFF, 0xF7, 0xBE, 0xFF, 0xF3, 0x80, 0xFF, 0x03, 0x80, 0xFF, 0x03,
  0x00, 0xFF, 0x01, 0x00, 0xFE, 0x00, 0xC0, 0x00, 0x0C, 0xE0, 0x00, 0x1C,
  0x70, 0x30, 0x38, 0x30, 0x30, 0x30, 0x00, 0x30, 0x00, 0x00, 0x30, 0x00
};
static const unsigned char WX24_PARTLY[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0xE0, 0x03, 0x00, 0xE0, 0x03, 0x00, 0xF0, 0x07, 0x00,
  0xE0, 0x03, 0x00, 0xE0, 0x03, 0x00, 0x80, 0xF1, 0x07, 0xC0, 0xFF, 0x0F,
  0xE0, 0xFF, 0x0F, 0xE0, 0xFF, 0x0F, 0xF0, 0xFF, 0x1F, 0xF0, 0xFF, 0x0F,
  0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const unsigned char WX24_CLOUD[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0xF0, 0x07, 0xC0, 0xFF, 0x0F, 0xE0, 0xFF, 0x0F, 0xE0, 0xFF, 0x0F,
  0xF0, 0xFF, 0x1F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F,
  0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const unsigned char WX24_FOG[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFC, 0xFF, 0x3F, 0xFC, 0xFF, 0x3F, 0xFC, 0xFF, 0x3F, 0x00, 0x00, 0x00,
  0xE0, 0xFF, 0x07, 0xE0, 0xFF, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFE, 0xFF, 0x7F, 0xFE, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xFC, 0xFF, 0x1F, 0xFC, 0xFF, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const unsigned char WX24_RAIN[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x07,
  0xC0, 0xFF, 0x0F, 0xE0, 0xFF, 0x0F, 0xE0, 0xFF, 0x0F, 0xF0, 0xFF, 0x1F,
  0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F,
  0xF0, 0xFF, 0x0F, 0x80, 0x31, 0x06, 0xC0, 0x31, 0x06, 0xC0, 0x39, 0x07,
  0xC0, 0x38, 0x07, 0xC0, 0x18, 0x03, 0xE0, 0x1C, 0x03, 0xE0, 0x1C, 0x03
};
static const unsigned char WX24_HEAVYRAIN[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0xE0, 0x0F, 0x00, 0xF0, 0x1F, 0xF0, 0xFF, 0x3F, 0xF8, 0xFF, 0x3F,
  0xF8, 0xFF, 0x3F, 0xF8, 0xFF, 0x7F, 0xFC, 0xFF, 0x3F, 0xFC, 0xFF, 0x3F,
  0xFC, 0xFF, 0x3F, 0xFC, 0xFF, 0x3F, 0xFC, 0xFF, 0x3F, 0xFC, 0xFF, 0x3F,
  0x00, 0x10, 0x00, 0x30, 0x67, 0x66, 0x30, 0x77, 0x66, 0x38, 0x73, 0x67,
  0xB8, 0x33, 0x77, 0x98, 0xBB, 0x33, 0x9C, 0x99, 0x3B, 0x9C, 0x99, 0x39
};
static const unsigned char WX24_STORM[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x07, 0xC0, 0xFF, 0x0F,
  0xE0, 0xFF, 0x0F, 0xE0, 0xFF, 0x0F, 0xF0, 0xFF, 0x1F, 0xF0, 0xFF, 0x0F,
  0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F, 0xF0, 0xFF, 0x0F,
  0x00, 0xF0, 0x00, 0x00, 0x30, 0x00, 0x00, 0xF8, 0x00, 0x00, 0xFC, 0x00,
  0x00, 0x7E, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00
};
static const unsigned char WX24_LOWSUN[] U8X8_PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x00,
  0x00, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
  0x00, 0xE0, 0x0F, 0x80, 0xF1, 0x1F, 0xF0, 0xFF, 0x3F, 0xF8, 0xFF, 0x3F,
  0xF8, 0xFF, 0x3F, 0xF8, 0xFF, 0x7F, 0xFC, 0xFF, 0x3F, 0xFC, 0xFF, 0x3F,
  0xFC, 0xFF, 0x3F, 0xFC, 0xFF, 0x3F, 0xFC, 0xFF, 0x3F, 0xFC, 0xFF, 0x3F,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const unsigned char* wxBits24(WxCond c) {
  switch (c) {
    case WC_SUNNY:     return WX24_SUN;
    case WC_PARTLY:    return WX24_PARTLY;
    case WC_CLOUDY:    return WX24_CLOUD;
    case WC_FOG:       return WX24_FOG;
    case WC_RAIN:      return WX24_RAIN;
    case WC_HEAVYRAIN: return WX24_HEAVYRAIN;
    case WC_STORM:     return WX24_STORM;
    default:           return WX24_CLOUD;
  }
}

static const unsigned char* wxBits28(WxCond c) {
  switch (c) {
    case WC_SUNNY:     return WX28_SUN;
    case WC_PARTLY:    return WX28_PARTLY;
    case WC_CLOUDY:    return WX28_CLOUD;
    case WC_FOG:       return WX28_FOG;
    case WC_RAIN:      return WX28_RAIN;
    case WC_HEAVYRAIN: return WX28_HEAVYRAIN;
    case WC_STORM:     return WX28_STORM;
    default:           return nullptr;
  }
}

static const unsigned char* pvBits28(WxIcon i) {
  switch (i) {
    case WI_SUNNY:  return WX28_SUN;
    case WI_PARTLY: return WX28_PARTLY;
    case WI_CLOUDY: return WX28_CLOUD;
    case WI_LOWSUN: return WX28_LOWSUN;
    default:        return nullptr;
  }
}

// ---- small drawing helpers -------------------------------------

// Centre a string in region A at the given baseline.
//
// If it does not fit, step down to the small face rather than let it
// run under the lane divider. Clamping x to OLED_A_X0 (what this used
// to do) hides an overflow at the left edge while still spilling out
// of the right -- silently wrong is worse than visibly smaller.
static void aCentre(const uint8_t* font, const char* s, int16_t baseline) {
  u8g2.setFont(font);
  int16_t w = (int16_t)u8g2.getStrWidth(s);
  if (w > OLED_A_W) {
    u8g2.setFont(FONT_TXT);
    w = (int16_t)u8g2.getStrWidth(s);
  }
  int16_t x = OLED_A_X0 + (OLED_A_W - w) / 2;
  if (x < OLED_A_X0) x = OLED_A_X0;
  u8g2.drawStr(x, baseline, s);
}

// Draw a string at x, dropping to a smaller face if the preferred
// one would run past OLED_A_X1 and into the arrow lane. Returns the
// width actually used.
//
// This exists because "STRONG SOLAR" at profont12 is 72 px and
// starts at x=42, which lands at 114 -- past a lane that is
// supposed to contain nothing but arrows. Clipping would hide the
// problem; measuring and stepping down fixes it.
static int16_t aFitted(int16_t x, int16_t baseline, const char* s) {
  u8g2.setFont(FONT_TXT);
  int16_t w = (int16_t)u8g2.getStrWidth(s);
  if (x + w > OLED_A_X1 + 1) {
    u8g2.setFont(FONT_SMALL);
    w = (int16_t)u8g2.getStrWidth(s);
  }
  u8g2.drawStr(x, baseline, s);
  return w;
}

// ---- region C: the arrow lane ----------------------------------
//
// Two arrows, OLED_ARROW_GAP apart, stepping OLED_ARROW_STEP px per
// frame and wrapping modulo OLED_ARROW_WRAP. Because the gap is
// exactly half the wrap, the lane always holds one arrow fully and
// one entering or leaving, so the flow never appears to stop and
// restart.
//
//   LOAD (house drawing from the pack)  arrows point UP, travel up
//   PV   (array charging the pack)      arrows point DOWN, travel down
static uint8_t oledArrowPos = 0;      // 0..OLED_ARROW_WRAP-1
static int8_t  oledArrowDir = 0;      // -1 up, +1 down, 0 idle

// One arrow with its TIP at y. Drawn even when partly off-panel:
// U8g2 clips, and that is what makes the wrap seamless.
static void oledArrowAt(int16_t y, bool up) {
  const int16_t cx = OLED_ARROW_CX;
  const int16_t hw = OLED_ARROW_HEAD_W / 2;
  const int16_t hh = OLED_ARROW_HEAD_H;
  const int16_t th = OLED_ARROW_TAIL_H;
  if (up) {
    u8g2.drawTriangle(cx, y, cx - hw, y + hh, cx + hw, y + hh);
    u8g2.drawBox(cx - 1, y + hh, 3, th);
  } else {
    u8g2.drawBox(cx - 1, y - hh - th, 3, th);
    u8g2.drawTriangle(cx, y, cx - hw, y - hh, cx + hw, y - hh);
  }
}

static void oledDrawLane(float watts, bool fresh) {
  bool idle = !fresh || (watts > -OLED_IDLE_W && watts < OLED_IDLE_W);
  if (idle) {
    // No flow: a static bar, so the lane still reads as "alive but
    // nothing moving" rather than as a dead pixel column.
    u8g2.drawBox(OLED_ARROW_CX - 4, 30, 9, 3);
    oledArrowDir = 0;
    return;
  }

  int8_t dir = watts < 0 ? -1 : +1;             // <0 discharging = LOAD = up
  if (dir != oledArrowDir) {
    oledArrowDir = dir;
    oledArrowPos = 0;                           // reset once on reversal
  }

  bool up = dir < 0;
  for (int k = 0; k < 2; k++) {
    int16_t p = (int16_t)((oledArrowPos + k * OLED_ARROW_GAP) % OLED_ARROW_WRAP);
    // Map the phase onto a tip position that starts fully off one
    // edge and ends fully off the other, so no arrow ever pops into
    // existence mid-lane.
    int16_t tip = up ? (int16_t)(OLED_HEIGHT + OLED_ARROW_H - p * (OLED_HEIGHT + 2 * OLED_ARROW_H) / OLED_ARROW_WRAP)
                     : (int16_t)(-OLED_ARROW_H + p * (OLED_HEIGHT + 2 * OLED_ARROW_H) / OLED_ARROW_WRAP);
    oledArrowAt(tip, up);
  }
}

// ---- region B: live power --------------------------------------
//
// The value and its direction both come from b.packW -- the same
// signed pack power the controller and the dashboard use. There is
// no second power calculation here, and no threshold of this
// module's own beyond the idle deadband.
// The wattage, and nothing else. v1 printed LOAD / PV / IDLE beside
// it; the arrow lane already says which way the energy is moving, so
// the word was the same fact twice and it cost the number its room.
static void oledDrawPower(float watts, bool fresh) {
  char val[12];
  if (!fresh) {
    // A stale link must not print a plausible-looking number.
    snprintf(val, sizeof(val), "-- W");
  } else if (watts > -OLED_IDLE_W && watts < OLED_IDLE_W) {
    snprintf(val, sizeof(val), "0 W");
  } else {
    // Rounded, never truncated: 184.73 W must read as 185 W.
    snprintf(val, sizeof(val), "%d W",
             (int)((watts < 0 ? -watts : watts) + 0.5f));
  }
  u8g2.setFont(FONT_TXT);
  u8g2.drawStr(OLED_B_X, OLED_B_BASELINE, val);
}

// ---- region A: the three screens -------------------------------

// SoC. Digits plus a percent sign, nothing else. The two are
// measured at runtime and centred as ONE block, so the pair is
// optically centred instead of the digits being centred and the
// percent hanging off the end; and both sit on the same baseline.
static void oledScreenSoc() {
  int soc = socStableVal;
  if (soc > 100) soc = 100;

  if (!bmsFresh() || soc < 0) {
    aCentre(FONT_SRC, "-- %", 36);              // never a stale number
    return;
  }

  char d[6];
  snprintf(d, sizeof(d), "%d", soc);

  u8g2.setFont(FONT_PCT);
  const int16_t pw = (int16_t)u8g2.getStrWidth("%");

  // Measure at the big face first; fall back to the narrower one
  // only if this particular reading will not fit. At the current
  // geometry "100" is 72 + 3 + 9 = 84 px inside a 110 px area, so
  // the fallback is a guard rather than a routine path -- but it
  // costs 600 bytes of flash and removes a whole class of "why is
  // the 1 clipped" bug if the area is ever narrowed.
  const uint8_t* face = FONT_SOC;
  u8g2.setFont(face);
  int16_t dw = (int16_t)u8g2.getStrWidth(d);
  if (dw + OLED_SOC_GAP + pw > OLED_A_W) {
    face = FONT_SOC_SM;
    u8g2.setFont(face);
    dw = (int16_t)u8g2.getStrWidth(d);
  }

  int16_t total = dw + OLED_SOC_GAP + pw;
  int16_t x = OLED_A_X0 + (OLED_A_W - total) / 2;
  if (x < OLED_A_X0) x = OLED_A_X0;

  u8g2.setFont(face);
  u8g2.drawStr(x, OLED_SOC_BASELINE, d);
  u8g2.setFont(FONT_PCT);
  u8g2.drawStr(x + dw + OLED_SOC_GAP, OLED_SOC_BASELINE, "%");
}

// The single word the house is running on. Read straight from the
// authoritative committed relay state -- never inferred from SoC,
// PV or the time.
static void oledScreenSrc() {
  aCentre(FONT_SRC, srcActual == SRC_UTILITY ? "CEB" : "Pack", 36);
}

// Weather: 28x28 icon on the left, condition text on the right,
// split over two lines when it is two words.
static void oledScreenWx() {
  const unsigned char* bits;
  const char* l1;
  const char* l2 = nullptr;

  WxCond c = wxCondNow();
  if (c != WC_UNKNOWN) {
    bits = wxBits28(c);
    switch (c) {
      case WC_PARTLY:    l1 = "PARTLY"; l2 = "CLOUDY"; break;
      case WC_HEAVYRAIN: l1 = "HEAVY";  l2 = "RAIN";   break;
      case WC_STORM:     l1 = "STORM";                 break;
      case WC_SUNNY:     l1 = "SUNNY";                 break;
      case WC_CLOUDY:    l1 = "CLOUDY";                break;
      case WC_FOG:       l1 = "FOG";                   break;
      default:           l1 = "RAIN";                  break;
    }
  } else {
    WxIcon i = wxIconNow();
    bits = pvBits28(i);
    switch (i) {
      case WI_SUNNY:  l1 = "SUNNY";                break;
      case WI_PARTLY: l1 = "PARTLY"; l2 = "CLOUDY"; break;
      case WI_CLOUDY: l1 = "CLOUDY";               break;
      case WI_LOWSUN: l1 = "LOW";    l2 = "SOLAR";  break;
      default:        l1 = "--";                   break;
    }
  }

  // +2 px on every baseline against v1: the top area grew from 46 to
  // 50 px tall, and the content is re-centred in it rather than left
  // sitting high.
  if (bits) u8g2.drawXBMP(8, 11, 28, 28, bits);
  if (l2) {
    aFitted(44, 24, l1);
    aFitted(44, 38, l2);
  } else {
    aFitted(44, 32, l1);                         // one word: centred
  }
}

// Battery ETA. Two lines, big over small:
//
//     4h 20m          FULL
//      LEFT           15:40
//
// Every number comes from section 11e, which reads the same
// socStableVal and pvFilt/loadFilt the controller uses. Nothing is
// measured or recalculated here.
static void oledScreenEta() {
  EtaOut e = etaCompute();
  char l1[16], l2[12];

  switch (e.kind) {
    case ETA_FULL_NOW:
      snprintf(l1, sizeof(l1), "FULL");
      snprintf(l2, sizeof(l2), "NOW");
      break;
    case ETA_FULL:
      snprintf(l1, sizeof(l1), "FULL");
      snprintf(l2, sizeof(l2), "%02d:%02d", e.hh, e.mm);
      break;
    case ETA_EMPTY:
      if (e.hours > ETA_MAX_HOURS) {
        snprintf(l1, sizeof(l1), ">%dh", (int)ETA_MAX_HOURS);
      } else {
        // Clamped into a range the COMPILER can see, not just one we
        // know is true: without these bounds gcc must assume a full
        // int and warns that the field could overrun l1.
        int mins = (int)(e.hours * 60.0f + 0.5f);
        if (mins < 0) mins = 0;
        if (mins > ETA_MAX_HOURS * 60) mins = ETA_MAX_HOURS * 60;
        int hh = mins / 60, mm = mins % 60;
        if (hh > 99) hh = 99;
        if (mm > 59) mm = 59;
        // No space between the fields. "23h 59m" measures 109 px in
        // fub20 against a 108 px region A -- one pixel of overflow,
        // reachable any time the pack has more than ten hours left,
        // which overnight is most of the time. "23h59m" is 101 px and
        // keeps the large face in every case, which is worth more
        // than the space.
        if (hh > 0) snprintf(l1, sizeof(l1), "%dh%02dm", hh, mm);
        else        snprintf(l1, sizeof(l1), "%dm", mm);
      }
      snprintf(l2, sizeof(l2), "LEFT");
      break;
    default:
      // Below ETA_MIN_W, or no validated SoC. Say so rather than
      // dividing by a number that means nothing.
      snprintf(l1, sizeof(l1), "-- --");
      snprintf(l2, sizeof(l2), bmsFresh() ? "NO FLOW" : "NO DATA");
      break;
  }

  aCentre(FONT_SRC, l1, 28);
  u8g2.setFont(FONT_TXT);
  int16_t w = (int16_t)u8g2.getStrWidth(l2);
  u8g2.drawStr(OLED_A_X0 + (OLED_A_W - w) / 2, 44, l2);
}

// Tomorrow's outlook. Label, icon, condition, confidence.
//
//        TOMORROW
//   [icon]  MOSTLY SUN
//           GOOD SOLAR
//
// DISPLAY ONLY -- see the banner on section 11d. Nothing this
// screen shows has any bearing on what the relay does tonight.
static void oledScreenTom() {
  PredOut p = predTomorrow();

  u8g2.setFont(FONT_SMALL);
  int16_t w = (int16_t)u8g2.getStrWidth("TOMORROW");
  u8g2.drawStr(OLED_A_X0 + (OLED_A_W - w) / 2, 11, "TOMORROW");

  const unsigned char* bits = wxBits24((WxCond)p.cond);
  if (bits) u8g2.drawXBMP(10, 16, 24, 24, bits);

  // aFitted drops to the smaller face rather than letting a long
  // word such as "STRONG SOLAR" run into the arrow lane.
  aFitted(42, 32, predCondWord((WxCond)p.cond));
  aFitted(42, 44, predConfWord(p));
}

// ---- the rotation ----------------------------------------------
// OledScreen itself is declared in config.h; see the note there.
static OledScreen oledScreen = OS_SOC;
static uint32_t   oledUntil  = 0;
static uint32_t   oledNextFrame = 0;
static bool       oledFirst  = true;
static uint8_t    oledStep   = 0;     // index into the active cycle

// The four cycles (config.h 8i). SoC appears TWICE in every one and
// stays the dominant screen; the new screens were inserted, nothing
// existing was retimed.
static const OledScreen CYC_DAY[]   = { OS_SOC, OS_SRC, OS_WX,  OS_SOC, OS_ETA };
static const OledScreen CYC_EVE[]   = { OS_SOC, OS_SRC, OS_TOM, OS_SOC, OS_ETA };
static const OledScreen CYC_PLAIN[] = { OS_SOC, OS_SRC,         OS_SOC, OS_ETA };

// Is the weather screen allowed right now? 06:00-14:00 local, and
// only when something can actually justify a label.
static bool oledWeatherDue() {
  struct tm tmv;
  if (!localNow(&tmv)) return false;
  if (tmv.tm_hour < OLED_WX_START_HOUR || tmv.tm_hour >= OLED_WX_END_HOUR) return false;
  return wxCondNow() != WC_UNKNOWN || wxIconNow() != WI_UNKNOWN;
}

// Is the outlook screen allowed? 18:00-23:59, and only when the
// history can actually produce a baseline.
static bool oledTomorrowDue() {
  struct tm tmv;
  if (!localNow(&tmv)) return false;
  if (tmv.tm_hour < OLED_TOM_START_HOUR || tmv.tm_hour >= OLED_TOM_END_HOUR) return false;
  return predTomorrow().ok;
}

static uint32_t oledScreenMs(OledScreen s) {
  switch (s) {
    case OS_SRC: return OLED_SRC_MS;
    case OS_WX:  return OLED_WX_MS;
    case OS_ETA: return OLED_ETA_MS;
    case OS_TOM: return OLED_TOM_MS;
    default:     return OLED_SOC_MS;
  }
}

// A screen with nothing to say is skipped rather than shown empty.
static bool oledScreenReady(OledScreen s) {
  if (s == OS_WX)  return oledWeatherDue();
  if (s == OS_TOM) return oledTomorrowDue();
  return true;                          // SoC, source and ETA always have a state
}

// Which cycle applies right now. Without a clock there is no window,
// so fall back to the plain one -- it contains only screens that do
// not depend on the time of day.
static void oledCycle(const OledScreen** out, uint8_t* n) {
  struct tm t;
  if (localNow(&t)) {
    int h = t.tm_hour;
    if (h >= OLED_WX_START_HOUR && h < OLED_WX_END_HOUR) {
      *out = CYC_DAY;   *n = sizeof(CYC_DAY) / sizeof(CYC_DAY[0]);   return;
    }
    if (h >= OLED_TOM_START_HOUR && h < OLED_TOM_END_HOUR) {
      *out = CYC_EVE;   *n = sizeof(CYC_EVE) / sizeof(CYC_EVE[0]);   return;
    }
  }
  *out = CYC_PLAIN; *n = sizeof(CYC_PLAIN) / sizeof(CYC_PLAIN[0]);
}

// Step to the next screen that has something to show. The cycle is
// re-read every advance, so crossing 14:00 or 18:00 takes effect at
// the next step rather than at the end of a cycle; oledStep is
// modulo'd against the NEW length so a shorter cycle cannot leave
// the index out of range.
static void oledAdvance(uint32_t now) {
  const OledScreen* cyc;
  uint8_t n;
  oledCycle(&cyc, &n);
  for (uint8_t tries = 0; tries < n; tries++) {
    oledStep = (uint8_t)((oledStep + 1) % n);
    OledScreen s = cyc[oledStep];
    if (oledScreenReady(s)) {
      oledScreen = s;
      oledUntil  = now + oledScreenMs(s);
      return;
    }
  }
  oledScreen = OS_SOC;                  // cannot happen: SoC is always ready
  oledUntil  = now + OLED_SOC_MS;
}

static void oledInit() {
#if OLED_ENABLE
  // Probe the address with Wire first. U8g2's begin() does not
  // report a missing panel, and the display must stay strictly
  // optional, so the detection has to happen here.
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN, OLED_I2C_HZ);
  Wire.setTimeOut(50);                    // no bus-hang timeout by default
  Wire.beginTransmission(OLED_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.printf("J: OLED not found at 0x%02X (SDA %d SCL %d) - continuing without it\n",
                  OLED_ADDR, OLED_SDA_PIN, OLED_SCL_PIN);
    oledOk = false;
    return;
  }

  u8g2.setI2CAddress(OLED_ADDR << 1);     // U8g2 wants the 8-bit form
  u8g2.setBusClock(OLED_I2C_HZ);
  oledOk = u8g2.begin();
  if (!oledOk) {
    Serial.println("J: OLED begin() failed - continuing without it");
    return;
  }
  u8g2.setFontPosBaseline();
  u8g2.setDrawColor(1);
  u8g2.clearBuffer();
  u8g2.setFont(FONT_TXT);
  u8g2.drawStr(2, 30, "SolarPulse");
  u8g2.sendBuffer();
  Serial.printf("J: OLED ok at 0x%02X (SDA %d SCL %d), U8g2 full buffer\n",
                OLED_ADDR, OLED_SDA_PIN, OLED_SCL_PIN);
#endif
}

// One frame. Short, straight-line, and free of any network, BLE,
// filesystem or blocking call by construction -- everything it
// reads is a plain variable or a bmsGet() snapshot copy.
static void oledTick() {
#if OLED_ENABLE
  if (!oledOk) return;
  uint32_t now = millis();
  if (!oledFirst && (int32_t)(now - oledNextFrame) < 0) return;
  oledNextFrame = now + OLED_FRAME_MS;

  if (oledFirst) { oledUntil = now + oledScreenMs(oledScreen); oledFirst = false; }
  else if ((int32_t)(now - oledUntil) >= 0) oledAdvance(now);

  oledArrowPos = (uint8_t)((oledArrowPos + OLED_ARROW_STEP) % OLED_ARROW_WRAP);

  BmsData b = bmsGet();
  bool fresh = bmsFresh();

  u8g2.clearBuffer();

  switch (oledScreen) {                        // region A
    case OS_SRC: oledScreenSrc(); break;
    case OS_WX:  oledScreenWx();  break;
    case OS_ETA: oledScreenEta(); break;
    case OS_TOM: oledScreenTom(); break;
    default:     oledScreenSoc(); break;
  }

  // The only chrome left: a short rule under the top area. No outer
  // border, no box around the power row, no divider beside the lane.
  u8g2.drawHLine(OLED_A_X0, OLED_RULE_Y, OLED_RULE_W);

  oledDrawPower(b.packW, fresh);                        // power row
  oledDrawLane(b.packW, fresh);                         // arrow lane

  u8g2.sendBuffer();
#endif
}

// ============================================================
//  SECTION 15 - LOCAL WEB API
// ============================================================
static bool authOk(AsyncWebServerRequest* req) {
  if (strlen(WEB_USER) == 0) return true;
  if (req->authenticate(WEB_USER, WEB_PASS)) return true;
  req->requestAuthentication();
  return false;
}

static String uploadBuf;

static void ingestLine(const String& line) {
  if (line.length() < 10) return;
  long ts = jsonLong(line, "t");
  if (ts < 1700000000L) return;
  struct tm tmv; time_t t = (time_t)ts; localtime_r(&t, &tmv);
  char path[32];
  snprintf(path, sizeof(path), HISTORY_DIR "/%04d%02d%02d.csv",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  FS_LOCK();
  bool isNew = !LittleFS.exists(path);
  File f = LittleFS.open(path, "a");
  if (f) {
    if (isNew) f.println("t,v,i,p,soc,pv,src");
    f.printf("%ld,%.2f,%.2f,%.1f,%ld,%.1f,%ld\n", ts,
             jsonFloat(line, "v"), jsonFloat(line, "i"), jsonFloat(line, "p"),
             jsonLong(line, "soc"), jsonFloat(line, "pv"), jsonLong(line, "src"));
    f.close();
  }
  FS_UNLOCK();
}

static String monthlyJson(int year) {
  float kwh[12] = {0}, used[12] = {0};
  int   days[12] = {0};

  FS_LOCK();
  File f = LittleFS.open(DAILY_FILE, "r");
  if (f) {
    f.readStringUntil('\n');
    while (f.available()) {
      String row = f.readStringUntil('\n');
      row.trim();
      if (row.length() < 12) continue;
      int y = row.substring(0, 4).toInt();
      int m = row.substring(5, 7).toInt();
      if (y != year || m < 1 || m > 12) continue;
      int c1 = row.indexOf(',');
      int c2 = row.indexOf(',', c1 + 1);
      int c3 = row.indexOf(',', c2 + 1);
      int c4 = row.indexOf(',', c3 + 1);
      if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0) continue;
      float dis = row.substring(c2 + 1, c3).toFloat();
      float har = row.substring(c3 + 1, c4).toFloat();
      kwh[m - 1]  += har / 1000.0f;
      used[m - 1] += dis / 1000.0f;
      days[m - 1] += 1;
    }
    f.close();
  }
  FS_UNLOCK();

  // fold today's running total in so the current month is not short
  struct tm tmv;
  if (localNow(&tmv) && tmv.tm_year + 1900 == year) {
    kwh[tmv.tm_mon]  += harvestWhToday() / 1000.0f;
    used[tmv.tm_mon] += todayDisWh / 1000.0f;
    days[tmv.tm_mon] += 1;
  }

  String out = "{\"year\":" + String(year) + ",\"months\":[";
  float total = 0;
  for (int i = 0; i < 12; i++) {
    if (i) out += ",";
    out += "{\"m\":" + String(i + 1) +
           ",\"kwh\":" + String(kwh[i], 3) +
           ",\"used\":" + String(used[i], 3) +
           ",\"days\":" + String(days[i]) + "}";
    total += kwh[i];
  }
  out += "],\"totalKwh\":" + String(total, 3) + "}";
  return out;
}

static void addCors(AsyncWebServerResponse* r) {
  r->addHeader("Access-Control-Allow-Origin", "*");
}

static void setupWebServer() {
  server.on("/api/live", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    char buf[3200];
    buildLiveJson(buf, sizeof(buf));
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", buf);
    addCors(r);
    req->send(r);
  });

  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    String date = req->hasParam("date") ? req->getParam("date")->value() : "";
    if (date.length() != 10) { char d[12]; dateString(nowEpoch(), d); date = d; }
    String path = String(HISTORY_DIR "/") + date.substring(0, 4) +
                  date.substring(5, 7) + date.substring(8, 10) + ".csv";
    if (!LittleFS.exists(path)) { req->send(404, "text/plain", "no data for " + date); return; }
    AsyncWebServerResponse* r = req->beginResponse(LittleFS, path, "text/csv");
    addCors(r);
    req->send(r);
  });

  server.on("/api/daily", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    if (!LittleFS.exists(DAILY_FILE)) {
      req->send(200, "text/csv", "date,chgWh,disWh,harvestWh,peakW,minSoc,utilMin\n");
      return;
    }
    AsyncWebServerResponse* r = req->beginResponse(LittleFS, DAILY_FILE, "text/csv");
    addCors(r);
    req->send(r);
  });

  server.on("/api/monthly", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    struct tm tmv;
    int year = localNow(&tmv) ? tmv.tm_year + 1900 : 2026;
    if (req->hasParam("year")) year = req->getParam("year")->value().toInt();
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", monthlyJson(year));
    addCors(r);
    req->send(r);
  });

  server.on("/api/upload", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      if (!authOk(req)) return;
      int n = 0;
      while (uploadBuf.length()) {
        int nl = uploadBuf.indexOf('\n');
        String line = nl < 0 ? uploadBuf : uploadBuf.substring(0, nl);
        uploadBuf = nl < 0 ? String() : uploadBuf.substring(nl + 1);
        line.trim();
        if (line.length()) { ingestLine(line); n++; }
      }
      req->send(200, "application/json", "{\"ok\":true,\"accepted\":" + String(n) + "}");
    },
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) { uploadBuf = ""; uploadBuf.reserve(total < 16384 ? total + 8 : 16384); }
      if (uploadBuf.length() > 16384) return;
      for (size_t i = 0; i < len; i++) uploadBuf += (char)data[i];
    });

  server.on("/api/relay", HTTP_ANY, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    if (req->hasParam("src")) {
      String s = req->getParam("src")->value();
      if      (s == "auto")    manualSrc = SRC_NONE;
      else if (s == "solar")   { manualSrc = SRC_SOLAR;   manualSince = millis(); }
      else if (s == "utility") { manualSrc = SRC_UTILITY; manualSince = millis(); }
      Serial.printf("[web] source request: %s\n", s.c_str());
    }
    if (req->hasParam("light")) {
      // Accepts the words and the legacy 0/1 the older on-device
      // page sent, so an un-updated dashboard keeps working.
      String v = req->getParam("light")->value();
      if      (v == "auto") lightMode = LIGHT_AUTO;
      else if (v == "on")   lightMode = LIGHT_ON;
      else if (v == "off")  lightMode = LIGHT_OFF;
      else                  lightMode = v.toInt() != 0 ? LIGHT_ON : LIGHT_OFF;
      Serial.printf("[web] lights request: %s\n", lightModeName());
    }
    req->send(200, "application/json",
      String("{\"src\":\"") + srcName(srcActual) + "\",\"manual\":" +
      (manualSrc != SRC_NONE ? "true" : "false") +
      ",\"light\":" + (lightOn ? "true" : "false") +
      ",\"lightMode\":\"" + lightModeName() + "\"" +
      ",\"cutoff\":" + (loadCutoff ? "true" : "false") + "}");
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    char buf[620];
    snprintf(buf, sizeof(buf),
      "{\"socCebOn\":%d,\"socCebOff\":%d,\"socCebSafe\":%d,\"socCebEarly\":%d,"
      "\"cebClearRelease\":\"%02d:%02d\",\"cebHeavyRelease\":\"%02d:%02d\","
      "\"cebActive\":%s,\"cebReleaseToday\":\"%02d:%02d\","
      "\"weather\":\"%s\",\"weatherFresh\":%s,\"weatherSource\":\"%s\","
      "\"socCritical\":%d,\"socBuzzerWarn\":%d,\"socLoadCutoff\":%d,"
      "\"capacityAh\":%.0f,\"oled\":%s,"
      "\"travelOn\":\"%02d:%02d\",\"travelOff\":\"%02d:%02d\","
      "\"wifiRetrySec\":%lu,\"logSec\":%lu,\"liveSec\":%lu}",
      SOC_CEB_ON, SOC_CEB_OFF, SOC_CEB_SAFE, CEB_EARLY_RELEASE_SOC,
      CEB_CLEAR_RELEASE_HOUR, CEB_CLEAR_RELEASE_MINUTE,
      CEB_HEAVY_RELEASE_HOUR, CEB_HEAVY_RELEASE_MINUTE,
      cebOn ? "true" : "false", cebRelMin / 60, cebRelMin % 60,
      wxName(wxEffectiveClass()), wxTrusted() ? "true" : "false",
      wxDecisionSrc(),        /* which evidence is in force right now */
      SOC_CRITICAL, SOC_BUZZER_WARN, SOC_LOAD_CUTOFF,
      PACK_CAPACITY_AH, OLED_ENABLE ? "true" : "false",
      TRAVEL_ON_HOUR, TRAVEL_ON_MIN, TRAVEL_OFF_HOUR, TRAVEL_OFF_MIN,
      (unsigned long)(WIFI_RETRY_MAX_MS / 1000),
      (unsigned long)(OFFLINE_LOG_MS / 1000), (unsigned long)(LIVE_PUSH_MS / 1000));
    req->send(200, "application/json", buf);
  });

  // health endpoint for external monitoring
  server.on("/api/health", HTTP_GET, [](AsyncWebServerRequest* req) {
    char buf[256];
    snprintf(buf, sizeof(buf),
      "{\"ok\":%s,\"ble\":\"%s\",\"reconn\":%lu,\"wifi\":%s,"
      "\"heap\":%lu,\"minHeap\":%lu,\"up\":%lu,\"queued\":%u}",
      bmsFresh() ? "true" : "false", bleStateName(bleState),
      (unsigned long)bleReconnects,
      WiFi.status() == WL_CONNECTED ? "true" : "false",
      (unsigned long)ESP.getFreeHeap(),
      (unsigned long)ESP.getMinFreeHeap(),
      (unsigned long)(millis() / 1000), bufferedLines);
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", buf);
    addCors(r);
    req->send(r);
  });

  server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "not found");
  });

  server.begin();
  Serial.println("[web] server on port 80");
}

// Bring the web server up only after the BMS link is established,
// or after the grace period if the BMS never shows up. AsyncTCP's
// task and buffers take a sizeable bite out of the heap and NimBLE
// needs contiguous heap plus radio time to walk the GATT table.
#ifndef WEB_START_GRACE_MS
#define WEB_START_GRACE_MS 90000UL
#endif

static void webServerTask() {
  static bool started = false;
  if (started) return;
  if (!bmsShared.lastFrameMs && millis() < WEB_START_GRACE_MS) return;
  started = true;
  Serial.printf("[web] starting (%s), heap %lu\n",
                bmsShared.lastFrameMs ? "BMS link up" : "grace period expired",
                (unsigned long)ESP.getFreeHeap());
  setupWebServer();
  if (WiFi.status() == WL_CONNECTED && MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[web] http://%s.local/\n", MDNS_NAME);
  }
}

// ============================================================
//  SECTION 16 - NETWORK TASK
//
//  Everything that can block on the network lives here, off the
//  control path and far away from any BLE callback.
// ============================================================
static void onWifiUp() {
  Serial.printf("[WiFi] up, IP %s\n", WiFi.localIP().toString().c_str());

  time_t before = nowEpoch();
  configTime(TZ_OFFSET_SEC, 0, NTP_1, NTP_2);
  for (int i = 0; i < 40 && nowEpoch() < 1700000000L; i++) vTaskDelay(pdMS_TO_TICKS(250));

  if (nowEpoch() > 1700000000L) {
    if (!ntpSynced && before > 1000000000L) {
      clockAdj = (int32_t)(nowEpoch() - before);
      if (clockAdj > 86400 || clockAdj < -86400) clockAdj = 0;
      Serial.printf("[time] clock corrected by %ld s\n", (long)clockAdj);
    }
    ntpSynced = true;
  }
}

static void netTask(void*) {
  esp_task_wdt_add(NULL);

  bool wifiWasUp = false;
  uint32_t tWifi = 0, tLive = 0, tDaily = 0, tUp = 0, tDaySync = 0;
  uint32_t wifiBackoff = WIFI_RETRY_MS;   // grows to WIFI_RETRY_MAX_MS

  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();
    bool wifiUp = WiFi.status() == WL_CONNECTED;

    if (wifiUp && !wifiWasUp) { onWifiUp(); wifiBackoff = WIFI_RETRY_MS; }
    if (!wifiUp && wifiWasUp) tlsDrop("wifi lost");
    wifiWasUp = wifiUp;

    if (!wifiUp) {
      // Escalating retry: seconds for a blip, then settling at
      // WIFI_RETRY_MAX_MS (15 min) for a real outage. Nothing here
      // blocks -- the control loop has never waited on WiFi.
      if (now - tWifi >= wifiBackoff) {
        tWifi = now;
        Serial.printf("[WiFi] retrying (next in %lus)...\n", (unsigned long)(wifiBackoff / 1000));
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        wifiBackoff = wifiBackoff * 2;
        if (wifiBackoff > WIFI_RETRY_MAX_MS) wifiBackoff = WIFI_RETRY_MAX_MS;
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // Hold every TLS handshake until the BMS link is up (or the
    // grace period expires): a handshake takes the radio for a
    // moment, and doing that while NimBLE is still walking the
    // GATT table is what made the BMS connect intermittently.
    //
    // bleBusy extends that to RECONNECTS, which the original test
    // missed: once lastFrameMs was ever set, a later BLE scan could
    // still collide with a handshake. A flag, not a delay -- this
    // task simply does no cloud work on this pass and comes back in
    // 100 ms, and neither task ever blocks on the other.
    bool cloudOk = (bmsShared.lastFrameMs || now >= WEB_START_GRACE_MS) && !bleBusy;

    // Each of these can block for up to the 8 s HTTP timeout, so the
    // watchdog is fed between them rather than once per iteration.
    if (cloudOk && now - tLive >= LIVE_PUSH_MS) {
      tLive = now; pushLive(); esp_task_wdt_reset();
    }
    if (cloudOk && now - tDaily >= DAILY_PUSH_MS) {
      tDaily = now; pushDailyToday(); esp_task_wdt_reset();
    }
    if (cloudOk && now - tDaySync >= 30000UL) {
      tDaySync = now; syncPendingDaily(); esp_task_wdt_reset();
    }
    if (cloudOk && now - tUp >= UPLOAD_TRY_MS) {
      tUp = now; uploadTask(); esp_task_wdt_reset();
    }

    // The subordinate forecast (section 11c). Deliberately LAST, on
    // its own short-lived client, so it can never delay or displace
    // anything above it: the Firebase session object is not reused
    // and not dropped, and a weather outage costs a log line.
    if (cloudOk) { weatherTask(); esp_task_wdt_reset(); }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================================================
//  SECTION 16b - RUNTIME DIAGNOSTICS  (reporting only)
//
//  None of this can move a relay, change a decision or touch a
//  counter. It exists so the next unexplained reset comes with
//  evidence instead of a guess.
// ============================================================
static TaskHandle_t hBle = nullptr, hNet = nullptr;

static void diagTick(uint32_t now) {
  // Soft watchdog: notice a stalled loop and SAY so, before the
  // hardware watchdog resets the board and takes the reason with it.
  if (loopLastMs && (uint32_t)(now - loopLastMs) > LOOP_STALL_MS)
    Serial.printf("[diag] MAIN LOOP STALLED %lu ms\n",
                  (unsigned long)(now - loopLastMs));
  loopLastMs = now;

  if ((uint32_t)(now - heapLastLog) < HEAP_LOG_MS) return;
  heapLastLog = now;

  uint32_t freeNow = ESP.getFreeHeap();
  uint32_t minEver = ESP.getMinFreeHeap();
  uint32_t biggest = ESP.getMaxAllocHeap();
  if (minEver < heapMinEver) heapMinEver = minEver;

  Serial.printf("[diag] heap %lu free, %lu min-ever, %lu largest block, frag %lu%% | "
                "stack ble %u net %u loop %u | up %lus\n",
                (unsigned long)freeNow, (unsigned long)minEver,
                (unsigned long)biggest,
                freeNow ? (unsigned long)(100 - (biggest * 100 / freeNow)) : 0,
                hBle ? uxTaskGetStackHighWaterMark(hBle) : 0,
                hNet ? uxTaskGetStackHighWaterMark(hNet) : 0,
                uxTaskGetStackHighWaterMark(nullptr),
                (unsigned long)(millis() / 1000));

  // Remember this run's uptime so the next boot can report how long
  // the previous one lasted. One small write a minute is well within
  // the wear budget and it is the only way a watchdog reset can
  // report how long the board had been up.
  prefs.putUInt("upm", (unsigned)(millis() / 60000UL));
}

// ============================================================
//  SECTION 17 - SETUP / CONTROL LOOP
// ============================================================
void setup() {
  // Safe electrical state before anything else can take time:
  // both source relays open, load open, buzzer silent.
  //
  // LEVEL BEFORE DIRECTION, and it matters. The ESP32 output latch
  // reads 0 after a reset, so pinMode(pin, OUTPUT) alone drives the
  // pin LOW the instant the driver is enabled -- which on this
  // RELAY_ACTIVE_LOW board is the ENERGISED level. The pulse is
  // about a microsecond and no mechanical relay can pull in inside
  // 5 ms, so it has never been observable; but writing the safe
  // level into the latch first means the coil is never commanded on
  // at all, which is what the requirement actually asks for.
  relayDrive(RELAY_UTILITY_PIN, false);
  pinMode(RELAY_UTILITY_PIN, OUTPUT);
  // RELAY_SOLAR_PIN is -1 on the v5 changeover wiring; only configure
  // it if a second physical relay actually exists.
  if (RELAY_SOLAR_PIN >= 0) {
    relayDrive(RELAY_SOLAR_PIN, false);
    pinMode(RELAY_SOLAR_PIN, OUTPUT);
  }
  setSourceRelays(SRC_NONE);
  if (RELAY_LOAD_PIN >= 0) {
    relayDrive(RELAY_LOAD_PIN, false);
    pinMode(RELAY_LOAD_PIN, OUTPUT);
    relayDrive(RELAY_LOAD_PIN, false);
  }
  if (BUZZER_PIN >= 0) {
    ledcSetup(BUZZER_LEDC_CH, 2000, BUZZER_LEDC_RES);
    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CH);
    ledcWrite(BUZZER_LEDC_CH, 0);
  }
#if TRAVEL_SWITCH_ACTIVE_LOW
  pinMode(TRAVEL_SWITCH_PIN, INPUT_PULLUP);
#else
  pinMode(TRAVEL_SWITCH_PIN, INPUT_PULLDOWN);
#endif

  // ---- BOOT ORDER MATTERS HERE (config.h 8o) --------------------
  //
  // The source is re-applied BEFORE Serial waits for USB, before
  // LittleFS, before WiFi, before BLE and before any weather fetch.
  // Nothing below this block can be waited on: a reboot must not
  // leave the house transferring while the device waits for a
  // network or for the BMS to produce its first frame.
  bootReason   = esp_reset_reason();
  sysFault     = resetIsFault((uint8_t)bootReason);
  clockTrusted = resetKeepsClock((uint8_t)bootReason) && timeReady();

  bool nvsOk = prefs.begin("solar", false);
  if (nvsOk) {
    loadCounters();        // may restore an approximate clock
    resetLogLoad();
    sourceRestore();       // <-- relays re-applied within ms of boot
    resetLogPush((uint8_t)bootReason,
                 timeReady() ? dateStamp(nowEpoch()) : 0,
                 (uint16_t)(prefs.getUInt("upm", 0)));
    prefs.putUInt("upm", 0);
  } else {
    snprintf(srcRestoreMsg, sizeof(srcRestoreMsg),
             "SOURCE RESTORE: none - NVS unavailable - normal startup");
  }

  Serial.begin(115200);
  delay(2000);                       // one-off, lets USB serial enumerate
  Serial.println();
  Serial.println("=== SolarPulse v4 ===");
  Serial.printf("A: boot reason %s%s\n", resetReasonName((uint8_t)bootReason),
                sysFault ? "  <-- FAULT RESET" : "");
  if (bootReason == ESP_RST_BROWNOUT)
    Serial.println("A: *** BROWNOUT *** the 3.3 V rail collapsed. This is a HARDWARE "
                   "fault - relay coil or buzzer inrush, or an undersized supply. "
                   "Software cannot fix it; see WIRING.md.");
  Serial.println(srcRestoreMsg);
  Serial.println(nvsOk ? "B: NVS open" : "B: NVS failed, counters not saved");

  dataMux = xSemaphoreCreateMutex();
  fsLock  = xSemaphoreCreateMutex();

  pvHistLoad();                        // measured PV history from NVS
  wxLoad();                            // cached forecast, if any
  predBiasLoad();                      // learned forecast bias, if any
  Serial.printf("C: counters loaded, today %.1f Wh in / %.1f Wh out, daySynced %ld\n",
                todayChgWh, todayDisWh, (long)daySynced);

  if (!LittleFS.begin(true)) {
    Serial.println("D: LittleFS failed, offline queue disabled");
  } else {
    fsOk = true;
    if (!LittleFS.exists(HISTORY_DIR)) LittleFS.mkdir(HISTORY_DIR);
    loadPos();
    countQueue();
    Serial.printf("D: LittleFS ok, %u samples queued from offset %lu\n",
                  bufferedLines, (unsigned long)queuePos);
  }

  {
    int raw = digitalRead(TRAVEL_SWITCH_PIN);
#if TRAVEL_SWITCH_ACTIVE_LOW
    travelMode = (raw == LOW);
#else
    travelMode = (raw == HIGH);
#endif
    Serial.printf("E: travel switch GPIO %d reads %s\n",
                  TRAVEL_SWITCH_PIN, travelMode ? "CLOSED (travel mode)" : "open (normal)");
  }

  NimBLEDevice::init("");                       // BLE before WiFi
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  Serial.println("F: NimBLE init ok");

  // WiFi and BLE share one 2.4 GHz radio, time-sliced by the
  // coexistence arbiter. Bluetooth wins: the BMS link drops if it
  // misses its slots, a Firebase push can simply be retried.
#if __has_include("esp_coexist.h")
  esp_coex_preference_set(ESP_COEX_PREFER_BT);
  Serial.println("G: coexistence set to prefer BT");
#endif

  WiFi.mode(WIFI_STA);
  // Do NOT disable modem sleep. Holding the radio continuously
  // starves BLE and the BMS stops delivering frames.
  WiFi.setSleep(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("H: WiFi started");

  // Watchdog last, so a slow boot cannot trip it.
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);                       // this is loopTask

  xTaskCreatePinnedToCore(bleTask, "ble", 4096, nullptr, 3, &hBle, 1);
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, &hNet, 1);

  // Display last: it is optional and must never delay the safety
  // hardware above. A missing panel just leaves oledOk false.
  oledInit();

  Serial.printf("I: setup complete, heap %lu\n", (unsigned long)ESP.getFreeHeap());
}

// The control loop. Deterministic, non-blocking, and independent
// of both the network and the BMS link: relays and the buzzer keep
// doing the right thing even with everything else down.
void loop() {
  static uint32_t tCtl = 0, tLog = 0, tRoll = 0, tNvs = 0, tStat = 0;
  uint32_t now = millis();

  esp_task_wdt_reset();
  diagTick(now);
  webServerTask();

  // Melody stepping runs on EVERY pass: its shortest note is 70 ms,
  // and quantising to the 250 ms control tick would wreck the rhythm.
  buzzerTick();
  oledTick();               // rate-limited inside; no-op if absent

  if (now - tCtl >= CONTROL_TICK_MS) {
    tCtl = now;
    BmsData b = bmsGet();
    integrateEnergy(b);     // also updates the PV/load low-pass filters
    socUpdate(b, now);      // validates SoC BEFORE anything acts on it
    etaUpdate(now);         // display-only rolling mean, no vote in anything
    protectionUpdate();     // sets loadCutoff / buzzMode from SoC
    travelTask();           // applies loadCutoff to the load relay
    relayTask();
    utilityAccounting();
    lightsAccounting();     // lights runtime today, display only
  }

  if (now - tLog  >= OFFLINE_LOG_MS) { tLog  = now; logSample(); }
  if (now - tRoll >= 10000)          { tRoll = now; rolloverCheck(); }
  if (now - tNvs  >= NVS_SAVE_MS)    { tNvs  = now; saveCounters(); }

  if (now - tStat >= 10000) {
    tStat = now;
    BmsData b = bmsGet();
    Serial.printf("[stat] ble=%s(%lu) wifi=%s soc=%d%%(raw %u) %.2fV %.2fA pv=%.0fW src=%s%s%s q=%u heap=%lu\n",
                  bleStateName(bleState), (unsigned long)bleReconnects,
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  socStableVal, b.soc, b.packV, b.packI, pvFilt, srcName(srcActual),
                  travelMode ? " travel" : "", loadCutoff ? " CUTOFF" : "",
                  bufferedLines, (unsigned long)ESP.getFreeHeap());
  }

  vTaskDelay(pdMS_TO_TICKS(20));
}
