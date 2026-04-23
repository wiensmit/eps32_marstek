# Marstek ESP32

Authoritative controller for a **Marstek Venus E3** home battery, plus a Home Assistant add-on that feeds it real-time grid power.

The Marstek is treated as a dumb actuator. All policy — per-slot mode schedule, power caps, self-consumption tuning — lives on the ESP32. Every cycle the ESP32 re-asserts its desired state over Modbus RTU, so any register the Marstek silently drops is restored within one cycle.

## Repo layout

```
esp_marstek/       Arduino sketch for LilyGO T-CAN485 (ESP32 + RS-485)
p1_rest_api/       Home Assistant add-on — serves an HA power sensor as a Shelly Pro 3EM JSON endpoint
repository.yaml    HA add-on repository manifest (add this repo URL in HA → Add-on Store)
```

## `esp_marstek/` — ESP32 firmware

Hardware: **LilyGO T-CAN485** (ESP32 with onboard RS-485 transceiver), wired to the Marstek's RS-485 port.

What it does:
- Drives the Marstek over Modbus RTU (`SLAVE_ID=1`, 115200 8N1)
- Polls a Shelly-Pro-3EM-compatible JSON endpoint for instantaneous grid power
- Runs a 96-slot (15-minute) daily schedule, with modes: `off`, `max_charge`, `max_discharge`, `self_consumption`
- Self-consumption mode modulates charge/discharge power to hold grid import near a configurable offset, with smoothing and a deadband
- Persists config + schedule in NVS (`Preferences`)
- Exposes a small HTTP API on port 80 + Arduino OTA

### REST API (port 80)

| Method | Path        | Purpose                                                      |
|--------|-------------|--------------------------------------------------------------|
| GET    | `/`         | Device info                                                  |
| GET    | `/health`   | Uptime, RSSI, modbus errors, current slot & mode             |
| GET    | `/status`   | Full state: battery, AC, temps, grid, desired vs. cached regs |
| GET    | `/summary`  | Flat shape with the most-used values                         |
| GET    | `/meter`    | Last grid meter reading                                      |
| GET    | `/config`   | Current config                                               |
| POST   | `/config`   | Partial config update (JSON)                                 |
| GET    | `/schedule` | 96-slot mode array                                           |
| POST   | `/schedule` | Full array, `range`, or single `slot` update                 |

### Configuration (edit before flashing)

In `esp_marstek.ino`:
- `ssid` / `password` — WiFi credentials
- `staticIP` / `gateway` / `subnet` — static IP for reliable addressing from HA
- Timezone, NTP servers (default: Europe/Amsterdam)

After first boot, everything else is set at runtime via `POST /config` and `POST /schedule`.

### Build

Arduino IDE or `arduino-cli`, ESP32 core. Libraries: `ESPAsyncWebServer`, `ArduinoJson`, `ModbusRTU`, `ArduinoOTA`.

## `p1_rest_api/` — Home Assistant add-on

A tiny Python HTTP server that reads a Home Assistant sensor and re-emits it in the Shelly Pro 3EM RPC shape the ESP32 expects. This lets the ESP32 use an HA-attached P1 meter (DSMR / ESPHome / etc.) as if it were a native Shelly device.

### Endpoints

- `GET /rpc/EM.GetStatus` — returns `{ id, total_act_power, a_act_power, b_act_power, c_act_power }`
- `GET /rpc/Shelly.GetDeviceInfo` — minimal Shelly identity
- `GET /health` — cache age, fetch success/error counts

### Options

| Option              | Default                                  | Description                                                  |
|---------------------|------------------------------------------|--------------------------------------------------------------|
| `sensor_entity`     | `sensor.marstek_meter_p1_adjusted`       | HA entity whose numeric `state` is served as total power (W) |
| `listen_port`       | `8088`                                   | Port exposed to the LAN                                      |
| `cache_ttl_ms`      | `1000`                                   | Min interval between HA fetches                              |
| `stale_fallback_ms` | `30000`                                  | How long to keep serving the last good value on fetch error  |
| `log_requests`      | `false`                                  | Log every HTTP request                                       |

### Install

1. In Home Assistant → **Settings → Add-ons → Add-on Store → ⋮ → Repositories**, add:
   `https://github.com/wiensmit/eps32_marstek`
2. Install **P1 REST API**, set `sensor_entity` to your meter, start it.
3. On the ESP32, `POST /config` with `{"meter_ip": "<HA-host>"}` (port `8088` is the default the ESP32 talks to — adjust in `pollMeter()` if you change `listen_port`).

## Data flow

```
  HA P1 sensor ──► p1_rest_api add-on ──► ESP32 ──► Marstek Venus E3
                   (Shelly-shaped JSON)   (Modbus RTU, every cycle)
```

The ESP32 is the only authority. HA is just the P1 data source.
