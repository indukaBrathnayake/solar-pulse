# Solar Pulse

Web dashboard for a 580 W off-grid solar rig with a JK BMS on a 4S LiFePO4 100 Ah pack.
An ESP32 reads the BMS over Bluetooth, keeps counters through power cuts and WiFi cuts,
and pushes everything to Firebase. A static page on GitHub Pages shows it live from
anywhere in the world.

```
JK BMS --BLE--> ESP32 --WiFi--> Firebase RTDB --websocket--> GitHub Pages dashboard
                  |
             LittleFS buffer (samples saved while WiFi is down, flushed later)
```

**New here? Follow `SETUP.md` instead.** It walks through every phase in order with a check at the end of each one. This README is the reference summary.

## Repository layout

- `firmware/SolarPulse/SolarPulse.ino` - ESP32 firmware
- `firmware/SolarPulse/config.h` - the only file to edit (WiFi, BMS MAC, Firebase)
- `docs/` - the dashboard site, served by GitHub Pages
- `SETUP.md` - the full step-by-step build guide

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

- Arduino IDE > Boards Manager > install "esp32 by Espressif"
- Library Manager > install "NimBLE-Arduino" (1.4.x recommended)
- Open `firmware/SolarPulse/SolarPulse.ino`
- Edit `config.h`: WiFi name and password, BMS MAC from the BMS label, Firebase host
- Select your ESP32 board and port, upload
- Open Serial Monitor at 115200 to watch it connect

Important: close the JK phone app first. The BMS accepts one Bluetooth
client at a time. If the app is open, the ESP32 cannot connect, and the
other way around.

## 3. Publish the dashboard

- Edit `docs/app.js`: paste the Firebase web config at the top
- Push this repository to GitHub
- Repo Settings > Pages > Source: Deploy from a branch > `main` / `docs`
- Open `https://<your-username>.github.io/<repo-name>/`

## How records are kept

Three paths in the database:

- `/live` - one snapshot, overwritten every 15 s. Drives the whole top of the page.
- `/history/YYYY-MM-DD/<epoch>` - one small sample per minute. Drives the
  "today power" curve. About 170 KB per day, roughly 60 MB per year, far
  inside the 1 GB free tier. Old months can be deleted from the console if
  space ever matters.
- `/daily/YYYY-MM-DD` - one row per day: Wh charged, Wh discharged, peak W.
  Thirty rows make the month chart, 365 rows make the year total. This path
  is tiny and can be kept forever.

The ESP32 also keeps `today` and `lifetime` Wh counters in its own flash
(NVS), saved every 5 minutes, so a power cut does not reset the totals.

## What happens when home WiFi is off

- The ESP32 keeps reading the BMS normally
- Every 2 minutes it appends a sample to a file in its flash (LittleFS),
  up to about 50 hours of backlog
- Energy counters keep integrating and keep being saved to NVS
- When WiFi returns, it syncs the clock, then uploads the whole backlog
  into `/history` in small batches, then resumes live pushes
- The dashboard notices the silence: after 90 s without a fresh `/live`
  timestamp it switches to "Monitor offline", greys the energy river, and
  keeps showing the last saved values with their time

## Status logic on the page

- Monitor online: `/live` timestamp fresher than 90 s
- BMS linked: last BLE frame fresher than 15 s (reported by the ESP)
- PV generating: battery charging during 06:00-19:00
- Charging (grid?): charging outside daylight, flagged separately since a
  hybrid inverter can also charge from the grid
- Inverter feeding load: battery discharging beyond 0.25 A
- Balancer, MOS states, alarm bits: straight from the BMS frame

## Known limits of this design, and fixes

1. Battery-side measurement only. Daytime loads fed straight from PV never
   pass through the battery, so "harvested" undercounts true generation.
   Fix later: a DC meter (PZEM-017) on the PV input, or a shunt + op-amp
   into the ESP32 ADC.
2. Charging source is inferred by time of day. A hybrid inverter charging
   from the grid at 07:00 would be counted as PV. Fix: disable grid
   charging in the inverter, or add the PV-side meter.
3. One BLE client at a time. Phone app and ESP32 cannot connect together.
4. TLS is not verified (`setInsecure`). Data is encrypted in transit but
   the server is not authenticated. Fix: pin the root certificate.
5. Test-mode database is world-writable for 30 days. Lock the rules as
   shown above.
6. The Firebase web config in `app.js` is public by design; the rules are
   the real protection. Keep `config.h` private (it may hold the secret).
7. Clock drift while offline. Without WiFi the clock runs on the crystal
   and restores from a 5-minute-old save after a reboot, so buffered
   samples can be minutes off. Samples taken before any sync are marked
   `approx` and skipped at upload.
8. If the ESP32 is powered from the inverter AC, a blackout also silences
   the monitor. Powering it from the battery through a small buck keeps
   monitoring alive through outages.
9. A midnight WiFi outage delays the closing write of `/daily`; the totals
   still exist in `/history` samples, so nothing is lost.

## References

[1] syssi, "esphome-jk-bms," GitHub. https://github.com/syssi/esphome-jk-bms
[2] JinkoSolar, "JKM580N-72HL4-(V) datasheet," 2024.
[3] Google, "Firebase Realtime Database REST API," firebase.google.com/docs/database/rest/start
