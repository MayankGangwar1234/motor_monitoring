# MQTT schema and REST API

## MQTT topics

| Topic | Producer → consumer | QoS | Retained | Purpose |
|---|---|---:|---:|---|
| `motor/G23/telemetry` | ESP32 → backend | 0 | No | Slow engineering-unit readings |
| `motor/G23/vibration/raw` | ESP32 → backend | 0 | No | Evenly sampled acceleration block |
| `motor/G23/vibration/features` | backend → tools | 0 | No | RMS/FFT summary |
| `motor/G23/status` | backend → clients | 1 | Yes | Current four-level assessment |
| `motor/G23/alert` | backend → clients | 1 | No | Abnormal/critical transition |
| `motor/G23/command` | dashboard/backend → ESP32 | 1 | No | Protected `STOP` or conditional `RESET` |
| `motor/G23/availability` | ESP32 LWT → clients | 1 | Yes | `online` / `offline` state |

Telemetry example:

```json
{
  "deviceId": "motor-G23",
  "experimentId": "healthy-load-01",
  "timestamp": "2026-08-27T14:22:31Z",
  "sequence": 1042,
  "temperatureC": 47.3,
  "currentA": 2.15,
  "voltageV": 229.4,
  "rpm": 1442,
  "vibrationRmsG": 0.084,
  "relayTripped": false
}
```

Raw vibration example (samples shortened):

```json
{
  "deviceId": "motor-G23",
  "timestamp": "2026-08-27T14:22:31Z",
  "sequence": 2084,
  "sampleRateHz": 512,
  "rpm": 1442,
  "samples": [0.0012, 0.0221, 0.0418, 0.0574]
}
```

## REST endpoints

| Method | Endpoint | Result |
|---|---|---|
| GET | `/health` | Backend, database and MQTT health |
| GET | `/api/motor/latest?deviceId=motor-G23` | Latest full measurement |
| GET | `/api/motor/history?from=...&to=...&limit=500` | Chronological trend data |
| GET | `/api/motor/alerts?limit=100` | Newest alerts first |
| GET | `/api/motor/status` | Compact current assessment |
| GET | `/api/motor/spectrum` | Latest FFT bins and features |
| GET | `/api/config/thresholds` | Active demonstration thresholds |
| POST | `/api/motor/command` | `{ "command": "STOP" }` or conditional `RESET` |
| POST | `/api/dev/telemetry` | Simulator/test ingestion when enabled |
| POST | `/api/dev/vibration` | Simulator/test block ingestion when enabled |

Dates use ISO-8601 UTC. Invalid dates, numeric ranges, device IDs, and vibration blocks return HTTP 400 or are rejected from MQTT. HTTP simulator ingestion should be disabled in deployed environments with `ALLOW_HTTP_INGEST=false`.

