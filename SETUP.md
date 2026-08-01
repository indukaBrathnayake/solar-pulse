# Solar Pulse — full setup guide

Follow the phases in order. Each one ends with a check. Do not move on until that check passes.

Total time: about 3 hours spread over an evening for the monitor, plus another
hour for the relay wiring, plus one day of watching.

```
JK BMS --Bluetooth--> ESP32 --WiFi--> Firebase --> GitHub Pages
                        ^  |
                        |  +--> utility relay / solar relay / lighting relay
                        |  +--> local dashboard at http://solarpulse.local/
                   MP1584 buck from the battery
```

---

## Phase 0 — Gather everything (10 min)

Hardware:

- ESP32 dev kit and a USB data cable (some cheap cables are charge-only and will not flash)
- MP1584 buck module
- 470 uF electrolytic capacitor
- 0.5 A inline fuse and holder
- Two lengths of thin wire to reach from the inverter battery terminals to where the ESP32 will sit
- Multimeter

For the relay control (Phase 9, can be done later):

- A 3-channel opto-isolated relay module, contacts rated for your real load — or better, two interlocked contactors driven by the module
- A separate 5 V supply for the relay coils (a phone charger will do), plus wire to tie its ground to the ESP32 ground
- One SPST toggle switch for travel mode, and two wires to reach it
- An enclosure for the AC side. See `WIRING.md` before buying anything

Accounts:

- A Google account (for Firebase)
- A GitHub account

Information to have written down:

- BMS Bluetooth MAC from the label: `A4:C1:38:08:19:21`
- Home WiFi name and password (2.4 GHz network, since ESP32 cannot use 5 GHz)

---

## Phase 1 — Firebase (20 min)

1. Open `console.firebase.google.com` and sign in
2. Click **Add project**. Name it `solar-pulse`. Turn Google Analytics **off**. Create
3. In the left menu open **Build > Realtime Database**, then **Create Database**
4. Location: pick **Singapore (asia-southeast1)**. It is the closest region to Sri Lanka, so the dashboard feels faster
5. Security rules: choose **Start in test mode**. Enable
6. Copy the database URL shown at the top. It looks like:
   `https://solar-pulse-xxxxx-default-rtdb.asia-southeast1.firebasedatabase.app`
7. Click the gear icon > **Project settings**. Scroll to **Your apps**, click the web icon `</>`
8. Nickname it `dashboard`, skip Firebase Hosting, click Register app
9. Copy the whole `firebaseConfig` block shown on screen. Paste it into a notepad for later

**Check:** you now have two things saved: the database URL, and the firebaseConfig block.

Test mode allows open access for 30 days. The database itself is free forever. Phase 6 locks it down properly.

---

## Phase 2 — Power the ESP32 (30 min)

Do this before flashing so the board has a permanent home.

**Set the buck output first, with nothing connected to it.**

1. Connect the MP1584 input to your battery, or to any 12 V source
2. Leave the output wires free, not touching anything
3. Put the multimeter on the output pads
4. Turn the small trimpot slowly. Clockwise usually raises the voltage
5. Stop when it reads **5.00 V**. Wait a minute and confirm it holds
6. Disconnect the input

**Then wire it in.**

- Take the input from the **inverter battery terminals**, after the main DC breaker
- Put the 0.5 A fuse on the positive input wire, close to the tap point
- Solder the 470 uF capacitor across the output, minus stripe to negative
- Output positive goes to the ESP32 **VIN** pin. Never the 3V3 pin
- Output negative goes to any ESP32 **GND** pin

**Why the inverter terminals and not the BMS:** that point sits on the load side of the BMS, so the BMS undervoltage protection still covers the ESP32. Tapping the BMS VBAT pin instead would bypass that protection.

**Check:** the ESP32 onboard LED lights up when the fuse is in. Measure 5.0 V at VIN with the board running.

---

## Phase 3 — Prepare the Arduino IDE (20 min)

