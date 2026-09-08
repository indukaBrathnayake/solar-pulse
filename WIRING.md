# Wiring guide — SolarPulse v5

Hardware: **two** 30 A relay modules (source changeover + switched load),
one travel-mode switch, one passive buzzer, one 0.96" I2C OLED, and an
optional PV meter.

> **Mains voltage kills.** The relay contacts carry 230 V. Do the AC side with
> the main breaker off, inside an enclosure, with a licensed electrician if you
> are not one. Nothing in this document is a substitute for that.

---

## 0. READ THIS FIRST — v5 uses ONE changeover relay for the source

v4 had two independent normally-open source relays (utility + solar) and
relied on firmware never to close both. **v5 replaces them with a single
SPDT changeover relay**, wired so the de-energised position feeds the
inverter:

```
   COM -> load bus LIVE
   NC  -> inverter / solar LIVE      (relay NOT triggered)
   NO  -> CEB / utility LIVE         (relay triggered)
```

**This is a safety improvement, not just a simplification.** A changeover
relay physically cannot bridge NC and NO — the armature must leave one
contact before it reaches the other. Break-before-make is now enforced by
the mechanism instead of by software.

Three consequences to design around:

1. **De-energised = solar, not isolated.** With the ESP unpowered or reset,
   the load bus sits on the inverter. That is the correct failsafe (a reset
   can never silently connect the grid), but the relay alone NEVER isolates
   the load bus. **Kill the upstream breaker before working on it.**

2. **`RELAY_DEAD_TIME_MS` is now a settle time, not an interlock.** It stops
   the source flapping and gives the inverter a moment to take the load. It
   is no longer what keeps the two sources apart.

3. **The neutrals stay commoned** (unchanged from v4). In a TN-C-S / PME
   installation the utility neutral is bonded to earth at the origin. If your
   inverter also bonds N-E internally in off-grid mode, commoning creates a
   parallel neutral-earth path. Check whether your inverter bonds N-E; if it
   does you need a 4-pole changeover that switches neutral too. **Get this
   confirmed by a licensed electrician before energising.**

`RELAY_SOLAR_PIN` is therefore `-1` in `config.h`. If you ever go back to two
independent NO relays, set it to a real pin and the old two-relay interlock
returns unchanged — `setSourceRelays()` still takes one source and cannot
express "both on".

## 1. Does one GPIO have enough drive for a relay?

Yes — with the usual relay *module*, and no if you try to drive a bare relay.

A bare relay coil (SRD-05VDC-SL-C and friends) wants 70–90 mA at 5 V. An ESP32
pin can source about 40 mA absolute maximum and only 3.3 V. Connecting one
directly will not pull the armature in and will damage the pin.

The blue relay boards everyone uses already solve this. Each channel is:

```
GPIO ──220Ω──►|── opto LED ──┐            (input side, ~3 mA)
                              └── phototransistor ── NPN ── coil ── 5 V
```

The pin only has to light an optocoupler LED: **about 3 mA at 3.3 V**, well
inside spec. The coil current comes from the 5 V rail, never from the ESP.

The number of *poles* changes nothing on the control side — one coil, one
signal, however many contact sets it throws. v5 uses a changeover (SPDT)
contact set on LIVE for the source, and a plain NO contact for the switched
load:

**One GPIO per relay. Two relays total: source changeover + switched load.**

Two conditions:

- **Power the relay board's `VCC`/`JD-VCC` from a separate 5 V supply**, not
  from the ESP32's 3V3 pin and not from a laptop USB port. Tie the grounds
  together. If the board has a `JD-VCC` jumper, remove it and feed `JD-VCC`
  from the 5 V supply — that keeps the coil's switching noise off the ESP
  ground.
- **Check the board's active level.** Most are active LOW (relay pulls in when
  the input is at 0 V). `config.h` has `RELAY_ACTIVE_LOW 1` for those; set it
  to `0` for an active-high board. Getting this wrong inverts every decision
  the firmware makes.

