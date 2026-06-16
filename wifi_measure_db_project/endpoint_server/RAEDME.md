# endpoint_server.c

Den gör detta:

- Lyssnar på port 5000
- Tar emot POST /measurements
 - Kräver header:

```
X-API-Key: byt-till-en-hemlig-nyckel
```

Tar emot JSON från Pico
Sparar hela JSON-payloaden i en SQLite-databas

### Installera SQLite development package:
```
sudo apt update
sudo apt install build-essential libsqlite3-dev sqlite3
```
### Kompilera
```
gcc endpoint_server.c -o endpoint_server -lsqlite3
```

### Kör
```
./endpoint_server
```

Servern lyssnar då på:
```
http://<server-ip>:5000/measurements
```

## Testa med curl

Från en annan dator:

```
curl -X POST http://192.168.1.50:5000/measurements \
  -H "Content-Type: application/json" \
  -H "X-API-Key: byt-till-en-hemlig-nyckel" \
  -d '{"sensor_id":"pico-01","temperature":22.5,"humidity":45.0,"voltage":3.28}'
```

Förväntat svar:
```
{"status":"ok"}
```


# kontrollera databasen

sqlite3 pico_measurements.db

kör:
```
sqlite> SELECT payload FROM measurements;
```
Funkar inte:
```
SELECT id, datetime(received_at, 'unixepoch', 'localtime'), payload
```
FROM measurements;

