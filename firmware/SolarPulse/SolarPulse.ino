/*
 * ============================================================
 *  SolarPulse v3 - ESP32 solar, battery and load controller
 *  JK BMS (BLE, JK02_32S, hw v11) -> Firebase RTDB + local API
 *
 *  v2 -> v3 adds, without touching the BLE/BMS layer:
 *   - gap-free logging. Every sample is written to flash first
 *     and uploaded from a persistent queue with a committed
 *     read offset, so nothing is lost across WiFi cuts or
 *     reboots and the graph has no holes.
 *   - daily harvest in kWh, persisted and reset at local
 *     midnight from NTP time.
 *   - a 400-day daily-totals log on flash, aggregated into
 *     monthly totals for the new "Monthly" tab.
 *   - a source-selection state machine driving two relays
 *     (utility / inverter) with a dead time between them.
 *   - travel mode on a physical switch: lights on 18:00,
 *     off 23:30, every day.
 *   - ESPAsyncWebServer on the LAN with a JSON API and a
 *     small dashboard served from LittleFS.
 *
 *  KEPT FROM v2 (do not "fix"):
 *   - The JK BMS exposes TWO characteristics with UUID 0xFFE1:
 *       handle 0x03  properties 0x0C  -> WRITE  (commands)
 *       handle 0x05  properties 0x12  -> NOTIFY (data)
 *   - Connects with an explicit BLE_ADDR_PUBLIC address type.
 *   - Re-sends the cell-info command until frames actually arrive.
 *   - BLE starts before WiFi; setMTU() removed.
 *   - Staged serial markers so any failure is visible.
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
#if __has_include("esp_coexist.h")
  #include "esp_coexist.h"      // WiFi/BLE radio arbitration, see setup()
#endif
#include <time.h>
#include "config.h"

// ---------- forward declarations ----------
void integrateEnergy();
void saveCounters();
void pushDaily(bool closing);
void rolloverCheck();
void appendDailyRow(int32_t stamp);

// ============================================================
//  BMS data model  (unchanged from v2)
// ============================================================
struct BmsData {
  float cell[4]       = {0};
  float packV         = 0;
  float packI         = 0;       // + charging, - discharging
  float packW         = 0;
  float t1 = 0, t2 = 0, mosT = 0;
  float balI          = 0;
  uint8_t soc         = 0;
  float remainAh      = 0;
  float fullAh        = 0;
  uint32_t cycles     = 0;
  uint8_t soh         = 0;
  uint32_t errBits    = 0;
  bool chgMos = false, disMos = false, balancing = false;
  uint32_t lastFrameMs = 0;
};
BmsData bms;

// ============================================================
//  energy accounting
// ============================================================
float   todayChgWh = 0, todayDisWh = 0, todayPeakW = 0;
float   todayPvWh  = 0;            // only used when PV_ADC_ENABLE
double  lifeChgWh  = 0, lifeDisWh  = 0;
int32_t dayStamp   = 0;
bool    ntpSynced  = false;

float   pvW = 0;                   // instantaneous array power, W
float   loadW = 0;                 // instantaneous house draw, W

// harvest = what actually went into storage, or true PV power when
// a PV-side meter is fitted. See config.h section 6.
static inline float harvestWhToday() {
#if PV_ADC_ENABLE
  return todayPvWh;
#else
  return todayChgWh;
#endif
}

// ============================================================
//  relay / mode state
//  (the enum itself lives in config.h -- see the note there on why)
// ============================================================
Source   srcActual   = SRC_NONE;   // what is closed right now
Source   srcTarget   = SRC_NONE;   // what the changeover is heading to
Source   manualSrc   = SRC_NONE;   // web override, SRC_NONE = automatic
uint32_t manualSince = 0;
uint32_t srcSince    = 0;
uint32_t deadUntil   = 0;
bool     inChangeover = false;
bool     topUpEngaged = false;     // loads parked on utility to charge faster
const char* srcReason = "boot";

bool     rainyCheckDone   = false; // has the 18:15 target check run today
bool     rainyFloorActive = false; // true = today fell short, allow SOC_RAINY_FLOOR

bool     travelMode  = false;      // physical switch closed
bool     lightOn     = false;      // lighting relay state
bool     manualLight = false;      // web toggle used when not travelling
uint32_t utilitySecToday = 0;      // how long the house ran on CEB today

// ============================================================
//  plumbing
// ============================================================
Preferences prefs;
NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* chrFfe1 = nullptr;   // UUID 0xFFE1: the only characteristic the real app uses
volatile uint8_t rawLogLeft = 10;                // print the first few raw notifications, then stop
volatile bool bleConnected = false;
uint8_t  frameBuf[400];
uint16_t frameLen = 0;
uint32_t tLive = 0, tLog = 0, tDaily = 0, tNvs = 0, tWifi = 0, tPoll = 0;
uint32_t tUp = 0, tRoll = 0, tRelay = 0;
uint32_t lastEnergyMs = 0;
uint16_t bufferedLines = 0;        // samples still waiting in the queue

bool     fsOk = false;
uint32_t queuePos = 0;             // byte offset of the first unsent record
int32_t  clockAdj = 0;             // seconds to add to records logged before NTP
SemaphoreHandle_t fsLock = nullptr;

AsyncWebServer server(80);

#define FS_LOCK()   do { if (fsLock) xSemaphoreTake(fsLock, portMAX_DELAY); } while (0)
#define FS_UNLOCK() do { if (fsLock) xSemaphoreGive(fsLock); } while (0)

// ============================================================
//  helpers
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

// The clock is restored from NVS at boot, so this is true within a
// few minutes even before the first NTP reply of the session.
bool timeReady() { return nowEpoch() > 1700000000L; }

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

bool bmsFresh() { return bms.lastFrameMs && millis() - bms.lastFrameMs < 15000; }
bool bmsLost()  { return !bms.lastFrameMs || millis() - bms.lastFrameMs > 300000UL; }

const char* srcName(Source s) {
  return s == SRC_SOLAR ? "solar" : s == SRC_UTILITY ? "utility" : "none";
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
//  JK BMS frame parsing (JK02_32S)   -- unchanged from v2
// ============================================================
void parseCellInfo(const uint8_t* d) {
  for (int i = 0; i < 4; i++) bms.cell[i] = u16(d, 6 + i * 2) * 0.001f;
  bms.mosT      = s16(d, 144) * 0.1f;
  bms.packV     = u32(d, 150) * 0.001f;
  bms.packI     = s32(d, 158) * 0.001f;
  bms.packW     = bms.packV * bms.packI;
  bms.t1        = s16(d, 162) * 0.1f;
  bms.t2        = s16(d, 164) * 0.1f;
  bms.errBits   = u32(d, 166);
  bms.balI      = s16(d, 170) * 0.001f;
  bms.balancing = d[172] != 0x00;
  bms.soc       = d[173];
  bms.remainAh  = u32(d, 174) * 0.001f;
  bms.fullAh    = u32(d, 178) * 0.001f;
  bms.cycles    = u32(d, 182);
  bms.soh       = d[190];
  bms.chgMos    = d[198] != 0;
  bms.disMos    = d[199] != 0;
  bms.lastFrameMs = millis();
  rawLogLeft = 0;                       // stop raw dumping once parsing works
  integrateEnergy();
}

void assembleFrame(const uint8_t* data, size_t len) {
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

void notifyCB(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool) {
  if (rawLogLeft) {
    rawLogLeft--;
    Serial.printf("[BLE] notify h=0x%02X len=%u first8=", c->getHandle(), (unsigned)len);
    for (size_t i = 0; i < len && i < 8; i++) Serial.printf("%02X ", data[i]);
    Serial.println();
  }
  assembleFrame(data, len);
}

// command frame: AA 55 90 EB | cmd | len | value(4B LE) | ... | crc[19]
// ---------------------------------------------------------------
// Reverse-engineered from a real JK app BLE capture (btsnoop) on
// this exact BMS (hw V21H). These two 20-byte frames are sent
// verbatim by the official app before it will stream cell data.
// The trailing bytes are NOT random/session-based: they were
// identical across two separate connection sessions captured
// 14 minutes apart, so they are a fixed key this firmware checks
// for. Zero-padding (what earlier firmware versions sent) is
// silently ignored by the BMS -- that was the whole bug.
// Both go to UUID 0xFFE1 only. 0xFFE2 and 0xFFE3 are never
// touched by the real app and are not used here.
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

void bmsSendCaptured(const uint8_t* frame) {
  if (!chrFfe1) return;
  chrFfe1->writeValue((uint8_t*)frame, 20, false);   // write without response, exactly as captured
}

class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override { bleConnected = true; }
  void onDisconnect(NimBLEClient*) override {
    bleConnected = false;
    chrFfe1 = nullptr;
    Serial.println("[BLE] disconnected");
  }
};

// find UUID 0xFFE1 specifically. This one characteristic both
// accepts the wake commands and delivers all notifications.
bool discoverChars() {
  chrFfe1 = nullptr;
  rawLogLeft = 10;

  NimBLERemoteService* svc = bleClient->getService(NimBLEUUID((uint16_t)0xFFE0));
  if (!svc) { Serial.println("[BLE] service 0xffe0 missing"); return false; }

  chrFfe1 = svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFE1));
  if (!chrFfe1) { Serial.println("[BLE] characteristic 0xffe1 missing"); return false; }

  Serial.printf("[BLE] ffe1 handle=0x%02X notify=%d writeNR=%d\n",
                chrFfe1->getHandle(), chrFfe1->canNotify(), chrFfe1->canWriteNoResponse());

  if (!chrFfe1->canNotify() || !chrFfe1->subscribe(true, notifyCB, true)) {
    Serial.println("[BLE] subscribe to 0xffe1 failed");
    return false;
  }
  Serial.println("[BLE] subscribed to 0xffe1");
  return true;
}

void bleConnectTask() {
  static uint32_t lastTry = 0;
  if (bleConnected || millis() - lastTry < 10000) return;
  lastTry = millis();

  if (!bleClient) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new ClientCB(), false);
    bleClient->setConnectTimeout(10);
  }

  Serial.println("[BLE] connecting...");
  if (!bleClient->connect(NimBLEAddress(BMS_MAC, BLE_ADDR_PUBLIC))) {
    Serial.println("[BLE] connect failed (JK app still open?)");
    return;
  }
  Serial.println("[BLE] connected");

  delay(300);
  if (!discoverChars()) { bleClient->disconnect(); return; }

  delay(300);
  bmsSendCaptured(CMD_DEVICE_INFO);
  delay(300);
  bmsSendCaptured(CMD_CELL_INFO);
  Serial.println("[BLE] wake sequence sent");
}

// ============================================================
//  PV-side metering (optional, see config.h section 6)
// ============================================================
float readPvWatts() {
#if PV_ADC_ENABLE
  float mv = analogReadMilliVolts(PV_VOLT_PIN) * PV_VOLT_DIVIDER;
  float v  = mv / 1000.0f;
  float i  = (analogReadMilliVolts(PV_CURR_PIN) - PV_CURR_ZERO_MV) / PV_CURR_MV_PER_A;
  if (i < 0) i = 0;
  return v * i;
#else
  // no PV meter: everything that flows into the pack is harvest
  return bms.packW > 0 ? bms.packW : 0;
#endif
}

// The array is counted as producing when real power is going into
// storage during daylight. Used by the relay state machine.
bool solarProducing() {
  struct tm tmv;
  bool haveTime = localNow(&tmv);
  bool daylight = !haveTime || (tmv.tm_hour >= PV_HOUR_START && tmv.tm_hour < PV_HOUR_END);
  return bmsFresh() && daylight && pvW > SOLAR_OK_W;
}

// ============================================================
//  energy integration + day rollover
// ============================================================
void integrateEnergy() {
  uint32_t nowMs = millis();
  pvW   = readPvWatts();
  loadW = bms.packW < 0 ? -bms.packW : 0;

  if (lastEnergyMs == 0) { lastEnergyMs = nowMs; return; }
  float dt = (nowMs - lastEnergyMs) / 1000.0f;
  lastEnergyMs = nowMs;
  if (dt <= 0 || dt > 10) return;

  float wh = bms.packW * dt / 3600.0f;
  if (bms.packI >  CURRENT_DEADBAND) { todayChgWh += wh;  lifeChgWh += wh; }
  if (bms.packI < -CURRENT_DEADBAND) { todayDisWh += -wh; lifeDisWh += -wh; }
  if (bms.packW > todayPeakW) todayPeakW = bms.packW;
  todayPvWh += pvW * dt / 3600.0f;

  rolloverCheck();
}

// Runs from integrateEnergy AND from loop(), so midnight is not
// missed when the BLE link happens to be down at 00:00.
void rolloverCheck() {
  if (!timeReady()) return;
  int32_t ds = dateStamp(nowEpoch());
  if (dayStamp == 0) { dayStamp = ds; return; }
  if (ds == dayStamp) return;

  Serial.printf("[day] rollover %ld -> %ld, harvest %.1f Wh\n",
                (long)dayStamp, (long)ds, harvestWhToday());
  pushDaily(true);                 // close yesterday in Firebase
  appendDailyRow(dayStamp);        // and in local flash
  todayChgWh = todayDisWh = todayPeakW = todayPvWh = 0;
  utilitySecToday = 0;
  topUpEngaged = false;
  rainyCheckDone = false;          // re-run the 18:15 target check tonight
  rainyFloorActive = false;        // back to the normal 60% floor by default
  dayStamp = ds;
  saveCounters();
}

// ============================================================
//  persistence (NVS)
// ============================================================
void saveCounters() {
  prefs.putFloat("tc", todayChgWh);
  prefs.putFloat("td", todayDisWh);
  prefs.putFloat("tp", todayPeakW);
  prefs.putFloat("tv", todayPvWh);
  prefs.putDouble("lc", lifeChgWh);
  prefs.putDouble("ld", lifeDisWh);
  prefs.putInt("day", dayStamp);
  prefs.putUInt("qp", queuePos);
  prefs.putInt("adj", clockAdj);
  prefs.putUInt("us", utilitySecToday);
  prefs.putLong64("ep", (int64_t)nowEpoch());
}

void loadCounters() {
  todayChgWh = prefs.getFloat("tc", 0);
  todayDisWh = prefs.getFloat("td", 0);
  todayPeakW = prefs.getFloat("tp", 0);
  todayPvWh  = prefs.getFloat("tv", 0);
  lifeChgWh  = prefs.getDouble("lc", 0);
  lifeDisWh  = prefs.getDouble("ld", 0);
  dayStamp   = prefs.getInt("day", 0);
  clockAdj   = prefs.getInt("adj", 0);
  utilitySecToday = prefs.getUInt("us", 0);
  int64_t ep = prefs.getLong64("ep", 0);
  if (ep > 1700000000LL) {
    struct timeval tv = { .tv_sec = (time_t)ep, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
  }
}

// ============================================================
//  JSON builders
// ============================================================
int buildLiveJson(char* out, size_t cap) {
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
    "\"bmsLink\":%s,\"rssi\":%d,\"buffered\":%u,\"heap\":%lu}",
    (long)t, ntpSynced ? "true" : "false",
    bms.packV, bms.packI, bms.packW, bms.soc, bms.remainAh,
    bms.cell[0], bms.cell[1], bms.cell[2], bms.cell[3],
    bms.t1, bms.t2, bms.mosT, bms.balI, bms.balancing ? "true" : "false",
    bms.chgMos ? "true" : "false", bms.disMos ? "true" : "false",
    (unsigned long)bms.errBits, bms.soh, (unsigned long)bms.cycles,
    todayChgWh, todayDisWh, todayPeakW, lifeChgWh, lifeDisWh,
    pvW, loadW, srcActual == SRC_UTILITY ? loadW : 0.0f, harvestWhToday(),
    srcName(srcActual),
    srcActual == SRC_UTILITY ? "true" : "false",
    srcActual == SRC_SOLAR   ? "true" : "false",
    srcReason,
    manualSrc != SRC_NONE ? "true" : "false",
    travelMode ? "true" : "false", lightOn ? "true" : "false",
    (unsigned long)(utilitySecToday / 60),
    // tonight's plan: which SoC floor the evening rule will hold to,
    // and whether that is the relaxed rainy-day one. Decided at 18:15.
    rainyFloorActive ? "true" : "false",
    rainyFloorActive ? SOC_RAINY_FLOOR : SOC_EVENING_FLOOR,
    bmsFresh() ? "true" : "false",
    WiFi.RSSI(), bufferedLines, (unsigned long)ESP.getFreeHeap());
}

int buildSampleJson(char* out, size_t cap, time_t t) {
  return snprintf(out, cap,
    "{\"t\":%ld,\"v\":%.2f,\"i\":%.2f,\"p\":%.1f,\"soc\":%u,"
    "\"pv\":%.1f,\"src\":%u,\"approx\":%s}",
    (long)t, bms.packV, bms.packI, bms.packW, bms.soc,
    pvW, (unsigned)srcActual, ntpSynced ? "false" : "true");
}

// ============================================================
//  Firebase REST
// ============================================================
bool fbRequest(const char* method, const String& path, const String& body) {
  if (WiFi.status() != WL_CONNECTED) return false;
  // TLS needs a big contiguous block; skip rather than crash.
  if (ESP.getFreeHeap() < 45000) { Serial.println("[fb] low heap, push skipped"); return false; }
  WiFiClientSecure sec;
  sec.setInsecure();
  HTTPClient http;
  String url = "https://" + String(FIREBASE_HOST) + path + ".json";
  if (strlen(FIREBASE_AUTH)) url += "?auth=" + String(FIREBASE_AUTH);
  if (!http.begin(sec, url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, body);
  http.end();
  return code >= 200 && code < 300;
}

void pushLive() {
  static char buf[1400];
  buildLiveJson(buf, sizeof(buf));
  fbRequest("PUT", "/live", buf);
}

void pushDaily(bool closing) {
  time_t t = nowEpoch();
  if (closing) t -= 3600;
  char date[12]; dateString(t, date);
  char body[280];
  snprintf(body, sizeof(body),
    "{\"chgWh\":%.1f,\"disWh\":%.1f,\"harvestWh\":%.1f,\"peakW\":%.1f,"
    "\"minSoc\":%u,\"utilMin\":%lu,\"closed\":%s}",
    todayChgWh, todayDisWh, harvestWhToday(), todayPeakW, bms.soc,
    (unsigned long)(utilitySecToday / 60), closing ? "true" : "false");
  char path[24]; snprintf(path, sizeof(path), "/daily/%s", date);
  fbRequest("PATCH", path, body);
}

// ============================================================
//  FLASH LOG STORE
//
//  Two files are written every LOG_INTERVAL:
//    QUEUE_FILE   append-only JSON lines waiting to go to the
//                 cloud. QUEUE_POS_FILE holds the byte offset of
//                 the first record that has NOT been accepted
//                 yet, so an upload that dies half way, or a
//                 reboot, resumes exactly where it stopped.
//    /h/YYYYMMDD.csv  the same sample in CSV, kept for
//                 LOCAL_HISTORY_DAYS and served by the local UI.
//
//  Logging never depends on WiFi. That is the whole fix for the
//  gaps: the record exists on flash before anyone tries to send
//  it, and it is only forgotten once the server has said 200.
// ============================================================
void savePos() {
  File f = LittleFS.open(QUEUE_POS_FILE, "w");
  if (!f) return;
  f.print(queuePos);
  f.close();
}

void loadPos() {
  File f = LittleFS.open(QUEUE_POS_FILE, "r");
  if (!f) { queuePos = prefs.getUInt("qp", 0); return; }
  queuePos = (uint32_t)f.readString().toInt();
  f.close();
}

// number of records still waiting, for the UI badge
void countQueue() {
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

void resetQueue() {
  LittleFS.remove(QUEUE_FILE);
  queuePos = 0;
  savePos();
  bufferedLines = 0;
}

// Rewrite the queue so it starts at the first unsent record.
void compactQueue() {
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

// Last resort when the cloud has been unreachable for weeks:
// throw away the oldest quarter so new samples keep landing.
void dropOldest() {
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) return;
  size_t target = f.size() / 4;
  f.seek(target);
  f.readStringUntil('\n');              // realign to a record boundary
  uint32_t cut = f.position();
  f.close();
  // never move the pointer backwards: anything before queuePos is
  // already at the server, compaction alone reclaims that space
  if (cut > queuePos) queuePos = cut;
  Serial.println("[queue] full, oldest quarter dropped");
  compactQueue();
}

void appendHistoryCsv(time_t t) {
  char path[32];
  struct tm tmv; localtime_r(&t, &tmv);
  snprintf(path, sizeof(path), HISTORY_DIR "/%04d%02d%02d.csv",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  bool isNew = !LittleFS.exists(path);
  File f = LittleFS.open(path, "a");
  if (!f) return;
  if (isNew) f.println("t,v,i,p,soc,pv,src");
  f.printf("%ld,%.2f,%.2f,%.1f,%u,%.1f,%u\n",
           (long)t, bms.packV, bms.packI, bms.packW, bms.soc, pvW, (unsigned)srcActual);
  f.close();
}

// keep only the newest LOCAL_HISTORY_DAYS files in /h.
// Names are collected first: deleting while walking a directory
// handle is not safe.
void pruneHistory() {
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

// One row per finished day, kept for DAILY_LOG_DAYS. This is what
// the Monthly tab is built from, and it survives independently of
// the cloud.
void appendDailyRow(int32_t stamp) {
  if (!fsOk || !stamp) return;
  char date[12]; stampToString(stamp, date);
  FS_LOCK();
  bool isNew = !LittleFS.exists(DAILY_FILE);
  File f = LittleFS.open(DAILY_FILE, "a");
  if (f) {
    if (isNew) f.println("date,chgWh,disWh,harvestWh,peakW,endSoc,utilMin");
    f.printf("%s,%.1f,%.1f,%.1f,%.1f,%u,%lu\n",
             date, todayChgWh, todayDisWh, harvestWhToday(), todayPeakW,
             bms.soc, (unsigned long)(utilitySecToday / 60));
    f.close();
  }
  FS_UNLOCK();

  // prune: rewrite keeping the header plus the newest DAILY_LOG_DAYS rows
  FS_LOCK();
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

// The one place a sample is created. Called every LOG_INTERVAL
// whatever the network is doing.
void logSample() {
  if (!fsOk || !bms.lastFrameMs) return;
  time_t t = nowEpoch();
  char line[220];
  buildSampleJson(line, sizeof(line), t);

  FS_LOCK();
  File f = LittleFS.open(QUEUE_FILE, "a");
  if (f) {
    if (f.size() > QUEUE_MAX_BYTES) { f.close(); dropOldest(); f = LittleFS.open(QUEUE_FILE, "a"); }
    if (f) { f.println(line); f.close(); bufferedLines++; }
  }
  appendHistoryCsv(t);
  FS_UNLOCK();
}

// ============================================================
//  BULK UPLOAD
//
//  Sends the queue oldest-first in batches of QUEUE_BATCH as one
//  multi-path PATCH, which is Firebase's bulk endpoint. The read
//  offset only moves after a 2xx, so a failure repeats the batch
//  instead of skipping it.
//
//  Records written before the clock was ever synced carry
//  "approx":true. Instead of dropping them (v2 did, which is
//  exactly where the graph holes came from) they are shifted by
//  the correction measured at the first NTP reply.
// ============================================================
void uploadTask() {
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
  if (queuePos >= size) { f.close(); FS_UNLOCK(); if (size) { FS_LOCK(); resetQueue(); FS_UNLOCK(); } return; }
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
    if (ts < 1700000000L) continue;          // unrecoverable, skip the record
    char date[12]; dateString((time_t)ts, date);
    if (n) body += ",";
    body += "\"history/" + String(date) + "/" + String(ts) + "\":" + line;
    n++;
  }
  f.close();
  FS_UNLOCK();
  body += "}";

  if (!consumed) return;
  bool ok = n ? fbRequest("PATCH", "", body) : true;
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
//  RELAY CONTROL
//
//  Two relays, one GPIO each. A relay module input is an opto LED
//  behind a resistor: about 3 mA at 3.3 V, far below the 40 mA a
//  GPIO can source, and both poles of the relay (Live and Neutral)
//  are thrown by the one coil. So one pin per relay is correct.
//  The COIL is fed from a separate 5 V supply, never from the ESP.
//
//  Priority: solar/battery first, utility only when the pack
//  cannot carry the house.
//
//  Both relays are opened for RELAY_DEAD_TIME_MS on every change,
//  so the two sources can never be paralleled even for a cycle.
// ============================================================
static inline void relayDrive(int pin, bool on) {
  if (pin < 0) return;
#if RELAY_ACTIVE_LOW
  digitalWrite(pin, on ? LOW : HIGH);
#else
  digitalWrite(pin, on ? HIGH : LOW);
#endif
}

void relaysAllOff() {
  relayDrive(RELAY_UTILITY_PIN, false);
  relayDrive(RELAY_SOLAR_PIN, false);
}

// Which source should be feeding the house right now.
Source decideSource() {
  // 1. a choice made in the web UI wins until it expires
  if (manualSrc != SRC_NONE) {
    if (MANUAL_OVERRIDE_MS == 0 || millis() - manualSince < MANUAL_OVERRIDE_MS) {
      srcReason = "manual override";
      return manualSrc;
    }
    manualSrc = SRC_NONE;                       // expired, back to automatic
  }

  // 2. once-daily rainy-day check, at RAINY_CHECK time (18:15).
  //    Did the pack actually reach SOC_TARGET_1600 (99%) today? If
  //    not, today under-produced -- most likely rain -- and holding
  //    the normal SOC_EVENING_FLOOR (60%) would just mean falling
  //    back to utility every such evening. Relax the floor to
  //    SOC_RAINY_FLOOR (35%) for the rest of tonight instead, so the
  //    battery is used properly and utility is the last resort, not
  //    the default. This runs once per day regardless of manual
  //    override or a momentary BMS hiccup -- it only commits when
  //    this tick's SoC reading is actually fresh.
  if (!rainyCheckDone) {
    struct tm tmvChk;
    if (localNow(&tmvChk)) {
      bool pastCheckpoint = tmvChk.tm_hour > RAINY_CHECK_HOUR ||
                            (tmvChk.tm_hour == RAINY_CHECK_HOUR && tmvChk.tm_min >= RAINY_CHECK_MIN);
      if (pastCheckpoint && bmsFresh()) {
        rainyCheckDone = true;
        rainyFloorActive = bms.soc < SOC_TARGET_1600;
        Serial.printf("[relay] %02d:%02d check: soc=%u%% -> %s\n",
                      RAINY_CHECK_HOUR, RAINY_CHECK_MIN, bms.soc,
                      rainyFloorActive ? "target missed, floor relaxed to 35% for tonight (rainy mode)"
                                       : "target reached, normal 60% floor holds");

        // Hand the house straight back to the battery on this tick.
        // On a rainy day it is already on utility here (top-up parked
        // it there at 14:00, then the 60% floor held it), and the
        // recovery hysteresis below would demand SOC_RAINY_FLOOR +
        // SOC_RECOVER_HYST = 40% before releasing it. That hysteresis
        // exists to stop the relays chattering around the floor, not
        // to block this once-a-day scheduled handover, so skip it here.
        // Below the floor we fall through and utility keeps the house.
        if (rainyFloorActive && bms.soc > SOC_RAINY_FLOOR) {
          topUpEngaged = false;
          srcReason = "18:15, target missed: off CEB, running the pack down to 35%";
          return SRC_SOLAR;
        }
      }
    }
  }

  // 3. no BMS telemetry -> we do not know the SoC. Keep the house
  //    alive on utility rather than flatten an unknown pack.
  if (bmsLost()) { srcReason = "BMS link lost, failsafe"; return SRC_UTILITY; }
  if (!bmsFresh()) { srcReason = "waiting for BMS"; return srcActual == SRC_NONE ? SRC_UTILITY : srcActual; }

  int soc = bms.soc;
  bool solar = solarProducing();

  struct tm tmv;
  bool haveTime = localNow(&tmv);
  int hour = haveTime ? tmv.tm_hour : 12;       // no clock: behave like daytime
  bool evening = haveTime && (hour >= EVENING_HOUR || hour < PV_HOUR_START);

  if (!evening) {
    // ---------------- before 16:00 ----------------
    // Deep-discharge guard first.
    if (soc <= SOC_CRITICAL && !solar) { srcReason = "SoC critical, no sun"; return SRC_UTILITY; }

    // Top-up window. The pack must be at SOC_TARGET_1600 by 16:00
    // so the evening runs on stored energy. This hardware cannot
    // charge from the mains directly, so the lever we have is to
    // take the house OFF the inverter and park it on utility for a
    // while: every watt the array makes then goes into the battery
    // instead of into the fridge. Utility carries the house only
    // for as long as the pack is short of target.
    if (haveTime && hour >= TOPUP_START_HOUR && soc < SOC_TARGET_1600) {
      if (solar || topUpEngaged) {
        topUpEngaged = true;
        srcReason = "topping the pack up to 99% before 16:00";
        return SRC_UTILITY;
      }
      // no sun in the window: nothing to divert, so do not waste grid
    }
    if (soc >= SOC_TARGET_1600 || hour < TOPUP_START_HOUR) topUpEngaged = false;

    srcReason = solar ? "solar carrying the house" : "running from the pack";
    return SRC_SOLAR;
  }

  // ---------------- 16:00 to sunrise ----------------
  topUpEngaged = false;
  if (solar) { srcReason = "late sun still producing"; return SRC_SOLAR; }

  // Normal nights protect the pack at 60%. A night that missed
  // today's 99% target (rainyFloorActive, decided at 18:15 above)
  // is allowed down to 35% instead, so a cloudy day does not turn
  // into "utility every evening".
  int floor = rainyFloorActive ? SOC_RAINY_FLOOR : SOC_EVENING_FLOOR;
  const char* floorReason = rainyFloorActive
    ? "pack below the 35% rainy-day floor" : "pack below evening floor";

  if (srcActual == SRC_UTILITY) {
    // hysteresis so a sagging pack does not chatter the relays
    if (soc >= floor + SOC_RECOVER_HYST) {
      srcReason = "pack recovered, back on battery";
      return SRC_SOLAR;
    }
    srcReason = floorReason;
    return SRC_UTILITY;
  }
  if (soc > floor) { srcReason = "evening, running on the pack"; return SRC_SOLAR; }
  srcReason = floorReason;
  return SRC_UTILITY;
}

void relayTask() {
  uint32_t now = millis();

  // finish a changeover that is in its dead time
  if (inChangeover) {
    if ((int32_t)(now - deadUntil) >= 0) {
      if (srcTarget == SRC_SOLAR)   relayDrive(RELAY_SOLAR_PIN, true);
      if (srcTarget == SRC_UTILITY) relayDrive(RELAY_UTILITY_PIN, true);
      srcActual = srcTarget;
      srcSince = now;
      inChangeover = false;
      Serial.printf("[relay] now on %s (%s)\n", srcName(srcActual), srcReason);
    }
    return;
  }

  Source want = decideSource();
  if (want == srcActual) return;

  // urgent cases skip the dwell timer
  bool urgent = (want == SRC_UTILITY && bmsFresh() && bms.soc <= SOC_CRITICAL) ||
                srcActual == SRC_NONE;
  if (!urgent && now - srcSince < SOURCE_MIN_DWELL_MS) return;

  Serial.printf("[relay] %s -> %s : %s\n", srcName(srcActual), srcName(want), srcReason);
  relaysAllOff();                      // break before make
  srcActual = SRC_NONE;
  srcTarget = want;
  deadUntil = now + RELAY_DEAD_TIME_MS;
  inChangeover = true;
}

// how long the house has been on the grid today
void utilityAccounting() {
  static uint32_t last = 0;
  static uint32_t carryMs = 0;              // keep the sub-second remainder
  uint32_t now = millis();
  if (last && srcActual == SRC_UTILITY) {
    carryMs += now - last;
    utilitySecToday += carryMs / 1000;
    carryMs %= 1000;
  }
  last = now;
}

// ============================================================
//  TRAVEL MODE
//
//  Switch closed = nobody home. The lighting relay is then driven
//  purely by the clock, 18:00 on and 23:30 off, so the house looks
//  lived in. Switch open = the web UI toggle decides, which is the
//  behaviour when someone is home.
//
//  This overrides the lighting circuit only. Source selection
//  above keeps running exactly the same either way.
// ============================================================
bool inTravelWindow(const struct tm& tmv) {
  int nowMin = tmv.tm_hour * 60 + tmv.tm_min;
  int onMin  = TRAVEL_ON_HOUR  * 60 + TRAVEL_ON_MIN;
  int offMin = TRAVEL_OFF_HOUR * 60 + TRAVEL_OFF_MIN;
  if (onMin <= offMin) return nowMin >= onMin && nowMin < offMin;
  return nowMin >= onMin || nowMin < offMin;      // window crossing midnight
}

void travelTask() {
  // --- debounced switch read ---
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
      Serial.printf("[travel] %s\n", travelMode ? "ON - schedule takes over the lights"
                                                : "OFF - back to normal control");
    }
  }

  // --- lighting relay ---
  bool want = manualLight;
  struct tm tmv;
  if (travelMode && localNow(&tmv)) want = inTravelWindow(tmv);

  if (want != lightOn) {
    lightOn = want;
    relayDrive(RELAY_LOAD_PIN, lightOn);
    Serial.printf("[light] %s\n", lightOn ? "ON" : "OFF");
  }
}

// ============================================================
//  LOCAL WEB API
//
//  The GitHub Pages dashboard reads Firebase, so it works from
//  anywhere. This server is the fallback that keeps working when
//  the internet does not, and it is where /api/upload lives.
// ============================================================
static bool authOk(AsyncWebServerRequest* req) {
  if (strlen(WEB_USER) == 0) return true;
  if (req->authenticate(WEB_USER, WEB_PASS)) return true;
  req->requestAuthentication();
  return false;
}

// accumulates an /api/upload body across chunks
static String uploadBuf;

// One NDJSON record -> the right day CSV. Used by /api/upload so a
// second node (or a replay script) can backfill this device.
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

// month totals for one year, straight off DAILY_FILE
static String monthlyJson(int year) {
  float kwh[12] = {0}, used[12] = {0};
  int   days[12] = {0};

  FS_LOCK();
  File f = LittleFS.open(DAILY_FILE, "r");
  if (f) {
    f.readStringUntil('\n');                       // header
    while (f.available()) {
      String row = f.readStringUntil('\n');
      row.trim();
      if (row.length() < 12) continue;
      int y = row.substring(0, 4).toInt();
      int m = row.substring(5, 7).toInt();
      if (y != year || m < 1 || m > 12) continue;
      // date,chgWh,disWh,harvestWh,peakW,endSoc,utilMin
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

void setupWebServer() {
  // ---- live snapshot ----
  server.on("/api/live", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    char buf[1400];
    buildLiveJson(buf, sizeof(buf));
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", buf);
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
  });

  // ---- per-minute history for one day ----
  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    String date = req->hasParam("date") ? req->getParam("date")->value() : "";
    if (date.length() != 10) { char d[12]; dateString(nowEpoch(), d); date = d; }
    String path = String(HISTORY_DIR "/") + date.substring(0, 4) +
                  date.substring(5, 7) + date.substring(8, 10) + ".csv";
    if (!LittleFS.exists(path)) { req->send(404, "text/plain", "no data for " + date); return; }
    AsyncWebServerResponse* r = req->beginResponse(LittleFS, path, "text/csv");
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
  });

  // ---- the whole daily log ----
  server.on("/api/daily", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    if (!LittleFS.exists(DAILY_FILE)) { req->send(200, "text/csv", "date,chgWh,disWh,harvestWh,peakW,endSoc,utilMin\n"); return; }
    AsyncWebServerResponse* r = req->beginResponse(LittleFS, DAILY_FILE, "text/csv");
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
  });

  // ---- monthly totals, drives the Monthly tab ----
  server.on("/api/monthly", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    struct tm tmv;
    int year = localNow(&tmv) ? tmv.tm_year + 1900 : 2026;
    if (req->hasParam("year")) year = req->getParam("year")->value().toInt();
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", monthlyJson(year));
    r->addHeader("Access-Control-Allow-Origin", "*");
    req->send(r);
  });

  // ---- bulk ingest: NDJSON body, one sample object per line ----
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
      if (uploadBuf.length() > 16384) return;          // refuse to grow without bound
      for (size_t i = 0; i < len; i++) uploadBuf += (char)data[i];
    });

  // ---- manual source selection ----
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
      Serial.printf("[web] light request: %d\n", manualLight);
    }
    req->send(200, "application/json",
      String("{\"src\":\"") + srcName(srcActual) + "\",\"manual\":" +
      (manualSrc != SRC_NONE ? "true" : "false") +
      ",\"light\":" + (lightOn ? "true" : "false") + "}");
  });

  // ---- thresholds, so the UI can label things correctly ----
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    char buf[480];
    snprintf(buf, sizeof(buf),
      "{\"socCritical\":%d,\"socEveningFloor\":%d,\"socTarget1600\":%d,"
      "\"socRainyFloor\":%d,\"rainyCheck\":\"%02d:%02d\",\"rainyModeTonight\":%s,"
      "\"eveningHour\":%d,\"topupHour\":%d,\"capacityAh\":%.0f,"
      "\"travelOn\":\"%02d:%02d\",\"travelOff\":\"%02d:%02d\",\"logSec\":%lu}",
      SOC_CRITICAL, SOC_EVENING_FLOOR, SOC_TARGET_1600,
      SOC_RAINY_FLOOR, RAINY_CHECK_HOUR, RAINY_CHECK_MIN,
      rainyFloorActive ? "true" : "false",
      EVENING_HOUR, TOPUP_START_HOUR, PACK_CAPACITY_AH,
      TRAVEL_ON_HOUR, TRAVEL_ON_MIN, TRAVEL_OFF_HOUR, TRAVEL_OFF_MIN,
      (unsigned long)(OFFLINE_LOG_MS / 1000));
    req->send(200, "application/json", buf);
  });

  // ---- static dashboard from LittleFS /www ----
  server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "not found");
  });

  server.begin();
  Serial.println("[web] server on port 80");
}

// Bring the web server up only after the BMS link is established, or
// after WEB_START_GRACE_MS if the BMS never shows up (so the LAN page
// and the API still work while you debug a BLE problem).
//
// Why: AsyncTCP starts its own task and ESPAsyncWebServer holds
// buffers, and NimBLE needs contiguous heap plus radio time to make
// the connection and read the GATT table. Starting the server first
// was enough to stop the BMS connecting on this board.
#ifndef WEB_START_GRACE_MS
#define WEB_START_GRACE_MS 90000UL
#endif

void webServerTask() {
  static bool started = false;
  if (started) return;
  if (!bms.lastFrameMs && millis() < WEB_START_GRACE_MS) return;
  started = true;
  Serial.printf("[web] starting (%s), heap %lu\n",
                bms.lastFrameMs ? "BMS link up" : "grace period expired",
                (unsigned long)ESP.getFreeHeap());
  setupWebServer();
  if (WiFi.status() == WL_CONNECTED && MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[web] http://%s.local/\n", MDNS_NAME);
  }
}

// ============================================================
//  WiFi + time
// ============================================================
void wifiTask() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - tWifi < WIFI_RETRY_MS) return;
  tWifi = millis();
  Serial.println("[WiFi] retrying...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void onWifiUp() {
  Serial.printf("[WiFi] up, IP %s\n", WiFi.localIP().toString().c_str());

  time_t before = nowEpoch();
  configTime(TZ_OFFSET_SEC, 0, NTP_1, NTP_2);
  for (int i = 0; i < 20 && nowEpoch() < 1700000000L; i++) delay(250);

  if (nowEpoch() > 1700000000L) {
    if (!ntpSynced && before > 1000000000L) {
      // how far the free-running clock had drifted. Records logged
      // before this moment carry "approx":true and get shifted by
      // this amount at upload, instead of being thrown away.
      clockAdj = (int32_t)(nowEpoch() - before);
      if (clockAdj > 86400 || clockAdj < -86400) clockAdj = 0;   // nonsense, ignore
      Serial.printf("[time] clock corrected by %ld s\n", (long)clockAdj);
    }
    ntpSynced = true;
    rolloverCheck();
  }

  // mDNS is advertised by webServerTask() once the server is actually
  // listening, so nothing announces a port that is not open yet.
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  // Relays first, before anything can take time. Both sources open
  // and lights off is the safe state to power up in.
  pinMode(RELAY_UTILITY_PIN, OUTPUT);
  pinMode(RELAY_SOLAR_PIN, OUTPUT);
  relaysAllOff();
  if (RELAY_LOAD_PIN >= 0) { pinMode(RELAY_LOAD_PIN, OUTPUT); relayDrive(RELAY_LOAD_PIN, false); }
#if TRAVEL_SWITCH_ACTIVE_LOW
  pinMode(TRAVEL_SWITCH_PIN, INPUT_PULLUP);
#else
  pinMode(TRAVEL_SWITCH_PIN, INPUT_PULLDOWN);
#endif

  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("=== SolarPulse v3 ===");
  Serial.println("A: serial alive");
  Serial.flush();

  fsLock = xSemaphoreCreateMutex();

  if (!prefs.begin("solar", false)) Serial.println("B: NVS failed, counters not saved");
  else                              Serial.println("B: NVS open");
  Serial.flush();

  loadCounters();
  Serial.printf("C: counters loaded, today %.1f Wh in / %.1f Wh out\n", todayChgWh, todayDisWh);
  Serial.flush();

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
  Serial.flush();

  // read the travel switch once so the first schedule tick is right
  {
    int raw = digitalRead(TRAVEL_SWITCH_PIN);
#if TRAVEL_SWITCH_ACTIVE_LOW
    travelMode = (raw == LOW);
#else
    travelMode = (raw == HIGH);
#endif
    Serial.printf("E: travel switch on GPIO %d reads %s\n",
                  TRAVEL_SWITCH_PIN, travelMode ? "CLOSED (travel mode)" : "open (normal)");
  }

  NimBLEDevice::init("");                       // BLE before WiFi
  Serial.println("F: NimBLE init ok");
  Serial.flush();

  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  Serial.println("G: BLE power set");
  Serial.flush();

  // WiFi and BLE share ONE 2.4 GHz radio, time-sliced by the
  // coexistence arbiter. Tell it Bluetooth wins: the BMS link is a
  // connection that drops if it misses its slots, while a Firebase
  // push can simply be retried a second later.
#if __has_include("esp_coexist.h")
  esp_coex_preference_set(ESP_COEX_PREFER_BT);
  Serial.println("G2: coexistence set to prefer BT");
#endif

  WiFi.mode(WIFI_STA);
  // NOTE: do NOT call WiFi.setSleep(false) here. Disabling modem
  // sleep makes the WiFi stack hold the radio continuously and
  // starves BLE of airtime -- with it on, the BMS either never
  // connects or connects and then stops delivering frames. v2 left
  // the default (WIFI_PS_MIN_MODEM) and that is what works.
  WiFi.setSleep(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("H: WiFi started");
  Serial.flush();

  // The web server is started later, from loop(), once the BMS link
  // is up -- see webServerTask(). AsyncTCP's task and buffers take a
  // sizeable bite out of the heap, and NimBLE needs room to build the
  // connection and walk the GATT table. Coming up in that order keeps
  // the first BLE connect in the same quiet conditions as v2.
  Serial.printf("I: setup complete, heap %lu\n", (unsigned long)ESP.getFreeHeap());
}

void loop() {
  static bool wifiWasUp = false;
  static uint32_t tStat = 0;

  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp && !wifiWasUp) onWifiUp();
  wifiWasUp = wifiUp;
  if (!wifiUp) wifiTask();

  bleConnectTask();
  webServerTask();          // deferred until the BMS link is up

  uint32_t now = millis();

  // until frames arrive, resend the exact captured wake frame
  if (bleConnected && !bms.lastFrameMs && now - tPoll >= 6000) {
    tPoll = now;
    Serial.println("[BLE] no frames yet, resending captured wake sequence");
    bmsSendCaptured(CMD_DEVICE_INFO);
    delay(200);
    bmsSendCaptured(CMD_CELL_INFO);
  }

  // --- control loop, runs whatever the network is doing ---
  if (now - tRelay >= 1000) {
    tRelay = now;
    // pvW / loadW come from BMS frames. If the link drops, report zero
    // rather than freezing the last reading on the dashboard.
    if (!bmsFresh()) { pvW = 0; loadW = 0; }
    travelTask();
    relayTask();
    utilityAccounting();
  }

  // --- logging: flash first, always ---
  if (now - tLog >= OFFLINE_LOG_MS) { tLog = now; logSample(); }

  // --- day boundary, even if BLE is silent ---
  if (now - tRoll >= 10000) { tRoll = now; rolloverCheck(); }

  // --- cloud ---
  // Hold off every TLS handshake until the BMS link is up (or the
  // grace period expires). A handshake allocates tens of kB and takes
  // the radio for a moment; doing that while NimBLE is still trying to
  // connect and read the GATT table is what makes the BMS connect
  // intermittently. Nothing is lost by waiting: samples are already on
  // flash and go up from the queue afterwards.
  bool cloudOk = wifiUp && (bms.lastFrameMs || millis() >= WEB_START_GRACE_MS);
  if (cloudOk && now - tLive  >= LIVE_PUSH_MS)  { tLive  = now; pushLive(); }
  if (cloudOk && now - tDaily >= DAILY_PUSH_MS) { tDaily = now; pushDaily(false); }
  if (cloudOk && now - tUp    >= UPLOAD_TRY_MS) { tUp    = now; uploadTask(); }

  if (now - tNvs >= NVS_SAVE_MS) { tNvs = now; saveCounters(); }

  if (now - tStat >= 10000) {
    tStat = now;
    Serial.printf("[stat] ble=%s wifi=%s soc=%u%% %.2fV %.2fA src=%s%s q=%u heap=%lu\n",
                  bleConnected ? "up" : "down", wifiUp ? "up" : "down",
                  bms.soc, bms.packV, bms.packI, srcName(srcActual),
                  travelMode ? " travel" : "", bufferedLines,
                  (unsigned long)ESP.getFreeHeap());
  }

  delay(50);                // same cadence as the known-good v2 loop
}