---

## 2. Pin map

| Function | Default pin | Notes |
|---|---|---|
| **Source changeover relay** | **`GPIO 25`** | `RELAY_UTILITY_PIN`. Coil OFF = solar (NC), ON = CEB (NO) |
| Separate solar relay | *(none)* | `RELAY_SOLAR_PIN = -1` — see §0 |
| Switched load relay | `GPIO 27` | `RELAY_LOAD_PIN`, set `-1` if unused |
| Travel-mode switch | `GPIO 32` | `TRAVEL_SWITCH_PIN`, see below |
| Passive buzzer | `GPIO 33` | `BUZZER_PIN`, LEDC ch 0, see §5 |
| **OLED SDA** | **`GPIO 21`** | `OLED_SDA_PIN`, I2C, see §5b |
| **OLED SCL** | **`GPIO 22`** | `OLED_SCL_PIN`, I2C, see §5b |
| PV voltage sense (optional) | `GPIO 34` | ADC1, input-only |
| PV current sense (optional) | `GPIO 35` | ADC1, input-only |

GPIO 26 is now free (it was the old solar relay).

Nothing on the BLE/BMS side changed. The BMS is Bluetooth only — no wires.

### Why not GPIO 4 and 5 as originally suggested

Both work electrically, but on the classic ESP32:

- **GPIO 5** is a strapping pin with an internal pull-up. During reset and for
  the first few ms of boot it floats HIGH. On an active-high relay board that
  is a momentary contact closure on every reset — on the utility relay that
  means the mains briefly hits the load bus.
- **GPIO 4** is fine, but is also the default touch/ADC2 pin on many boards and
  ADC2 is unusable while WiFi is on.

`25`, `26` and `27` are plain outputs with no boot-time behaviour, so they are
the defaults. Change them in `config.h` if your build needs different ones.

### Why not GPIO 8 for the travel switch

**On a classic ESP32 (the "ESP32 Dev Module" this project builds for),
GPIO 6–11 are bonded to the SPI flash chip.** Pulling one of them to ground
stops the flash from being read and the board will not boot at all. The
specification asked for GPIO 8; on this chip that pin does not exist as a
usable input.

`config.h` therefore selects:

- **GPIO 8** when compiled for ESP32-S2, S3 or C3, where GPIO 8 is a normal pin
- **GPIO 32** on the classic ESP32

and there is a `#error` guard so that forcing 6–11 fails at compile time
instead of bricking the boot. If you want the switch on a specific pin, any of
32, 33, 34, 35, 36, 39 will do; 34–39 are input-only but have **no internal
pull-up**, so those need an external 10 kΩ resistor to 3V3.

---

## 3. Relay wiring, AC side

One SPDT changeover on the LIVE conductor. The neutrals are commoned
(see §0):

```
                 ┌─────── SOURCE CHANGEOVER RELAY (GPIO 25) ───────┐
   Inverter L ───┤ NC                                              │
                 │        COM ├──────────────► LOAD BUS  Live      │
   CEB / grid L ─┤ NO                                              │
                 └─────────────────────────────────────────────────┘
                    coil OFF → COM-NC → SOLAR      (failsafe / reset)
                    coil ON  → COM-NO → CEB

   CEB Neutral ──────┬───────────────────────────── LOAD BUS Neutral
   Inverter Neutral ─┘        (commoned, never switched — see §0.3)
```

- **Inverter on NC, grid on NO.** Getting these the wrong way round means a
  reset or a power cut connects the grid instead of the inverter. Verify with
  a meter, coil unpowered, before energising anything.
- With the ESP32 unpowered the load bus is on the **inverter** and still has
  neutral. It is never isolated by this relay. Kill the breaker.
- A changeover cannot bridge NC and NO, so utility and inverter can never be
  paralleled by a firmware fault. This is why v5 is safer than v4's two
  independent relays. A mechanically interlocked contactor pair is an equally
  good choice if you prefer separate devices.