1. Install Arduino IDE 2.x
2. **File > Preferences > Additional board manager URLs**, paste:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. **Tools > Board > Boards Manager**, search `esp32`, install **esp32 by Espressif Systems**
4. **Tools > Manage Libraries**, search `NimBLE-Arduino`
5. Open the version dropdown and pick a **1.4.x** version. Do not install 2.x, since the callback signature changed and the code will not compile
6. The web server needs two libraries that are **not** in Library Manager. Download both as ZIPs from GitHub and add each with **Sketch > Include Library > Add .ZIP Library**:
   - `me-no-dev/ESPAsyncWebServer`
   - `me-no-dev/AsyncTCP` (required by the above; install it too or nothing compiles)
7. To upload the on-device dashboard you also need the filesystem plugin: **arduino-esp32 LittleFS Data Upload**. Follow its README to drop the `.vsix`/jar into your Arduino tools folder, then restart the IDE. This is optional — skip it and everything works except the local web page.

**Check:** under **Tools > Board > esp32**, an entry like `ESP32 Dev Module` is selectable, and **Sketch > Include Library** lists both `ESPAsyncWebServer` and `AsyncTCP`.

---

## Phase 4 — Flash the firmware (20 min)

1. Open `firmware/SolarPulse/SolarPulse.ino`
2. If there is no `config.h` next to it, copy `config.h.example` to `config.h` first. `config.h` is gitignored so your WiFi password never leaves the machine
3. Click the `config.h` tab and fill in five values:

```c
#define WIFI_SSID     "your wifi name"
#define WIFI_PASS     "your wifi password"
#define BMS_MAC       "A4:C1:38:08:19:21"
#define FIREBASE_HOST "solar-pulse-xxxxx-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH ""
```

Note on `FIREBASE_HOST`: paste the URL **without** `https://` and **without** the trailing slash.

Everything else in `config.h` — relay pins, SoC thresholds, the 16:00 target, the travel-mode schedule — has a working default and is commented in place. Leave it alone on the first flash.

4. **Tools > Board**: `ESP32 Dev Module`
5. **Tools > Partition Scheme**: `Huge APP (3MB No OTA/1MB SPIFFS)` — the filesystem needs that 1 MB
6. **Tools > Port**: pick the port that appears when the board is plugged in
7. Click Upload. If it stalls at "Connecting...", hold the BOOT button on the board until it starts
8. Then **Tools > ESP32 LittleFS Data Upload** to write `data/` (the local dashboard) into flash. Close the Serial Monitor first or the upload fails on a busy port

**Close the JK BMS app on your phone before the next step.** The BMS accepts one Bluetooth client at a time. If the app is connected, the ESP32 cannot get in.

9. Open **Tools > Serial Monitor**, set the baud rate to **115200**

Expected output within a minute:

```
=== SolarPulse v3 ===
A: serial alive
B: NVS open
C: counters loaded, today 0.0 Wh in / 0.0 Wh out
D: LittleFS ok, 0 samples queued from offset 0
E: travel switch on GPIO 32 reads open (normal)
F: NimBLE init ok
...
[WiFi] up, IP 192.168.x.x
[web] http://solarpulse.local/
[BLE] connected
```

**Check:** you see `[BLE] connected` and no repeating `connect failed` lines, and `http://solarpulse.local/` (or the IP) opens the local dashboard in a browser on the same network.

---

## Phase 5 — Confirm data is arriving (10 min)

1. Go back to the Firebase console, open **Realtime Database**
2. Within 15 seconds a `live` node should appear
3. Expand it. You should see `v`, `i`, `soc`, `c1` through `c4`, and the rest
4. Cross-check two numbers against the JK app: `soc` and `v` should match

If `live` never appears but the serial monitor looks healthy, the database URL is wrong. Recheck for a stray `https://` or trailing slash.

**Check:** `live` exists and its numbers match the JK app.

---

## Phase 6 — Publish the dashboard (25 min)

1. Open `docs/app.js` in any text editor
2. Replace the placeholder block at the top with the firebaseConfig you saved in Phase 1:

