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
#if __has_include("esp_coexist.h")
  #include "esp_coexist.h"      // WiFi/BLE radio arbitration, see setup()
#endif
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
static bool     topUpEngaged = false;
static const char* srcReason = "boot";

static bool     rainyCheckDone   = false;
static bool     rainyFloorActive = false;

static bool     travelMode  = false;
static bool     lightOn     = false;      // switched load circuit state
static bool     manualLight = false;
static bool     loadCutoff  = false;      // true = protection has opened the load
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
  snprintf(out, 12, "%04d-%02d-%02d", (int)(s / 10000), (int)((s / 100) % 100), (int)(s % 100));
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

static bool solarProducing() {
  struct tm tmv;
  bool haveTime = localNow(&tmv);
  bool daylight = !haveTime || (tmv.tm_hour >= PV_HOUR_START && tmv.tm_hour < PV_HOUR_END);
  return bmsFresh() && daylight && pvW > SOLAR_OK_W;
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
    return;
  }

  pvW   = readPvWatts(b);
  loadW = b.packW < 0 ? -b.packW : 0;

  if (lastMs == 0) { lastMs = nowMs; return; }
  float dt = (nowMs - lastMs) / 1000.0f;
  lastMs = nowMs;
  if (dt <= 0 || dt > 5.0f) return;  // clamp: a gap is not energy

  float wh = b.packW * dt / 3600.0f;
  if (b.packI >  CURRENT_DEADBAND) { todayChgWh += wh;  lifeChgWh += wh; }
  if (b.packI < -CURRENT_DEADBAND) { todayDisWh += -wh; lifeDisWh += -wh; }
  if (b.packW > todayPeakW) todayPeakW = b.packW;
  if (b.soc && b.soc < todayMinSoc) todayMinSoc = b.soc;
  todayPvWh += pvW * dt / 3600.0f;
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
  prefs.putLong64("ep", (int64_t)nowEpoch());
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
  int64_t ep = prefs.getLong64("ep", 0);
  if (ep > 1700000000LL) {
    struct timeval tv = { .tv_sec = (time_t)ep, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
  }
}