- Earth is never switched. Bond it straight through.
- Size the contacts for the real load. The 30 A on the module is a resistive
  rating at best; derate hard for motors and pumps, or use the module to
  drive a proper contactor.

### Switched load relay

```
   LOAD BUS Live ───┤ COM ── NO ├─── load circuit Live
   LOAD BUS Neutral ──────────────── load circuit Neutral
```

Feed it from the load bus, not from a source directly, so the circuit follows
whichever source is active.

This relay is also the **battery protection cutoff**: the firmware opens it at
`SOC_LOAD_CUTOFF` (35%) regardless of the travel schedule or the web toggle,
and closes it again at 37%. Put the discretionary loads on it — lighting,
entertainment — not the fridge or anything that must never lose power.

---

## 4. Travel-mode switch

A plain SPST toggle, no power of its own:

```
   GPIO 32 ──┬── switch ── GND
             │
             └── internal pull-up (enabled in firmware)
```

- Closed (shorted to GND) = **travel mode on**
- Open = normal operation

`pinMode(TRAVEL_SWITCH_PIN, INPUT_PULLUP)` is set in `setup()`, so no external
resistor is needed on GPIO 32. Add a 100 nF cap across the switch if it runs
more than a metre or two; the firmware also debounces it for 500 ms.

If you prefer switch-to-3V3 wiring, set `TRAVEL_SWITCH_ACTIVE_LOW 0` in
`config.h` and add a 10 kΩ pull-down.

---

## 5. Passive buzzer (GPIO 33)

**Passive**, not active. An active buzzer has its own oscillator and just needs
DC; a passive one is a piezo element that needs a square wave. The firmware
drives it with the ESP32 LEDC peripheral, so it can play actual notes and costs
no CPU time. An active buzzer on this pin will squeal at one pitch or not at
all.

A small piezo (under ~20 mA) can go straight on the pin:

```
   GPIO 33 ──────────┬────── buzzer + 
                     │
                    ═╧═ buzzer
                     │
   GND ──────────────┴────── buzzer −
```

Anything louder — a magnetic transducer, or a piezo with a driver board —
exceeds what a GPIO should source. Use a small NPN:

```
   GPIO 33 ──1kΩ──── B
                      \
                       NPN (2N3904 / BC547)
                      /  C ────── buzzer − ,  buzzer + ── +5 V
                     E
                     └── GND                 (add a 1N4148 across the
                                              buzzer if it is magnetic)
```

Behaviour, from `config.h` section 8b:

| Pack SoC | What happens |
|---|---|
| above 38% | silent |
| **38% or below** | rising three-note chime every 20 s |
| **35% or below** | load relay **opens**, urgent triple beep every 5 s |
| back to 37% | load reconnects |
| back to 40% | buzzer stops |

The two recovery thresholds are `SOC_ALARM_HYST` (2 points) above the trigger,
so a pack hovering on the line cannot chatter the relay or stutter the tone.
Set `BUZZER_PIN` to `-1` to build without a buzzer.

---

## 5b. 0.96" OLED (SSD1306, I2C)

### What it shows (v8, layout v2)

Driven by **U8g2 in full-buffer mode**. Layout v2 removed the chrome:
there is **no outer border, no box around the power row and no
vertical divider** beside the arrow lane. What is left is one short
44 px rule.

```
  +--------------------------------------+
  |                            |         |  y 0
  |                            |         |
  |          8 8 %             |  ARROW  |  TOP AREA
  |                            |  LANE   |  x 0..109
  |                            |         |  y 0..49
  |____________                |         |  rule y=51, 44 px wide
  |   124 W                    |         |  POWER ROW y 52..63
  +--------------------------------------+  y 63
```

The lane (x 112..127) is **reserved, not drawn** — nothing is ever
written into it and no line marks its edge. Top-area content is
centred inside 110 px, not inside the full 128, so it does not sit
visibly right of centre under the arrows.

