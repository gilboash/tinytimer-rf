# tinytimer

<img src="web/breaking_carbon.jpg" alt="Breaking Carbon" height="70">

Analog FPV lap timing on an ESP32-C5: a gate that scans up to 8 pilots' 5.8 GHz
video channels, a LoRa link to a base station, and a base that is its own WiFi
hotspot serving the race page to any phone. No computer needed at the track.

```
 gate (XIAO ESP32-C5 + Wio-SX1262)               base (XIAO ESP32-S3 + Wio-SX1262)
  scans 8 pilots' video channels   --LoRa-->      ACKs passes, runs the race engine,   --WiFi AP-->  phones
  threshold per pilot -> pass + lap time          keeps the round's laps, serves the                (open network
  USB: live debug dashboard                       pages. USB: JSON debug stream                      tinytimer_breaking)
        \________ optional: a Mac running server/lapserver.py reads both over USB ________/
```

The gate only knows **pilot numbers (1–8), per-pilot thresholds and pass times**.
Everything else (names, rounds, laps, standings, call-outs) is above it.

## Using it

1. Power the gate and the base. Join the open WiFi network **`tinytimer_breaking`**
   on a phone and open **http://192.168.4.1** (tell the phone to stay connected
   although there is no internet).
2. **Race** page (`/`): tick the pilots, type their names, pick channels, press Start.
   Standings, lap tables per round (kept in the phone's browser, newest first),
   lap beeps and spoken call-outs (language + voice selectable) run on the phone.
3. **Base station** page (`/base`): LoRa link health (RSSI/SNR), received passes,
   per-pilot channel and thresholds with a *Gate has* column that shows whether the
   gate agrees, and calibration: *Measure noise*, *Measure peak* (a quad held in the
   gate), *Fill suggested thresholds*, *Send all*.
4. **Gate debug** page (`/gate`, only with the gate on a computer's USB): live signal
   per pilot against its thresholds, calibration from the live stream, test passes.

The network is open on purpose (a phone just joins). Anyone in range can reach the
controls; define `AP_PASS` (8+ characters) in `base/platformio.ini` build flags for WPA2.

## Race logic

Ported from the Chorus32 race page (*Chorus32-LapTimerBreakingCarbon*): pilot names edited
inline in the roster and standings; the **hole shot** (start → first gate crossing) is
timed but only counts as a lap if *First lap counts* is ticked; nothing scores after a
pilot takes the flag but later laps are still timed and shown struck through; position
is laps first, then time on track; a lead change, last lap and each finish are announced;
a table per round (HS, laps, best, best two consecutive, total) with the fastest lap
highlighted; target laps default 20, kept per device.

The base keeps the **raw laps of the current round** (lap 0 is the hole shot) and
streams them as `lap` events; the page derives everything else, so a phone that reloads
mid-race rebuilds the round from the base. Round history lives in the phone.
Lap times are exact: the difference of two stamps from the gate's own clock.

## Where settings live

In the **gate's flash**: pilot names (12 bytes each), channels, thresholds, the target laps, the
countdown, the hole-shot option and the minimum lap time, saved a couple of seconds after any change.
The base and the Mac server keep only a cache and **adopt what the gate reports** (status every 15 s,
a names/options sync when the gate says they changed, `GATE?` over USB). An edit is sent to the gate
(USB if attached, else over LoRa, retried until confirmed) and shielded for 20 s from a stale report.
A gate powered on its own therefore comes back as it was left, and every phone and the Mac show the
same thing. Still per phone: round history, callout language/voice, beep, screen-awake.

## Levels and thresholds

The gate reports each pilot's level as **SNR**: dB above that channel's own noise floor,
re-measured on every capture (`METRICS SNR`). The channels' absolute noise floors differ
by several dB, SNR doesn't, so one threshold shape fits every pilot and follows gain
changes. Level codes are 0.25 dB steps; the pages show and accept **dB**. Defaults are
enter 16 dB / exit 8 dB; calibrate for the real setup. Thresholds sent over LoRa are
queued, retransmitted until the gate confirms, and shown as *in sync* once it reports them.

## Known issues

* **Cross-talk between seats.** When a pilot flies, other seats rise too, mostly the seats
  two channels apart (74 MHz). Use *Measure peak* with a single VTX on: any other seat that
  reads high is leakage from that channel, and its threshold needs to sit above it.
* **The radio module's reset pin is not on the C5 header**, so the SX1262 only resets on a
  power cycle. If receive ever goes dead (gate `LORA?` shows `err:-705`, no ACKs), unplug and
  replug the gate.

## Layout

| Path | What |
|---|---|
| `common/lap_link.h` | LoRa wire format shared by gate and base |
| `gate/` | ESP-IDF/PlatformIO firmware for the C5 (GPL-3.0, derived from [Widscan58](https://github.com/CapnBry/widscan58)) |
| `gate/src/gate/` | pilot seats + pass detection (`gate.c`), USB events (`gate_io.c`), LoRa (`lora_link.cpp`), pins (`board_pins.h`) |
| `base/` | Arduino/PlatformIO firmware for the XIAO S3: LoRa (`main.cpp`), race engine + REST (`app.cpp`), hotspot + pages (`web.cpp`); `embed_web.py` packs `web/` into the firmware at build |
| `server/lapserver.py` | the same engine and API on a Mac, for the gate's USB debug path |
| `web/` | the pages (shared by the base and the Mac server) |
| `tests/lap_filter_test.c` | host test behind the EMA-filter default |
| `docs/upstream/` | Widscan58's own notes (RF details, calibration, gain) |

## Build and flash

PlatformIO ≥ 6.2 is needed by the pinned pioarduino platform (the system 6.1.x is too old):

```sh
python3 -m venv /tmp/pio && /tmp/pio/bin/pip install platformio
cd gate && /tmp/pio/bin/pio run -e esp32c5 -t upload     # C5 gate
cd base && /tmp/pio/bin/pio run -e base -t upload        # S3 base (rebuild after editing web/)
```

Debugging both over USB at once: `python3 server/lapserver.py` (PlatformIO's python has
pyserial). It finds the gate and the base by their `INFO` reply and serves the same pages on
http://localhost:8421; `/base` buttons always use LoRa, so that path can be exercised with the
gate on USB. Pins: gate radio on D3 (CS), D1 (BUSY), D0 (DIO1), D4 (RXEN), D2 (RST, not
wired through); see `gate/src/gate/board_pins.h`.

**Radio.** 868.1 MHz, SF7/BW250/CR4:5, sync 0x12, 14 dBm; EU 1 % duty limit (a status packet
every 15 s is ~0.4 %). Passes are retransmitted until ACKed (up to 12 tries, 3 per packet) and
deduplicated by `(gate, pilot, count, gate_ms)`.
