# Solar Pulse

Web dashboard and load controller for a 580 W off-grid solar rig with a JK BMS
on a 4S LiFePO4 100 Ah pack. An ESP32 reads the BMS over Bluetooth, decides
whether the house runs on the inverter or on utility power, keeps counters
through power cuts and WiFi cuts, and pushes everything to Firebase. A static
page on GitHub Pages shows it live from anywhere in the world.

```
JK BMS --BLE--> ESP32 --WiFi--> Firebase RTDB --websocket--> GitHub Pages dashboard
                  |  |
                  |  +-- ESPAsyncWebServer on the LAN (http://solarpulse.local/)
                  |
                  +-- LittleFS: every sample written to flash BEFORE it is sent,
                  |   uploaded from a queue with a committed read offset
                  |
                  +-- two relays: utility (CEB) / inverter, never both
                  +-- lighting relay + travel-mode switch
```

**New here? Follow `SETUP.md` instead.** It walks through every phase in order
with a check at the end of each one. This README is the reference summary.
**Wiring the relays and the travel switch: `WIRING.md`.**

## Repository layout

- `firmware/SolarPulse/SolarPulse.ino` — ESP32 firmware
- `firmware/SolarPulse/config.h` — the only file to edit (WiFi, BMS MAC, Firebase, pins, thresholds)
- `firmware/SolarPulse/config.h.example` — tracked template; copy it to `config.h`
- `firmware/SolarPulse/data/www/` — the on-device dashboard, uploaded to LittleFS
- `docs/` — the public dashboard site, served by GitHub Pages
- `WIRING.md` — relays, GPIO drive current, travel switch, optional PV meter
- `SETUP.md` — the full step-by-step build guide

## 1. Firebase setup (one time, free tier)

- Go to console.firebase.google.com, create a project (no Analytics needed)
- Build > Realtime Database > Create database > pick Singapore region > Test mode
- Copy the database URL (looks like `xxx-default-rtdb.asia-southeast1.firebasedatabase.app`)
- Project settings > Your apps > add a Web app > copy the config object

Test mode leaves the database open to anyone with the URL for 30 days.
Before that expires, set rules to public read, secret write:

```json
{
  "rules": {
    ".read": true,
    ".write": "auth != null"
  }
}
```

Then create a legacy database secret (Project settings > Service accounts >
Database secrets) and paste it into `FIREBASE_AUTH` in `config.h`.

## 2. Flash the ESP32

Libraries (Arduino IDE > Library Manager unless noted):

- **NimBLE-Arduino** 1.4.x
- **ESP Async WebServer** — me-no-dev, install from ZIP
- **AsyncTCP** — me-no-dev, install from ZIP, required by the above

Then:

- Boards Manager > install "esp32 by Espressif" (2.0.17 is what this is built against)
- Copy `config.h.example` to `config.h` and fill in WiFi, BMS MAC, Firebase host
- Tools > Board: `ESP32 Dev Module`
- Tools > Partition Scheme: `Huge APP (3MB No OTA/1MB SPIFFS)`
- Upload, then open Serial Monitor at 115200

Then upload the on-device web UI to flash:

- Install the **arduino-esp32 LittleFS Data Upload** plugin
- Tools > ESP32 LittleFS Data Upload (this writes `data/` to the filesystem)

The firmware runs fine without this step — you just get a 404 at
`http://solarpulse.local/` and lose the local dashboard. The JSON API still works.

Important: close the JK phone app first. The BMS accepts one Bluetooth
client at a time. If the app is open, the ESP32 cannot connect, and the
other way around.

## 3. Publish the dashboard

- Edit `docs/app.js`: paste the Firebase web config at the top
- Push this repository to GitHub
- Repo Settings > Pages > Source: Deploy from a branch > `main` / `docs`
- Open `https://<your-username>.github.io/<repo-name>/`

---

## How records are kept

Four paths in the database:

- `/live` — one snapshot, overwritten every 15 s. Drives the whole top of the page.
- `/history/YYYY-MM-DD/<epoch>` — one small sample per minute. Drives the
  "today power" curve. About 170 KB per day, roughly 60 MB per year, far
  inside the 1 GB free tier.
- `/daily/YYYY-MM-DD` — one row per day: Wh charged, Wh discharged, Wh
  harvested, peak W, minutes on utility. Twelve months of these make the
  Monthly tab. This path is tiny and can be kept forever.

On the ESP32 itself:

