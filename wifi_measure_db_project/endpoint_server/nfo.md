gcc endpoint_server.c -o endpoint_server -lsqlite3 -lcjson

PICO_API_KEY="default-dev-key" ./endpoint_server