| Screen | Time | Top-area content |
|---|---|---|
| Battery | 10 s | the percentage, `logisoso38_tn` + `profont17_mr` `%`, both on baseline 44 |
| Source | 2 s | one word, `fub20_tf`: `CEB` or `Pack` |
| Weather | 4 s | 28×28 icon + condition, **06:00–13:59** |
| ETA | 3 s | time to empty, or clock time of full |
| Tomorrow | 4 s | 24×24 icon + outlook, **18:00–23:59** |

Rotation and all five timings are unchanged from v8. SoC appears
twice per cycle in every window.

### The SoC screen

Digits and the percent sign are measured at runtime and centred as
**one block**, both on baseline 44. At 38 px ascent that puts the
glyph top at row 6 and leaves 7 px above the rule.

`100` at `logisoso38_tn` measures 72 + 3 + 9 = **84 px inside a
110 px area**, so it fits comfortably; the `logisoso32_tn` fallback
is a guard that triggers only if the area is ever narrowed, not a
routine path. A stale BMS shows `-- %`, never a number.

### The power row

**Wattage only** — no `LOAD`, no `PV`, no `IDLE`, no label of any
kind. The arrow already carries the direction, so the word was the
same fact twice and it cost the number its room. Left aligned at
x = 2, baseline 62, `profont12_tf`, whole numbers, **rounded not
truncated** (184.73 W reads `185 W`). Under `OLED_IDLE_W` it reads
`0 W`; a stale BMS reads `-- W`.

### The arrow lane — unchanged

Two arrows 32 px apart, 3 px per frame every 50 ms, wrapping modulo
64, travelling the full 64 px. **Up** while drawing from the pack,
**down** while harvesting. Reversal resets the phase once and flips
the glyph.

### Fonts

| Use | Font |
|---|---|
| SoC digits | `u8g2_font_logisoso38_tn` (fallback `logisoso32_tn`) |
| Percent | `u8g2_font_profont17_mr` |
| Source / ETA line 1 | `u8g2_font_fub20_tf` |
| Weather, ETA line 2, power row | `u8g2_font_profont12_tf` |
| `TOMORROW` label, long outlook words | `u8g2_font_profont10_tf` |

---

## 5c. Lighting circuit modes

The lights are the **switched load relay** (`RELAY_LOAD_PIN`), and
`travelTask()` remains its only writer — the modes below are inputs
to that one function, not a second controller for the same GPIO.

Precedence, highest first:

| # | Rule | Source |
|---|---|---|
| 1 | `loadCutoff` — battery protection at `SOC_LOAD_CUTOFF` (35%) | existing |
| 2 | lights low-battery lockout at `LIGHTS_CUTOFF_SOC` (25%), latched, re-arms +5% | new |
| 3 | travel mode switch and its schedule | existing |
| 4 | `ON` / `OFF` from the dashboard | new |
| 5 | `AUTO` — sunset to `LIGHTS_OFF_HOUR` (23:00) | new |

**AUTO** takes sunset from the daily forecast section 8k already
fetches — one extra field on a response that was being parsed
anyway, no new request and no new service. With no cached sunset it
falls back to `LIGHTS_FALLBACK_ON_HOUR` (18:30), so the lights still
work with the internet down.

> **Note.** On this build the lights share the protected load relay,
> and that relay already opens at 35%. So the 25% lights cutoff is a
> **backstop** — it only becomes the operative rule if
> `SOC_LOAD_CUTOFF` is lowered below it, or if the lights are moved
> to their own relay. Raise `LIGHTS_CUTOFF_SOC` above 35% to shed
> lighting *before* the rest of the load circuit.

The ON/OFF/AUTO buttons live on the **on-device** dashboard, which is
the only page with a route to the relay. The cloud dashboard reads
Firebase and cannot reach the ESP, so it reports the mode, the
reason and the daily runtime instead.