- **NVS** — today's and lifetime Wh counters, the queue offset, the clock,
  saved every 5 minutes so a power cut does not reset the totals
- **`/queue.jsonl` + `/queue.pos`** — the upload queue (below)
- **`/daily.csv`** — 400 days of daily totals, independent of the cloud
- **`/h/YYYYMMDD.csv`** — the last 3 days at one-minute resolution, served
  by the local UI

## Gap-free logging

This is the fix for the holes that used to appear in the graph after a WiFi cut.

1. Every 60 s a sample is appended to `/queue.jsonl` **whether or not WiFi is
   up**. The old firmware only buffered while WiFi was down, so a push that
   failed for any other reason (DNS, TLS, a Firebase hiccup, low heap) simply
   lost that minute.
2. `/queue.pos` holds the byte offset of the first record that has **not** been
   accepted by the server. It is only advanced after a 2xx response, and it is
   on flash, so an upload that dies half way — or a reboot — resumes at exactly
   the right record instead of skipping or duplicating.
3. On reconnect the backlog goes up oldest-first, 20 records per multi-path
   `PATCH`. Chronological order is a property of the file: it is append-only.
4. Once the offset passes 32 KB the file is rewritten from that point, so flash
   use stays flat. Past 300 KB of un-uploaded backlog (about 4 days) the oldest
   quarter is dropped and that is logged loudly.
5. Records written before the clock had ever been set carry `"approx":true`.
   The old firmware **threw those away**, which is where the first gap after
   every reboot came from. Now the drift measured at the first NTP reply is
   applied to them and they are uploaded like any other.

## Daily harvest

Instantaneous power is integrated continuously into a Wh counter, saved to NVS
every 5 minutes and restored at boot, so a restart mid-day resumes from where
it left off rather than from zero. At local midnight (NTP, `TZ_OFFSET_SEC`) the
day is closed: the totals are written to `/daily` and to `/daily.csv`, then
reset. The rollover check runs from `loop()` every 10 s as well as from the BMS
frame handler, so midnight is not missed if the Bluetooth link happens to be
down at the time.

`harvestWh` is the same number as `chgWh` unless a PV-side meter is fitted
(`PV_ADC_ENABLE`), because without one the only place power can be measured is
the battery. See "Known limits" below.

## Monthly tab

The dashboard has a bottom tab bar. **Monthly** shows a twelve-bar chart of
kWh harvested per month of the selected year, plus a table with monthly
harvested / used / days-logged totals. It is built from `/daily`, and the same
numbers are available offline from the ESP at `/api/monthly`.

## Source selection (relays)

Priority is **solar → battery → utility**, with utility used as little as
possible. Two relays, one GPIO each, and both are opened for 800 ms on every
change so the two sources can never be paralleled.

**Before 16:00**

- The house runs on the inverter. Whatever the array makes beyond the load
  charges the pack.
- If SoC falls to `SOC_CRITICAL` (20%) *and* the array is not producing, the
  house moves to utility to protect the pack.
- From `TOPUP_START_HOUR` (14:00), if SoC is still under `SOC_TARGET_1600`
  (99%), the house is parked on utility while the sun is up. This hardware
  cannot charge the battery from the mains, so the only lever available is to
  take the load off the inverter — then every watt the array makes goes into
  the pack instead of into the fridge. It releases as soon as the pack reaches
  target.

**From 16:00 to sunrise**

- The house runs on the pack for as long as it can.
- Below `SOC_EVENING_FLOOR` (60%) with no sun, it moves to utility.
- It only goes back to the pack once SoC has recovered by `SOC_RECOVER_HYST`
  (5 points), so a sagging pack cannot chatter the relays.

**Always**

- A minimum dwell of 60 s between changes, bypassed when SoC is critical.
- If the BMS link has been dead for 5 minutes the SoC is unknown, so the
  controller fails over to utility rather than flatten a pack it cannot see.
- A manual choice from the web UI wins for 30 minutes, then automatic resumes.

All thresholds are compile-time constants at the top of `config.h`.

## Travel mode

A toggle switch to ground (see `WIRING.md` for the pin — **not** GPIO 8 on a
classic ESP32, that pin is bonded to the SPI flash and the board will not boot).

- Switch closed: the lighting relay is driven purely by the clock, on at 18:00
  and off at 23:30, every day, overriding the web toggle.
- Switch open: the lighting relay follows the web UI as before.

Source selection is unaffected either way. The switch is read every second and
debounced for 500 ms; the schedule uses local time from NTP.

