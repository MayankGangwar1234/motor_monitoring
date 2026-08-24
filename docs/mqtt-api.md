# MQTT schema and REST API

## MQTT topics

| Topic | Producer -> consumer | QoS | Retained | Purpose |
|---|---|---:|---:|---|
| `motor/G23/telemetry` | ESP32 -> backend | 0 | No | 12 V DC motor engineering-unit readings and output state |
| `motor/G23/vibration/raw` | ESP32 -> backend | 0 | No | Evenly sampled acceleration block |
| `motor/G23/vibration/features` | backend -> tools | 0 | No | RMS/FFT summary |
| `motor/G23/status` | backend -> clients | 1 | Yes | Assessment plus motor/trip state |
| `motor/G23/alert` | backend -> clients | 1 | No | Abnormal/critical transition |
| `motor/G23/command` | dashboard/backend -> ESP32 | 1 | No | `START`, protected `STOP` or conditional `RESET` |
| `motor/G23/availability` | ESP32 LWT -> clients | 1 | Yes | `online` / `offline` state |

Telemetry example:

```json
{
  "deviceId": "motor-G23",
  "experimentId": "dc-12v-healthy-01",
  "timestamp": "2026-08-24T14:22:31Z",
  "sequence": 1042,
  "temperatureC": 34.2,
  "currentA": 0.18,
  "voltageV": 12.01,
  "powerW": 2.16,
  "rpm": 300,
  "vibrationRmsG": 0.031,
  "motorEnabled": true,
  "tripLatched": false,
  "tripReason": "none"
}
```

Raw vibration example (samples shortened):

```json
{
  "deviceId": "motor-G23",
  "timestamp": "2026-08-24T14:22:31Z",
  "sequence": 2084,
  "sampleRateHz": 256,
  "rpm": 300,
  "samples": [0.0012, 0.0121, 0.0218, 0.0274]
}
```

Command examples:

```json
{ "command": "START" }
{ "command": "STOP" }
{ "command": "RESET" }
```

`STOP` immediately drives GPIO23 LOW and latches the motor off. `RESET` is accepted only when required sensors and local readings are safe; it does not start the motor. `START` is a separate command and is rejected while a trip is latched.

## REST endpoints

| Method | Endpoint | Result |
|---|---|---|
| GET | `/health` | Backend, database and MQTT health |
| GET | `/api/motor/latest?deviceId=motor-G23` | Latest full measurement |
| GET | `/api/motor/history?from=...&to=...&limit=500` | Chronological trend data |
| GET | `/api/motor/alerts?limit=100` | Newest alerts first |
| GET | `/api/motor/status` | Assessment, motor output and trip state |
| GET | `/api/motor/spectrum` | Latest FFT bins and features |
| GET | `/api/config/thresholds` | Active demonstration thresholds |
| POST | `/api/motor/command` | `{ "command": "START" }`, `STOP` or `RESET` |
| POST | `/api/dev/telemetry` | Simulator/test ingestion when enabled |
| POST | `/api/dev/vibration` | Simulator/test block ingestion when enabled |

Dates use ISO-8601 UTC. Validation limits match the low-voltage model: bus voltage 0-26 V, current 0-5 A, temperature -55 to 125 C and RPM 0-5000. Disable HTTP simulator ingestion outside development with `ALLOW_HTTP_INGEST=false`.