// ============================================================
//  SECTION 7 - JSON BUILDERS
// ============================================================
static int buildLiveJson(char* out, size_t cap) {
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
    "\"cutoff\":%s,\"buzz\":%u,"
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
    rainyFloorActive ? "true" : "false",
    rainyFloorActive ? SOC_RAINY_FLOOR : SOC_EVENING_FLOOR,
    loadCutoff ? "true" : "false", (unsigned)buzzMode,
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
  static char buf[1500];
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

  // 2. reset for the new day
  todayChgWh = todayDisWh = todayPeakW = todayPvWh = 0;
  todayMinSoc = 100;
  utilitySecToday = 0;
  topUpEngaged = false;
  rainyCheckDone = false;
  rainyFloorActive = false;
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

static Source decideSource() {
  BmsData b = bmsGet();

  // 1. a choice made in the web UI wins until it expires
  if (manualSrc != SRC_NONE) {
    if (MANUAL_OVERRIDE_MS == 0 || millis() - manualSince < MANUAL_OVERRIDE_MS) {
      srcReason = "manual override";
      return manualSrc;
    }
    manualSrc = SRC_NONE;
  }

  // 2. once-daily rainy-day check at 18:15 (see config.h section 8)
  if (!rainyCheckDone) {
    struct tm tmvChk;
    if (localNow(&tmvChk)) {
      bool pastCheckpoint = tmvChk.tm_hour > RAINY_CHECK_HOUR ||
                            (tmvChk.tm_hour == RAINY_CHECK_HOUR && tmvChk.tm_min >= RAINY_CHECK_MIN);
      if (pastCheckpoint && bmsFresh()) {
        rainyCheckDone = true;
        rainyFloorActive = b.soc < SOC_TARGET_1600;
        Serial.printf("[relay] %02d:%02d check: soc=%u%% -> %s\n",
                      RAINY_CHECK_HOUR, RAINY_CHECK_MIN, b.soc,
                      rainyFloorActive ? "target missed, floor relaxed for tonight"
                                       : "target reached, normal floor holds");
        if (rainyFloorActive && b.soc > SOC_RAINY_FLOOR) {
          topUpEngaged = false;
          srcReason = "18:15, target missed: off CEB, running the pack down";
          return SRC_SOLAR;
        }
      }
    }
  }

  // 3. no BMS telemetry -> SoC unknown, keep the house alive on
  //    utility rather than flatten a pack we cannot see
  if (bmsLost()) { srcReason = "BMS link lost, failsafe"; return SRC_UTILITY; }
  if (!bmsFresh()) {
    srcReason = "waiting for BMS";
    return srcActual == SRC_NONE ? SRC_UTILITY : srcActual;
  }

  int soc = b.soc;
  bool solar = solarProducing();

  struct tm tmv;
  bool haveTime = localNow(&tmv);
  int hour = haveTime ? tmv.tm_hour : 12;
  bool evening = haveTime && (hour >= EVENING_HOUR || hour < PV_HOUR_START);

  if (!evening) {
    // ---------------- before 16:00 ----------------
    if (soc <= SOC_CRITICAL && !solar) { srcReason = "SoC critical, no sun"; return SRC_UTILITY; }

    if (haveTime && hour >= TOPUP_START_HOUR && soc < SOC_TARGET_1600) {
      if (solar || topUpEngaged) {
        topUpEngaged = true;
        srcReason = "topping the pack up before 16:00";
        return SRC_UTILITY;
      }
    }
    if (soc >= SOC_TARGET_1600 || hour < TOPUP_START_HOUR) topUpEngaged = false;

    srcReason = solar ? "solar carrying the house" : "running from the pack";
    return SRC_SOLAR;
  }

  // ---------------- 16:00 to sunrise ----------------
  topUpEngaged = false;
  if (solar) { srcReason = "late sun still producing"; return SRC_SOLAR; }

  int floorSoc = rainyFloorActive ? SOC_RAINY_FLOOR : SOC_EVENING_FLOOR;
  const char* floorReason = rainyFloorActive
    ? "pack below the rainy-day floor" : "pack below evening floor";

  if (srcActual == SRC_UTILITY) {
    if (soc >= floorSoc + SOC_RECOVER_HYST) {
      srcReason = "pack recovered, back on battery";
      return SRC_SOLAR;
    }
    srcReason = floorReason;
    return SRC_UTILITY;
  }
  if (soc > floorSoc) { srcReason = "evening, running on the pack"; return SRC_SOLAR; }
  srcReason = floorReason;
  return SRC_UTILITY;
}

static void relayTask() {
  uint32_t now = millis();

  if (inChangeover) {
    if ((int32_t)(now - deadUntil) >= 0) {
      setSourceRelays(srcTarget);
      srcActual = srcTarget;
      srcSince = now;
      inChangeover = false;
      Serial.printf("[relay] now on %s (%s)\n", srcName(srcActual), srcReason);
    }
    return;
  }

  Source want = decideSource();
  if (want == srcActual) return;

  BmsData b = bmsGet();
  bool urgent = (want == SRC_UTILITY && bmsFresh() && b.soc <= SOC_CRITICAL) ||
                srcActual == SRC_NONE;
  if (!urgent && now - srcSince < SOURCE_MIN_DWELL_MS) return;

  Serial.printf("[relay] %s -> %s : %s\n", srcName(srcActual), srcName(want), srcReason);
  setSourceRelays(SRC_NONE);            // break before make
  srcActual = SRC_NONE;
  srcTarget = want;
  deadUntil = now + RELAY_DEAD_TIME_MS;
  inChangeover = true;
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
      buzzMode = BUZZ_SILENT;
      melody = nullptr;
      buzzerOutput(0);
    }
    return;
  }

  int soc = b.soc;

  if (loadCutoff) {
    if (soc >= SOC_LOAD_CUTOFF + SOC_ALARM_HYST) {
      loadCutoff = false;
      Serial.printf("[prot] SoC %d%%, load reconnected\n", soc);
    }
  } else if (soc <= SOC_LOAD_CUTOFF) {
    loadCutoff = true;
    Serial.printf("[prot] SoC %d%%, LOAD DISCONNECTED\n", soc);
  }

  BuzzTone want;
  if (loadCutoff)                    want = BUZZ_ALARM;
  else if (soc <= SOC_BUZZER_WARN)   want = BUZZ_WARN;
  else if (buzzMode != BUZZ_SILENT &&
           soc < SOC_BUZZER_WARN + SOC_ALARM_HYST) want = buzzMode;
  else                               want = BUZZ_SILENT;

  if (want != buzzMode) {
    buzzMode = want;
    lastMelody = 0;                  // sound the new state at once
    if (want == BUZZ_SILENT) { melody = nullptr; buzzerOutput(0); }
    Serial.printf("[buzz] %s (soc %d%%)\n",
                  want == BUZZ_ALARM ? "ALARM" : want == BUZZ_WARN ? "warn" : "silent", soc);
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
    uint32_t period = (buzzMode == BUZZ_ALARM) ? BUZZER_ALARM_PERIOD_MS : BUZZER_WARN_PERIOD_MS;
    if (now - lastMelody >= period) {
      if (buzzMode == BUZZ_ALARM)
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

  bool want = manualLight;
  struct tm tmv;
  if (travelMode && localNow(&tmv)) want = inTravelWindow(tmv);
  if (loadCutoff) want = false;              // protection overrides everything

  if (want != lightOn) {
    lightOn = want;
    relayDrive(RELAY_LOAD_PIN, lightOn);
    Serial.printf("[load] %s%s\n", lightOn ? "ON" : "OFF",
                  loadCutoff ? " (battery protection)" : "");
  }
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
    char buf[1500];
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
      manualLight = req->getParam("light")->value().toInt() != 0;
      Serial.printf("[web] load request: %d\n", manualLight);
    }
    req->send(200, "application/json",
      String("{\"src\":\"") + srcName(srcActual) + "\",\"manual\":" +
      (manualSrc != SRC_NONE ? "true" : "false") +
      ",\"light\":" + (lightOn ? "true" : "false") +
      ",\"cutoff\":" + (loadCutoff ? "true" : "false") + "}");
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    char buf[560];
    snprintf(buf, sizeof(buf),
      "{\"socCritical\":%d,\"socEveningFloor\":%d,\"socTarget1600\":%d,"
      "\"socRainyFloor\":%d,\"rainyCheck\":\"%02d:%02d\",\"rainyModeTonight\":%s,"
      "\"socBuzzerWarn\":%d,\"socLoadCutoff\":%d,"
      "\"eveningHour\":%d,\"topupHour\":%d,\"capacityAh\":%.0f,"
      "\"travelOn\":\"%02d:%02d\",\"travelOff\":\"%02d:%02d\","
      "\"logSec\":%lu,\"liveSec\":%lu}",
      SOC_CRITICAL, SOC_EVENING_FLOOR, SOC_TARGET_1600,
      SOC_RAINY_FLOOR, RAINY_CHECK_HOUR, RAINY_CHECK_MIN,
      rainyFloorActive ? "true" : "false",
      SOC_BUZZER_WARN, SOC_LOAD_CUTOFF,
      EVENING_HOUR, TOPUP_START_HOUR, PACK_CAPACITY_AH,
      TRAVEL_ON_HOUR, TRAVEL_ON_MIN, TRAVEL_OFF_HOUR, TRAVEL_OFF_MIN,
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

  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();
    bool wifiUp = WiFi.status() == WL_CONNECTED;

    if (wifiUp && !wifiWasUp) onWifiUp();
    if (!wifiUp && wifiWasUp) tlsDrop("wifi lost");
    wifiWasUp = wifiUp;

    if (!wifiUp) {
      if (now - tWifi >= WIFI_RETRY_MS) {
        tWifi = now;
        Serial.println("[WiFi] retrying...");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // Hold every TLS handshake until the BMS link is up (or the
    // grace period expires): a handshake takes the radio for a
    // moment, and doing that while NimBLE is still walking the
    // GATT table is what made the BMS connect intermittently.
    bool cloudOk = bmsShared.lastFrameMs || now >= WEB_START_GRACE_MS;

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

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================================================
//  SECTION 17 - SETUP / CONTROL LOOP
// ============================================================
void setup() {
  // Safe electrical state before anything else can take time:
  // both source relays open, load open, buzzer silent.
  pinMode(RELAY_UTILITY_PIN, OUTPUT);
  pinMode(RELAY_SOLAR_PIN, OUTPUT);
  setSourceRelays(SRC_NONE);
  if (RELAY_LOAD_PIN >= 0) { pinMode(RELAY_LOAD_PIN, OUTPUT); relayDrive(RELAY_LOAD_PIN, false); }
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

  Serial.begin(115200);
  delay(2000);                       // one-off, lets USB serial enumerate
  Serial.println();
  Serial.println("=== SolarPulse v4 ===");

  dataMux = xSemaphoreCreateMutex();
  fsLock  = xSemaphoreCreateMutex();

  if (!prefs.begin("solar", false)) Serial.println("B: NVS failed, counters not saved");
  else                              Serial.println("B: NVS open");

  loadCounters();
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

  xTaskCreatePinnedToCore(bleTask, "ble", 4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 1);

  Serial.printf("I: setup complete, heap %lu\n", (unsigned long)ESP.getFreeHeap());
}

// The control loop. Deterministic, non-blocking, and independent
// of both the network and the BMS link: relays and the buzzer keep
// doing the right thing even with everything else down.
void loop() {
  static uint32_t tCtl = 0, tLog = 0, tRoll = 0, tNvs = 0, tStat = 0;
  uint32_t now = millis();

  esp_task_wdt_reset();
  webServerTask();

  // Melody stepping runs on EVERY pass: its shortest note is 70 ms,
  // and quantising to the 250 ms control tick would wreck the rhythm.
  buzzerTick();

  if (now - tCtl >= CONTROL_TICK_MS) {
    tCtl = now;
    BmsData b = bmsGet();
    integrateEnergy(b);
    protectionUpdate();     // sets loadCutoff / buzzMode from SoC
    travelTask();           // applies loadCutoff to the load relay
    relayTask();
    utilityAccounting();
  }

  if (now - tLog  >= OFFLINE_LOG_MS) { tLog  = now; logSample(); }
  if (now - tRoll >= 10000)          { tRoll = now; rolloverCheck(); }
  if (now - tNvs  >= NVS_SAVE_MS)    { tNvs  = now; saveCounters(); }

  if (now - tStat >= 10000) {
    tStat = now;
    BmsData b = bmsGet();
    Serial.printf("[stat] ble=%s(%lu) wifi=%s soc=%u%% %.2fV %.2fA src=%s%s%s q=%u heap=%lu\n",
                  bleStateName(bleState), (unsigned long)bleReconnects,
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  b.soc, b.packV, b.packI, srcName(srcActual),
                  travelMode ? " travel" : "", loadCutoff ? " CUTOFF" : "",
                  bufferedLines, (unsigned long)ESP.getFreeHeap());
  }

  vTaskDelay(pdMS_TO_TICKS(20));
}