## Local API

Served by ESPAsyncWebServer on port 80 at `http://solarpulse.local/` (or the
device's IP). Useful when the internet is the thing that is down.

| Endpoint | Method | What it does |
|---|---|---|
| `/` | GET | the on-device dashboard from LittleFS |
| `/api/live` | GET | the same JSON that goes to `/live` |
| `/api/history?date=YYYY-MM-DD` | GET | that day's per-minute CSV |
| `/api/daily` | GET | the whole daily-totals CSV |
| `/api/monthly?year=YYYY` | GET | twelve monthly totals (harvested / used / days) |
| `/api/upload` | POST | bulk ingest, NDJSON body, one sample object per line |
| `/api/relay?src=auto\|solar\|utility&light=0\|1` | GET/POST | manual control |
| `/api/config` | GET | the compiled-in thresholds and schedule |

Set `WEB_USER` / `WEB_PASS` in `config.h` to put HTTP basic auth in front of all
of it. It is off by default.

## What happens when home WiFi is off

- The ESP32 keeps reading the BMS and keeps switching relays normally — none of
  the control logic depends on the network
- Every 60 s a sample lands on flash, and the local dashboard keeps serving it
- Energy counters keep integrating and keep being saved to NVS
- When WiFi returns it syncs the clock, corrects the timestamps of anything
  logged before the sync, uploads the whole backlog oldest-first, then resumes
  live pushes
- The public dashboard notices the silence: after 60 s without a fresh `/live`
  timestamp it switches to "Monitor offline", greys the energy river, and keeps
  showing the last saved values with their time

## Status logic on the page

- Monitor online: `/live` timestamp fresher than 60 s
- BMS linked: last BLE frame fresher than 15 s (reported by the ESP)
- Source pill: which relay is closed, straight from the controller
- Travel mode pill: appears only while the switch is closed
- PV generating: battery charging during 06:00–19:00
- Inverter feeding load: battery discharging beyond 0.25 A
- Balancer, MOS states, alarm bits: straight from the BMS frame

## Known limits of this design, and fixes

1. **Battery-side measurement only.** Daytime loads fed straight from PV never
   pass through the battery, so "harvested" undercounts true generation. Fix:
   a DC meter (PZEM-017) on the PV input, or a divider + hall sensor into ADC1
   — the firmware already supports the second option, set `PV_ADC_ENABLE 1`.
2. **Utility power is not metered.** `gridW` on the dashboard is the house load
   at the moment utility is carrying it, not a real energy meter reading. A
   PZEM-004T on the CEB feed would make it real.
3. **Charging source is inferred by time of day.** A hybrid inverter charging
   from the grid at 07:00 would be counted as PV.
4. **The 16:00 top-up cannot charge from mains.** It works by moving the load
   off the inverter so the array charges faster. If the day has no sun, no
   amount of grid time will reach 99% — the firmware detects this and does not
   waste grid power trying.
5. **One BLE client at a time.** Phone app and ESP32 cannot connect together.
6. **Two relays are not a mechanical interlock.** Exclusivity is enforced in
   firmware. Interlocked contactors enforce it in physics — see `WIRING.md`.
7. **TLS is not verified** (`setInsecure`). Data is encrypted in transit but the
   server is not authenticated. Fix: pin the root certificate.
8. **The local API has no auth by default** and no HTTPS. It is fine on a home
   LAN; set `WEB_USER`/`WEB_PASS` and never port-forward it.
9. **Test-mode database is world-writable for 30 days.** Lock the rules as
   shown above. The Firebase web config in `app.js` is public by design; the
   rules are the real protection. Keep `config.h` private.
10. **Heap is tight.** NimBLE, WiFi, TLS and AsyncWebServer share one ESP32.
    Firebase pushes are skipped below 45 KB free rather than risking a crash;
    watch the `heap=` figure in the serial `[stat]` line.
11. **If the ESP32 is powered from the inverter AC**, a blackout silences the
    monitor and stops the controller. Power it from the battery through a small
    buck.

## References

[1] syssi, "esphome-jk-bms," GitHub. https://github.com/syssi/esphome-jk-bms
[2] JinkoSolar, "JKM580N-72HL4-(V) datasheet," 2024.
[3] Google, "Firebase Realtime Database REST API," firebase.google.com/docs/database/rest/start
[4] Espressif, "ESP32 datasheet," section 2.2 — GPIO 6–11 SPI flash pin assignment.
