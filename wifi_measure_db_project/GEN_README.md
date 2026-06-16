# Raspberry Pi Pico 2 W Telemetry Pipeline

A complete prototype for sending buffered sensor data from a **Raspberry Pi Pico 2 W** to a **C endpoint server** over Wi‑Fi using **HTTP POST + JSON batches**, with **NTP-based UTC timestamps** on the device and **SQLite storage** on the server.

## Features

### Pico client

- Wi‑Fi connection with automatic reconnect
- Local RAM ring buffer for measurements
- Batch upload of buffered measurements
- NTP synchronization for `timestamp_utc`
- Per-measurement fields:
  - `seq`
  - `timestamp_utc`
  - `time_valid`
  - `uptime_s`
  - `temperature`
  - `humidity`
  - `voltage`
- Retry behavior that avoids losing buffered samples if the server is unavailable

### Endpoint server

- `POST /measurements` endpoint in C
- API key validation using `X-API-Key`
- SQLite persistence
- Batch-level storage in `batches`
- Per-measurement storage in `measurements`
- Server-side receive timestamp `server_received_at`
- Device-side timestamp `device_timestamp_utc`
- Deduplication using `UNIQUE(device_id, seq)` + `INSERT OR IGNORE`
- Raw batch JSON retained for troubleshooting

---

## End-to-End Architecture

```text
Raspberry Pi Pico 2 W
   |
   | Wi‑Fi + HTTP POST + JSON batches + X-API-Key
   v
C endpoint server on Linux / Raspberry Pi
   |
   v
SQLite database
```

---

## Repository Layout

```text
.
├── README.md
├── pico-main.c                # Pico client code (batch upload + NTP + buffer)
├── CMakeLists.txt             # Pico build file
├── lwipopts.h                 # lwIP options for Pico build
└── endpoint_server.c          # C endpoint server with SQLite + cJSON
```

> Rename `pico-main.c` as needed in your build, for example `main.c`.

---

## JSON Format

The Pico sends JSON in this format:

```json
{
  "device_id": "pico-01",
  "queued_total": 6,
  "dropped_total": 1,
  "time_valid": true,
  "measurements": [
    {
      "seq": 101,
      "timestamp_utc": 1781613432,
      "time_valid": true,
      "uptime_s": 500,
      "temperature": 22.5,
      "humidity": 45.0,
      "voltage": 3.28
    },
    {
      "seq": 102,
      "timestamp_utc": 1781613442,
      "time_valid": true,
      "uptime_s": 510,
      "temperature": 22.6,
      "humidity": 45.1,
      "voltage": 3.27
    }
  ]
}
```

### Meaning of timestamps

- `timestamp_utc`: device-side UTC timestamp generated on the Pico
- `server_received_at`: server-side Unix timestamp when the batch was received

This makes it easy to debug:

- NTP sync issues
- network delays
- buffered backlog on the Pico
- batch retry timing

---

## SQLite Schema

### Table: `batches`

One row per incoming HTTP request.

```sql
CREATE TABLE IF NOT EXISTS batches (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    server_received_at INTEGER NOT NULL,
    remote_ip TEXT,
    device_id TEXT,
    queued_total INTEGER NOT NULL DEFAULT 0,
    dropped_total INTEGER NOT NULL DEFAULT 0,
    time_valid INTEGER NOT NULL DEFAULT 0,
    raw_json TEXT NOT NULL
);
```

### Table: `measurements`

One row per measurement in the batch.

```sql
CREATE TABLE IF NOT EXISTS measurements (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    batch_id INTEGER NOT NULL,
    server_received_at INTEGER NOT NULL,
    device_id TEXT NOT NULL,
    seq INTEGER NOT NULL,
    device_timestamp_utc INTEGER NOT NULL DEFAULT 0,
    time_valid INTEGER NOT NULL DEFAULT 0,
    uptime_s INTEGER NOT NULL DEFAULT 0,
    temperature REAL,
    humidity REAL,
    voltage REAL,
    UNIQUE(device_id, seq),
    FOREIGN KEY(batch_id) REFERENCES batches(id)
);
```

