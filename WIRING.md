# Wiring guide — SolarPulse v3

Everything new in v3 that touches hardware: two source relays, one lighting
relay, one travel-mode switch, and an optional PV meter.

> **Mains voltage kills.** The relay contacts carry 230 V. Do the AC side with
> the main breaker off, inside an enclosure, with a licensed electrician if you
> are not one. Nothing in this document is a substitute for that.

---

## 1. Does one GPIO have enough drive for a two-pole relay?

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

The number of *poles* on the relay changes nothing electrically. A DPDT relay
has one coil throwing two sets of contacts. Live and Neutral are switched by
the same armature, so:

**One GPIO per relay is correct. Do not use four pins for two relays.**

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
| Utility (CEB) relay | `GPIO 25` | `RELAY_UTILITY_PIN` |
| Solar / inverter relay | `GPIO 26` | `RELAY_SOLAR_PIN` |
| Lighting relay (travel mode) | `GPIO 27` | `RELAY_LOAD_PIN`, set `-1` if unused |
| Travel-mode switch | `GPIO 32` | `TRAVEL_SWITCH_PIN`, see below |
| PV voltage sense (optional) | `GPIO 34` | ADC1, input-only |
| PV current sense (optional) | `GPIO 35` | ADC1, input-only |

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

Only one source may be connected to the load bus at a time. The firmware
guarantees this in software (both relays open for `RELAY_DEAD_TIME_MS` on every
change), but wire it so a software fault cannot cause a backfeed either:

```
                        ┌──────── UTILITY RELAY (GPIO 25) ────────┐
   CEB Live  ───────────┤ COM ──────────────── NO ├───────────────┼──┐
   CEB Neutral ─────────┤ COM ──────────────── NO ├───────────────┼─┐│
                        └─────────────────────────────────────────┘ ││
                                                                    ││
                        ┌──────── SOLAR RELAY (GPIO 26) ──────────┐ ││
   Inverter Live ───────┤ COM ──────────────── NO ├───────────────┼─┼┤
   Inverter Neutral ────┤ COM ──────────────── NO ├───────────────┼─┤│
                        └─────────────────────────────────────────┘ ││
                                                                    ││
                                        LOAD BUS  Live  ────────────┘│
                                        LOAD BUS  Neutral ───────────┘
```

- Use the **NO** (normally open) contacts on both. With the ESP32 unpowered,
  both relays are open and the load bus is dead — the safe state.
- **Fit a mechanical interlock** (two interlocked contactors, or a changeover
  contactor) if you can. Two independent relays rely on firmware for
  exclusivity; interlocked contactors rely on physics.
- Earth is never switched. Bond it straight through.
- Size the contacts for the real load. The 10 A printed on a cheap blue module
  is a resistive rating at best; derate hard for motors and pumps, or use the
  module to drive a proper contactor.

### Lighting relay (travel mode)

```
   LOAD BUS Live ───┤ COM ── NO ├─── lighting circuit Live
   LOAD BUS Neutral ──────────────── lighting circuit Neutral
```

Feed it from the load bus, not from a source directly, so the lights follow
whichever source is active.

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

## 5. Optional PV meter

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

## 6. Powering the ESP32

Power it from the **battery** through a small buck converter, not from the
inverter's AC output. If it runs on AC, a blackout silences the monitor exactly
when you most want to see what the system did — and the relay controller stops
being able to switch to utility.

---

## 7. Bring-up checklist

1. Flash the firmware with `RELAY_ACTIVE_LOW` set for your board, **relay board
   not connected to mains**. Confirm from the serial log that both relays read
   as open at boot.
2. Connect only the relay board's logic and 5 V. Watch it click through a
   changeover — you should hear one relay drop, a pause, then the other pull in.
   Never both at once.
3. Measure continuity across both sets of contacts to confirm the dead time is
   real before anything is energised.
4. Only then wire the AC side.
5. Toggle the travel switch and check the serial log prints
   `[travel] ON` / `[travel] OFF`.