```
   ESP32 3V3 ──── VCC
   ESP32 GND ──── GND
   GPIO 21   ──── SDA
   GPIO 22   ──── SCL
```

- **Address:** `0x3C` (most 0.96" panels). A few are `0x3D` — change
  `OLED_ADDR` in `config.h` if `begin()` reports "not found" at boot.
- **Pull-ups:** nearly every SSD1306 breakout has 4.7 kΩ pull-ups on board,
  so you do not need to add any. If you daisy-chain several I2C devices,
  remove the duplicates.
- **Why 21/22:** they are the ESP32's default I2C pair and the only fully
  unencumbered pins left here — not strapping pins, not bonded to the SPI
  flash, not ADC2 (which WiFi disables), and nothing else in this project
  uses them.
- **3.3 V only.** Do not feed the module 5 V; the SSD1306 logic is 3.3 V.
- Keep the wires short (< 20 cm) at the 400 kHz default, or drop
  `OLED_I2C_HZ` to 100000.

**It is entirely optional.** If the panel is missing, unplugged, or fails to
initialise, the firmware logs it once, sets `oledOk = false`, and every later
display call becomes a no-op. The controller, relays, BLE link and watchdog
are unaffected. Set `OLED_ENABLE 0` to compile it out completely.

The display also never shows a stale number: if the BMS link goes quiet it
prints `--%` and "BMS link lost" rather than a value that is no longer true.

---

## 6. Optional PV meter

The rig has no PV-side sensor today, so "harvested" is measured at the battery:
loads fed straight from the array during the day never pass the BMS shunt and
are not counted. To fix it properly, wire a divider and a current sensor into
ADC1 and set `PV_ADC_ENABLE 1`:

```
   PV +  ──┬── R1 100k ──┬── GPIO 34
           │             │
           │            R2 10k
           │             │
   PV −  ──┴─────────────┴── GND (common with ESP32 GND)
```

`PV_VOLT_DIVIDER` must equal `(R1+R2)/R2` — 11.0 for the values above, giving
about 36 V full scale. Add a 3.3 V zener across R2 as protection.

For current, an ACS712-30A in the array positive lead, output to GPIO 34's
sibling GPIO 35, with `PV_CURR_MV_PER_A 66.0`. A hall sensor is simpler than a
shunt here because it needs no isolation.

Use **ADC1 pins only** (32–39). ADC2 stops working the moment WiFi is on.

---

## 7. Powering the ESP32

Power it from the **battery** through a small buck converter, not from the
inverter's AC output. If it runs on AC, a blackout silences the monitor exactly
when you most want to see what the system did — and the relay controller stops
being able to switch to utility.

---

## 8. Bring-up checklist

Do these in order, **with the AC side disconnected** until step 6.

1. Flash the firmware with `RELAY_ACTIVE_LOW` set for your board. Confirm the
   serial log reaches `I: setup complete` with no `#error`.
2. **Verify the changeover direction with a meter, coil unpowered.** COM must
   read continuous to NC (inverter side). If it reads continuous to NO, your
   NC/NO are swapped and a reset would connect the grid — rewire before going
   further. This is the single most important check in this document.
3. Connect only the relay board's logic and its separate 5 V. Force a source
   change from the local UI (Force CEB / Force solar) and listen: one clean
   click per change, with the `RELAY_DEAD_TIME_MS` pause between states.
4. Check the OLED shows a live SoC. Unplug it while running — the controller
   must carry on unaffected and the serial log must not report a watchdog.
5. Toggle the travel switch and confirm `[travel] ON` / `[travel] OFF`.
6. Only now wire the AC side, per §3.
7. With mains live, watch one real handover in the serial log:
   `[ceb] ON  soc=..` then later `[ceb] OFF soc=.. ...`. Confirm the house
   does not lose power for longer than the dead time.

To exercise the CEB logic without waiting for a flat battery, temporarily
raise `SOC_CEB_ON` in `config.h` to just under the current SoC, reflash, and
watch it engage — then put it back to 18.