### Why the unique constraint matters

The Pico may retry sending a batch after a timeout or connectivity failure. The server therefore uses:

```sql
UNIQUE(device_id, seq)
```

combined with:

```sql
INSERT OR IGNORE
```

This prevents duplicate measurements from being stored twice.

---

## Pico Build

### Requirements

- Raspberry Pi Pico SDK
- Pico 2 W or Pico W
- Working Wi‑Fi credentials

### Example build for Pico 2 W

```bash
mkdir build
cd build
cmake -DPICO_BOARD=pico2_w \
      -DWIFI_SSID="YourWifi" \
      -DWIFI_PASSWORD="YourWifiPassword" \
      -DAPI_KEY="my-secret-key" \
      ..
make -j4
```

### Example build for Pico W

```bash
mkdir build
cd build
cmake -DPICO_BOARD=pico_w \
      -DWIFI_SSID="YourWifi" \
      -DWIFI_PASSWORD="YourWifiPassword" \
      -DAPI_KEY="my-secret-key" \
      ..
make -j4
```

### Pico compile-time settings

Typical configuration inside the Pico source:

```c
#define DEVICE_ID "pico-01"
#define SERVER_HOST "192.168.1.50"
#define SERVER_PORT 5000
#define SERVER_PATH "/measurements"
```

### Useful runtime parameters in the Pico code

```c
#define SEND_INTERVAL_MS                 10000
#define MEASUREMENT_QUEUE_CAPACITY          32
#define MAX_BATCH_SIZE                       8
#define MAX_BATCH_REQUESTS_PER_CYCLE         4
#define HTTP_TIMEOUT_MS                  10000
#define NTP_SERVER                    "pool.ntp.org"
```

---

## Endpoint Server Build

### Requirements

On Debian / Ubuntu / Raspberry Pi OS:

```bash
sudo apt update
sudo apt install build-essential libsqlite3-dev sqlite3 libcjson-dev
```

### Compile

```bash
gcc endpoint_server.c -o endpoint_server -lsqlite3 -lcjson
```

### Run

```bash
PICO_API_KEY="my-secret-key" ./endpoint_server
```

The server listens on:

```text
http://<server-ip>:5000/measurements
```

Example:

```text
http://192.168.1.50:5000/measurements
```

---

## Test the Server with `curl`

Before testing the Pico, verify the endpoint manually:

```bash
curl -X POST http://192.168.1.50:5000/measurements \
  -H "Content-Type: application/json" \
  -H "X-API-Key: my-secret-key" \
  -d '{
    "device_id":"pico-01",
    "queued_total":2,
    "dropped_total":0,
    "time_valid":true,
    "measurements":[
      {
        "seq":101,
        "timestamp_utc":1781613432,
        "time_valid":true,
        "uptime_s":500,
        "temperature":22.5,
        "humidity":45.0,
        "voltage":3.28
      },
      {
        "seq":102,
        "timestamp_utc":1781613442,
        "time_valid":true,
        "uptime_s":510,
        "temperature":22.6,
        "humidity":45.1,
        "voltage":3.27
      }
    ]
  }'
```

Expected response for a new batch:

```json
{"status":"ok","batch_id":1,"inserted_measurements":2,"duplicate_measurements":0}
```

If the same batch is sent again, a typical response is:

```json
{"status":"ok","batch_id":2,"inserted_measurements":0,"duplicate_measurements":2}
```

---

## Database Queries

### Open the database

```bash
sqlite3 pico_measurements.db
```

### Latest batches per device

```sql
SELECT id,
       device_id,
       datetime(server_received_at, 'unixepoch', 'localtime') AS received_local,
       queued_total,
       dropped_total,
       time_valid
FROM batches
WHERE device_id = 'pico-01'
ORDER BY id DESC
LIMIT 20;
```

### Latest measurements

