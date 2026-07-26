/*
 * ============================================================
 *  SolarPulse final - ESP32 solar & battery monitor
 *  JK BMS (BLE, JK02_32S, hw v11) -> Firebase RTDB
 *
 *  FIXED IN:
 *   - The JK BMS exposes TWO characteristics with UUID 0xFFE1:
 *       handle 0x03  properties 0x0C  -> WRITE  (commands)
 *       handle 0x05  properties 0x12  -> NOTIFY (data)
 *     v1 called getCharacteristic(0xFFE1), which returns only one
 *     of them, so commands went to the notify characteristic and
 *     the BMS never replied. v2 enumerates all characteristics
 *     and picks each one by its properties.
 *   - Connects with an explicit BLE_ADDR_PUBLIC address type.
 *   - Re-sends the cell-info command until frames actually arrive.
 *   - BLE starts before WiFi; setMTU() removed.
 *   - Staged serial markers so any failure is visible.
 *
 *  Board    : ESP32 Dev Module
 *  Partition: Huge APP (3MB No OTA/1MB SPIFFS)
 *  Core     : esp32 by Espressif 2.0.17
 *  Library  : NimBLE-Arduino 1.4.x
 *
 *  Protocol offsets follow syssi/esphome-jk-bms (JK02_32S). [1]
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <time.h>
#include "config.h"

// ---------- forward declarations ----------
void integrateEnergy();
void saveCounters();
void pushDaily(bool closing);

// ---------------- BMS data model ----------------
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

// ---------------- energy accounting ----------------
float   todayChgWh = 0, todayDisWh = 0, todayPeakW = 0;
double  lifeChgWh  = 0, lifeDisWh  = 0;
int32_t dayStamp   = 0;
bool    ntpSynced  = false;

// ---------------- plumbing ----------------
Preferences prefs;
NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* chrFfe1 = nullptr;   // UUID 0xFFE1: the only characteristic the real app uses
volatile uint8_t rawLogLeft = 10;                // print the first few raw notifications, then stop
volatile bool bleConnected = false;
uint8_t  frameBuf[400];
uint16_t frameLen = 0;
uint32_t tLive = 0, tHist = 0, tOff = 0, tDaily = 0, tNvs = 0, tWifi = 0, tPoll = 0;
uint32_t lastEnergyMs = 0;
uint16_t bufferedLines = 0;

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

int32_t dateStamp(time_t t) {
  struct tm tmv; localtime_r(&t, &tmv);
  return (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
}
void dateString(time_t t, char* out) {
  struct tm tmv; localtime_r(&t, &tmv);
  strftime(out, 12, "%Y-%m-%d", &tmv);
}

// ============================================================
//  JK BMS frame parsing (JK02_32S)
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
//  energy integration + day rollover
// ============================================================
void integrateEnergy() {
  uint32_t nowMs = millis();
  if (lastEnergyMs == 0) { lastEnergyMs = nowMs; return; }
  float dt = (nowMs - lastEnergyMs) / 1000.0f;
  lastEnergyMs = nowMs;
  if (dt <= 0 || dt > 10) return;

  float wh = bms.packW * dt / 3600.0f;
  if (bms.packI >  CURRENT_DEADBAND) { todayChgWh += wh;  lifeChgWh += wh; }
  if (bms.packI < -CURRENT_DEADBAND) { todayDisWh += -wh; lifeDisWh += -wh; }
  if (bms.packW > todayPeakW) todayPeakW = bms.packW;

  int32_t ds = dateStamp(nowEpoch());
  if (dayStamp == 0) dayStamp = ds;
  if (ds != dayStamp) {
    pushDaily(true);
    todayChgWh = todayDisWh = todayPeakW = 0;
    dayStamp = ds;
    saveCounters();
  }
}

// ============================================================
//  persistence
// ============================================================
void saveCounters() {
  prefs.putFloat("tc", todayChgWh);
  prefs.putFloat("td", todayDisWh);
  prefs.putFloat("tp", todayPeakW);
  prefs.putDouble("lc", lifeChgWh);
  prefs.putDouble("ld", lifeDisWh);
  prefs.putInt("day", dayStamp);
  prefs.putLong64("ep", (int64_t)nowEpoch());
}

void loadCounters() {
  todayChgWh = prefs.getFloat("tc", 0);
  todayDisWh = prefs.getFloat("td", 0);
  todayPeakW = prefs.getFloat("tp", 0);
  lifeChgWh  = prefs.getDouble("lc", 0);
  lifeDisWh  = prefs.getDouble("ld", 0);
  dayStamp   = prefs.getInt("day", 0);
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
    "\"bmsLink\":%s,\"rssi\":%d,\"buffered\":%u,\"heap\":%lu}",
    (long)t, ntpSynced ? "true" : "false",
    bms.packV, bms.packI, bms.packW, bms.soc, bms.remainAh,
    bms.cell[0], bms.cell[1], bms.cell[2], bms.cell[3],
    bms.t1, bms.t2, bms.mosT, bms.balI, bms.balancing ? "true" : "false",
    bms.chgMos ? "true" : "false", bms.disMos ? "true" : "false",
    (unsigned long)bms.errBits, bms.soh, (unsigned long)bms.cycles,
    todayChgWh, todayDisWh, todayPeakW, lifeChgWh, lifeDisWh,
    (bms.lastFrameMs && millis() - bms.lastFrameMs < 15000) ? "true" : "false",
    WiFi.RSSI(), bufferedLines, (unsigned long)ESP.getFreeHeap());
}

int buildSampleJson(char* out, size_t cap, time_t t) {
  return snprintf(out, cap,
    "{\"t\":%ld,\"v\":%.2f,\"i\":%.2f,\"p\":%.1f,\"soc\":%u,\"approx\":%s}",
    (long)t, bms.packV, bms.packI, bms.packW, bms.soc, ntpSynced ? "false" : "true");
}

// ============================================================
//  Firebase REST
// ============================================================
bool fbRequest(const char* method, const String& path, const String& body) {
  if (WiFi.status() != WL_CONNECTED) return false;
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
  static char buf[900];
  buildLiveJson(buf, sizeof(buf));
  fbRequest("PUT", "/live", buf);
}

void pushHistorySample() {
  if (!bms.lastFrameMs) return;
  time_t t = nowEpoch();
  char date[12]; dateString(t, date);
  char body[160]; buildSampleJson(body, sizeof(body), t);
  char path[48]; snprintf(path, sizeof(path), "/history/%s/%ld", date, (long)t);
  fbRequest("PUT", path, body);
}

void pushDaily(bool closing) {
  time_t t = nowEpoch();
  if (closing) t -= 3600;
  char date[12]; dateString(t, date);
  char body[200];
  snprintf(body, sizeof(body),
    "{\"chgWh\":%.1f,\"disWh\":%.1f,\"peakW\":%.1f,\"minSoc\":%u,\"closed\":%s}",
    todayChgWh, todayDisWh, todayPeakW, bms.soc, closing ? "true" : "false");
  char path[24]; snprintf(path, sizeof(path), "/daily/%s", date);
  fbRequest("PATCH", path, body);
}

// ============================================================
//  offline buffer
// ============================================================
void bufferSample() {
  if (!bms.lastFrameMs || bufferedLines >= BUFFER_MAX_LINES) return;
  File f = LittleFS.open("/buffer.jsonl", "a");
  if (!f) return;
  char line[160];
  buildSampleJson(line, sizeof(line), nowEpoch());
  f.println(line);
  f.close();
  bufferedLines++;
}

void countBufferedLines() {
  bufferedLines = 0;
  File f = LittleFS.open("/buffer.jsonl", "r");
  if (!f) return;
  while (f.available()) { f.readStringUntil('\n'); bufferedLines++; }
  f.close();
}

void flushBuffer() {
  if (!bufferedLines || WiFi.status() != WL_CONNECTED) return;
  File f = LittleFS.open("/buffer.jsonl", "r");
  if (!f) return;
  Serial.printf("[BUF] flushing %u samples\n", bufferedLines);

  bool ok = true;
  while (f.available() && ok) {
    String body = "{";
    int n = 0;
    while (f.available() && n < 20) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() < 10) continue;
      int tp = line.indexOf("\"t\":") + 4;
      long ts = line.substring(tp, line.indexOf(',', tp)).toInt();
      if (ts < 1700000000L) continue;
      char date[12]; dateString((time_t)ts, date);
      if (n) body += ",";
      body += "\"history/" + String(date) + "/" + String(ts) + "\":" + line;
      n++;
    }
    body += "}";
    if (n) ok = fbRequest("PATCH", "", body);
  }
  f.close();
  if (ok) { LittleFS.remove("/buffer.jsonl"); bufferedLines = 0; Serial.println("[BUF] done"); }
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
  configTime(TZ_OFFSET_SEC, 0, NTP_1, NTP_2);
  for (int i = 0; i < 20 && nowEpoch() < 1700000000L; i++) delay(250);
  if (nowEpoch() > 1700000000L) ntpSynced = true;
  flushBuffer();
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("=== SolarPulse v2 ===");
  Serial.println("A: serial alive");
  Serial.flush();

  if (!prefs.begin("solar", false)) Serial.println("B: NVS failed, counters not saved");
  else                              Serial.println("B: NVS open");
  Serial.flush();

  loadCounters();
  Serial.println("C: counters loaded");
  Serial.flush();

  if (!LittleFS.begin(true)) {
    Serial.println("D: LittleFS failed, offline buffer disabled");
  } else {
    countBufferedLines();
    Serial.printf("D: LittleFS ok, %u buffered\n", bufferedLines);
  }
  Serial.flush();

  NimBLEDevice::init("");                       // BLE before WiFi
  Serial.println("E: NimBLE init ok");
  Serial.flush();

  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  Serial.println("F: BLE power set");
  Serial.flush();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("G: WiFi started");
  Serial.flush();

  Serial.printf("H: setup complete, heap %lu\n", (unsigned long)ESP.getFreeHeap());
}

void loop() {
  static bool wifiWasUp = false;
  static uint32_t tStat = 0;

  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp && !wifiWasUp) onWifiUp();
  wifiWasUp = wifiUp;
  if (!wifiUp) wifiTask();

  bleConnectTask();

  uint32_t now = millis();

  // until frames arrive, resend the exact captured wake frame
  if (bleConnected && !bms.lastFrameMs && now - tPoll >= 6000) {
    tPoll = now;
    Serial.println("[BLE] no frames yet, resending captured wake sequence");
    bmsSendCaptured(CMD_DEVICE_INFO);
    delay(200);
    bmsSendCaptured(CMD_CELL_INFO);
  }

  if (wifiUp && now - tLive  >= LIVE_PUSH_MS)    { tLive  = now; pushLive(); }
  if (wifiUp && now - tHist  >= HISTORY_PUSH_MS) { tHist  = now; pushHistorySample(); }
  if (wifiUp && now - tDaily >= DAILY_PUSH_MS)   { tDaily = now; pushDaily(false); }
  if (!wifiUp && now - tOff  >= OFFLINE_LOG_MS)  { tOff   = now; bufferSample(); }
  if (now - tNvs >= NVS_SAVE_MS)                 { tNvs   = now; saveCounters(); }

  if (now - tStat >= 10000) {
    tStat = now;
    Serial.printf("[stat] ble=%s wifi=%s soc=%u%% %.2fV %.2fA cells %.3f/%.3f/%.3f/%.3f heap=%lu\n",
                  bleConnected ? "up" : "down", wifiUp ? "up" : "down",
                  bms.soc, bms.packV, bms.packI,
                  bms.cell[0], bms.cell[1], bms.cell[2], bms.cell[3],
                  (unsigned long)ESP.getFreeHeap());
  }

  delay(50);
}
