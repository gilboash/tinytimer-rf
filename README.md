# ESP32-C5 RF lap timer

Analog FPV lap timing with an ESP32-C5 as the gate receiver, LoRa to a base
station, and all the race logic on a server (the Mac for now) with a web UI.

```
 gate node (XIAO ESP32-C5 + Wio-SX1262)            base (XIAO ESP32-S3 + Wio-SX1262)
  scans up to 8 pilots' video channels   --LoRa-->   ACKs passes, forwards JSON   --USB-->  server  --HTTP--> browser
  per-pilot threshold -> pass + lap time                                                    names, laps, best,
  USB: debug dashboard, same events                                                         standings, call-outs
```

The gate only knows **pilot numbers (1–8), per-pilot thresholds and timing**.
Pilot names, race state, lap aggregation and spoken call-outs are server-side.

## Status

| Part | State |
|---|---|
| `gate/` firmware (fork of Widscan58 + gate/LoRa layer) | **builds** for ESP32-C5; **not yet run on hardware** (no C5 yet) |
| `base/` firmware | **builds** for the XIAO S3; not yet flashed |
| `server/` + `web/` | working against the built-in simulator (`--sim`) |
| LoRa gate ↔ base | written, untested on air |

The S3 currently plugged in still runs the old power-meter walk-test gateway;
nothing has been flashed to it.

## Run the dashboard now (no hardware)

```sh
python3 server/lapserver.py --sim        # http://localhost:8421
```

* `/` race page: start/stop, standings, lap chart, pilot setup, voice call-outs
* `/gate` gate debug: live per-pilot signal chart (dBFS) with thresholds,
  noise/peak calibration, test passes, LoRa health, raw command terminal

With real hardware use PlatformIO's python (it has pyserial) and no flag; the
server probes `/dev/cu.usbmodem*` with `INFO` and picks out the gate and the
base by their reply (it never toggles DTR/RTS, so it doesn't reset boards).

## Layout

| Path | What |
|---|---|
| `common/lap_link.h` | LoRa wire format shared by gate and base |
| `gate/` | ESP-IDF/PlatformIO firmware for the C5 (GPL-3.0, derived from [Widscan58](https://github.com/CapnBry/widscan58)) |
| `gate/src/gate/` | pilot seats + pass detection (`gate.c`), USB events (`gate_io.c`), LoRa (`lora_link.cpp`), pins (`board_pins.h`) |
| `base/` | Arduino/PlatformIO firmware for the XIAO S3 base |
| `server/lapserver.py` | race engine, serial plumbing, SSE + REST |
| `web/` | race and gate-debug pages |
| `tests/lap_filter_test.c` | host test behind the EMA-filter default |
| `docs/upstream/` | Widscan58's own notes (RF details, calibration, gain) |

## Build

PlatformIO ≥ 6.2 is required by the pinned pioarduino platform; the system one
(6.1.x) is too old. A throwaway venv works:

```sh
python3 -m venv /tmp/pio && /tmp/pio/bin/pip install platformio
cd gate && /tmp/pio/bin/pio run -e esp32c5 -t upload     # C5 gate
cd base && /tmp/pio/bin/pio run -e base -t upload        # S3 base
```

C5 flashing quirks (BOOT+RESET recovery, rev 1.0 ROM bug) are in
`docs/upstream/widscan58-CLAUDE.md`.

## Design notes

**Timing.** Passes are stamped with the gate's own ms clock; the lap time is
the difference of two gate stamps, so radio latency never touches it. Each PASS
packet carries the gate clock at transmit, which lets the server place the pass
on its own timeline (`arrival − airtime − age`) without a sync exchange. Passes
are retransmitted until ACKed (up to 12 tries), 3 per packet.

**Detector.** Widscan58's `lap.c`, with enter/exit thresholds in 0.25 dB level
codes (dBFS = code/4 − 63.75). Default filter is EMA, not RotorHazard's median:
the median removes pulses under ~130 ms, which a fast quad can be
(`tests/lap_filter_test.c`). `GATE MINLAP` (default 3 s) suppresses double hits.

**Gate USB commands** (also in the dashboard's terminal): `PILOT n MHz|0`,
`THRESH n enter exit`, `GATE?`, `GATE RESET|SAVE|ID n|MINLAP ms`, `STREAM hz`,
`SIMPASS n`, `LORA?`, `PROTOCOL MONITOR` (switch to Widscan58's full scanner
protocol for RF debugging; `PROTOCOL GATE` returns).

**Radio.** 868.1 MHz, SF7/BW250/CR4:5, sync 0x12, 14 dBm; EU 1 % duty limit
applies (status packet every 15 s is ~0.4 %).

## Things to verify when the C5 arrives

1. **Wio-SX1262 pins on the C5** (`gate/src/gate/board_pins.h`): derived from
   Seeed's XIAO C5 pin table plus the pin-header module's control pins from the
   power-meter project. `LORA?` reports `radio NOT up` + the RadioLib error if wrong.
2. **XIAO C5 antenna switch**: Widscan58 has a `GPIO <n> LOW|HIGH` + `GPIO SAVE`
   command for boards with an antenna-select pin; the XIAO needs the U.FL path
   selected for real range.
3. **Gain/thresholds**: default gain is manual 45 (Widscan58's value for a 25 mW
   VTX at 0.9 m). Use the Gate-debug calibration buttons per pilot.
4. **Scan rate with 8 pilots**: Widscan58 reports ~510 updates/s per seat for
   8 channels; check `TIMING?` after adding the LoRa task (single core).