```sql
SELECT id,
       batch_id,
       device_id,
       seq,
       datetime(device_timestamp_utc, 'unixepoch', 'localtime') AS device_time,
       datetime(server_received_at, 'unixepoch', 'localtime') AS server_time,
       temperature,
       humidity,
       voltage
FROM measurements
ORDER BY id DESC
LIMIT 20;
```

### Compare device time vs server receive time

```sql
SELECT device_id,
       seq,
       server_received_at - device_timestamp_utc AS delay_s,
       temperature,
       humidity,
       voltage
FROM measurements
WHERE time_valid = 1
ORDER BY id DESC
LIMIT 20;
```

### Find batches where the Pico dropped data

```sql
SELECT id,
       device_id,
       datetime(server_received_at, 'unixepoch', 'localtime') AS received_local,
       queued_total,
       dropped_total
FROM batches
WHERE dropped_total > 0
ORDER BY id DESC;
```

### Inspect stored raw JSON

```sql
SELECT id,
       device_id,
       raw_json
FROM batches
ORDER BY id DESC
LIMIT 5;
```

---

## Operational Notes

### Batch-level fields

The root JSON fields give useful diagnostic context:

- `device_id`: identifies which Pico sent the batch
- `queued_total`: how many measurements were queued on the Pico when the batch was built
- `dropped_total`: how many measurements had been dropped locally due to full buffer
- `time_valid`: whether the Pico considered its NTP/device time valid

### Measurement-level fields

Each measurement row contains:

- `seq`: monotonically increasing sequence number
- `device_timestamp_utc`: UTC time on the Pico when the measurement was created
- `server_received_at`: UTC time on the server when the batch was received
- `uptime_s`: seconds since Pico boot
- sensor values (`temperature`, `humidity`, `voltage`)

### Why both device and server time are stored

This makes timing analysis much easier:

- detect batch buffering delays
- detect missing or late NTP sync on the Pico
- compare network delay vs local queue delay
- identify retry timing after outages

---

## If You Already Have an Older Database

If you previously ran an earlier schema version, there are two options.

### Option 1: easiest for lab/prototype work

Delete the database and let the server recreate it.

```bash
rm pico_measurements.db
```

### Option 2: migrate manually

If you need to preserve old data, add the new columns manually.

Example migration for `batches`:

```sql
ALTER TABLE batches ADD COLUMN device_id TEXT;
ALTER TABLE batches ADD COLUMN queued_total INTEGER NOT NULL DEFAULT 0;
ALTER TABLE batches ADD COLUMN dropped_total INTEGER NOT NULL DEFAULT 0;
ALTER TABLE batches ADD COLUMN time_valid INTEGER NOT NULL DEFAULT 0;
```

You may also want to create the missing indexes manually after migration.

---

## Troubleshooting

### `401 Unauthorized`

The Pico and the server do not use the same API key.

Check:

```bash
-DAPI_KEY="my-secret-key"
```

on the Pico build side and:

```bash
PICO_API_KEY="my-secret-key" ./endpoint_server
```

on the server side.

### `404 Not Found`

The Pico is not posting to `/measurements`.

Check:

```c
#define SERVER_PATH "/measurements"
```

### `411 Length Required`

The request does not include a valid `Content-Length`.

### Duplicate measurements detected

This is normal if the Pico retries a batch after timeout or connection problems. The server ignores duplicates by design.

### Time appears incorrect

Check:

- whether `time_valid` is `true`
- whether the Pico has completed NTP sync
- whether the server has network access for NTP via the Pico's upstream path
- `delay_s = server_received_at - device_timestamp_utc`

---

## Summary

This solution gives you a robust telemetry pipeline with:

- buffered measurements on Pico
- batch upload over HTTP
- NTP-based device timestamps
- server-side receive timestamps
- batch diagnostics in SQLite
- per-measurement storage in SQLite
- deduplication safe for retry logic

It is a good foundation for:

- lab and prototype telemetry
- resilient Wi‑Fi sensor uploads
- timing analysis between device and backend
- future expansion to dashboards, services, or cloud ingestion