```js
const FIREBASE_CONFIG = {
  apiKey: "AIza...",
  authDomain: "solar-pulse-xxxxx.firebaseapp.com",
  databaseURL: "https://solar-pulse-xxxxx-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "solar-pulse-xxxxx",
};
```

3. On GitHub, create a **new public repository** named `solar-pulse`. Public is required for GitHub Pages on a free account
4. Upload the whole project folder, including `.gitignore`, `README.md`, `SETUP.md`, `docs/`, and `firmware/`
5. Confirm `firmware/SolarPulse/config.h` did **not** upload. `.gitignore` blocks it, so your WiFi password stays off the internet. If you see it in the repo, delete it immediately and change your WiFi password
6. Go to **Settings > Pages**
7. Source: **Deploy from a branch**. Branch: `main`. Folder: `/docs`. Save
8. Wait 1 to 2 minutes, then refresh that page. A green box appears with your link:

```
https://<your-username>.github.io/solar-pulse/
```

**Check:** the page opens, the battery fills to your real SOC, and the four cell bars show live voltages.

---

## Phase 7 — Lock the database (10 min)

Do this within 30 days or writes will start failing.

1. Firebase console > **Realtime Database** > **Rules** tab
2. Replace everything with:

```json
{
  "rules": {
    ".read": true,
    ".write": "auth != null"
  }
}
```

3. Publish
4. Go to **Project settings > Service accounts > Database secrets**. Click **Show**, copy the secret
5. Paste it into `config.h`:

```c
#define FIREBASE_AUTH "your_long_secret_here"
```

6. Re-upload the firmware

Now anyone can read your dashboard, but only your ESP32 can write to it.

**Check:** after the re-upload, `live` in the console keeps updating. If it freezes, the secret was pasted wrong.

---

## Phase 8 — Test the offline queue (15 min)

This proves the WiFi-drop handling works before you rely on it.

1. Turn off your WiFi router, or move the ESP32 out of range
2. Watch the serial monitor. It should keep printing BMS data with no crash
3. Leave it for 10 minutes. The `q=` figure in the `[stat]` line should climb by one per minute
4. **Pull the power from the ESP32 while it is still offline, then plug it back in.** This is the reboot test — the queue lives on flash, so the count must come back, not reset
5. Turn the router back on
6. Within a minute the serial monitor should print lines like:

```
[queue] uploaded 20, 4 still waiting
[queue] uploaded 4, 0 still waiting
```

7. Open the dashboard. The footer note about buffered samples should disappear

**Check:** the day chart has **no gap** across the outage, including across the reboot, and `q=0` in the `[stat]` line.

---

## Phase 9 — Wire the relays and the travel switch (60 min)

Do this only after everything above passes. **Read `WIRING.md` first** — it covers the GPIO drive question, why the pins are 25/26/27 and not 4/5, and why the travel switch is not on GPIO 8.

1. Main breaker **off**. Confirm dead with the multimeter, not with faith
2. Wire the relay module's logic side: `IN1` to GPIO 25 (utility), `IN2` to GPIO 26 (solar), `IN3` to GPIO 27 (lights), `GND` to ESP32 GND
3. Feed the relay board's `VCC`/`JD-VCC` from a **separate 5 V supply**, grounds tied together. Not from the ESP32's 3V3 pin
4. Reset the ESP32 with the AC side still disconnected. Both source relays must be silent and open at boot
5. Watch one automatic changeover on the serial monitor:

```
[relay] solar -> utility : pack below evening floor
[relay] now on utility (pack below evening floor)
```

There must be an audible gap between the two clicks — that is the 800 ms dead time. Confirm it with a continuity meter across both contact sets before energising anything
6. Wire the travel switch between GPIO 32 and GND. Flip it and check the log prints `[travel] ON` / `[travel] OFF`
7. Only now wire the AC side per the diagram in `WIRING.md`

**Check:** at `http://solarpulse.local/`, the Source panel shows exactly one green relay, the Force solar / Force utility buttons work, and Auto hands control back.

---

## Phase 10 — Mount it permanently (20 min)

- Put the ESP32 and MP1584 in a small plastic box, away from the battery terminals
- Keep the box out of direct sun and away from the inverter's hot air outlet
- Check the WiFi signal reaches. On the dashboard the `rssi` value should be better than -75 dBm
- Tidy the wires so nothing can fall onto the busbars

---

## Phase 11 — Watch one full day

The first sunny day tells you whether the numbers are trustworthy.

Compare at sunset:

- Dashboard "Harvested today" against the SOC rise you saw
- Peak watts against what the inverter LCD showed at midday
- Cell delta at the end of day. It should still be under 0.01 V

After a week you will have enough daily bars to see the pattern, and you can start tuning the inverter program 12 value as discussed.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `[BLE] connect failed` repeating | JK app is connected on the phone | Force-close the app, then reset the ESP32 |
| `[BLE] connect failed` with app closed | Wrong MAC, or out of range | Recheck the MAC on the label, move the ESP32 closer |
| WiFi never connects | 5 GHz network | ESP32 needs a 2.4 GHz network. Split the bands on your router |
| WiFi connects then drops constantly | Weak signal or brownout | Check `rssi`, add the 470 uF capacitor if you skipped it |
| Serial shows BMS data but Firebase stays empty | Wrong `FIREBASE_HOST` | Remove `https://` and any trailing slash |
| Firebase worked, then stopped after weeks | Test-mode rules expired | Do Phase 7 |
| Dashboard page loads but stays blank | firebaseConfig not pasted in `app.js` | Recheck all four fields, especially `databaseURL` |
| Board will not boot after wiring the switch | Switch is on GPIO 6–11 | Those pins are the SPI flash. Move it to 32/33, see `WIRING.md` |
| A relay clicks on every reset | Relay input on a strapping pin, or wrong active level | Use 25/26/27, and check `RELAY_ACTIVE_LOW` matches your board |
| Both relays on together | `RELAY_ACTIVE_LOW` is inverted for your board | Flip it in `config.h` and reflash. Do not energise the AC side until fixed |
| `solarpulse.local` does not resolve | mDNS blocked on the network | Use the IP printed in the serial log |
| Local page 404s but `/api/live` works | `data/` was never uploaded | Run **Tools > ESP32 LittleFS Data Upload** |
| `[fb] low heap, push skipped` repeating | Too little RAM for the TLS handshake | Reduce `QUEUE_BATCH`, or drop the local web server if you do not need it |
| Monthly tab is empty | `/daily` only fills at midnight | Wait for one rollover, or check `/api/monthly` on the device |
| `[queue] full, oldest quarter dropped` | Offline for days | Expected at ~4 days offline. Raise `QUEUE_MAX_BYTES` if the flash has room |
| Dashboard says "Monitor offline" | ESP32 not pushing | Check the serial monitor, check the fuse |
| Cell voltages all read 0.000 | Frame not parsing | Confirm the BMS is a JK-B1A8S10P on hardware v11 or later |
| ESP32 reboots every few minutes | Power supply sagging | Confirm 5.0 V at VIN under load, add the capacitor |
| GitHub Pages shows 404 | Wrong folder selected | Settings > Pages, folder must be `/docs`, not `/root` |

---

## Optional upgrade: switch from Bluetooth to cable

Bluetooth allows only one client, so the phone app and the ESP32 cannot both connect. Running a cable fixes this permanently.

- Use the 4-pin JST SH socket on the BMS
- Wire GND, RX, TX only. Leave VBAT unconnected, since power already comes from the MP1584
- Your JK-B1A8S10P has that connector mounted on the top side of the board, so the pin order reads reversed. Measure with a multimeter before connecting
- The frame layout is identical, so only the transport changes in the firmware

Ask before starting this. The parser needs a small rewrite.

---

## References

[1] syssi, "esphome-jk-bms," GitHub. https://github.com/syssi/esphome-jk-bms
[2] Google, "Understand Realtime Database billing," Firebase Documentation.
[3] GitHub, "Configuring a publishing source for your GitHub Pages site," GitHub Docs.
