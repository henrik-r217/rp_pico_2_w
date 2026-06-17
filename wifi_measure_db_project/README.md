# Raspberry Pi Pico 2 W to C Endpoint Server with SQLite

```    
cmake -DAPI_KEY="default-dev-key" -DDEVICE_ID="pico-04" -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=~/code_env/pico-sdk -DWIFI_SSID="Telia-9F1ED6" -DWIFI_PASSWORD="aTuptyeMvx89neCd" ..
```


    
## Overview

This document describes a complete local-network prototype for sending measurement data from a **Raspberry Pi Pico 2 W** to another device using **HTTP POST**.

Recommended architecture:

```text
Raspberry Pi Pico 2 W
   |
   | Wi-Fi + HTTP POST + JSON
   | Header: X-API-Key
   v
C endpoint server on Linux/Raspberry Pi
   |
   v
SQLite database
```

The Pico sends JSON data to a C endpoint server. The server validates an API key, receives the JSON body, and stores it in SQLite.

> This example uses plain HTTP for a local/lab network. For production or untrusted networks, use HTTPS or place the endpoint behind a secure reverse proxy such as Nginx or Caddy.

---

## Part 1 — Raspberry Pi Pico 2 W HTTP POST Client in C

### Function

The Pico code:

- Connects to Wi-Fi
- Opens a TCP connection to the endpoint server
- Sends an HTTP `POST` request to `/measurements`
- Sends JSON data in the request body
- Sends an API key using the HTTP header `X-API-Key`
- Prints the HTTP response using USB serial output

---

## Pico Project Structure

```text
pico_http_post/
├── CMakeLists.txt
├── lwipopts.h
└── main.c
```
## File: `main.c`

<details>
<summary>File: main.c</summary>
   
```c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"


#ifndef WIFI_SSID
#define WIFI_SSID "your-wifi-ssid"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your-wifi-password"
#endif

#ifndef API_KEY
#define API_KEY "default-dev-key"
#endif


#define SERVER_HOST "192.168.1.50"
#define SERVER_PORT 5000
#define SERVER_PATH "/measurements"


typedef struct {
    struct tcp_pcb *pcb;
    ip_addr_t remote_addr;
    bool complete;
    bool success;
} http_client_t;


static void http_client_close(http_client_t *client) {
    if (client->pcb != NULL) {
        tcp_arg(client->pcb, NULL);
        tcp_sent(client->pcb, NULL);
        tcp_recv(client->pcb, NULL);
        tcp_err(client->pcb, NULL);

        err_t err = tcp_close(client->pcb);
        if (err != ERR_OK) {
            tcp_abort(client->pcb);
        }

        client->pcb = NULL;
    }
}


static void http_client_err(void *arg, err_t err) {
    http_client_t *client = (http_client_t *)arg;

    printf("TCP error: %d\n", err);

    if (client != NULL) {
        client->pcb = NULL;
        client->complete = true;
        client->success = false;
    }
}


static err_t http_client_recv(
    void *arg,
    struct tcp_pcb *pcb,
    struct pbuf *p,
    err_t err
) {
    http_client_t *client = (http_client_t *)arg;

    if (p == NULL) {
        printf("Server closed connection\n");
        client->complete = true;
        client->success = true;
        http_client_close(client);
        return ERR_OK;
    }

    if (err == ERR_OK) {
        printf("HTTP response:\n");

        struct pbuf *q = p;
        while (q != NULL) {
            fwrite(q->payload, 1, q->len, stdout);
            q = q->next;
        }

        printf("\n");

        tcp_recved(pcb, p->tot_len);
    }

    pbuf_free(p);
    return ERR_OK;
}


static err_t http_client_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    printf("Sent %u bytes\n", len);
    return ERR_OK;
}


static err_t http_client_connected(
    void *arg,
    struct tcp_pcb *pcb,
    err_t err
) {
    http_client_t *client = (http_client_t *)arg;

    if (err != ERR_OK) {
        printf("Connect failed: %d\n", err);
        client->complete = true;
        client->success = false;
        return err;
    }

    printf("Connected to server\n");

    /*
     * TODO:
     * Replace these fixed values with real sensor readings.
     */
    const char *json_body =
        "{"
        "\"sensor_id\":\"pico-01\","
        "\"temperature\":22.5,"
        "\"humidity\":45.0,"
        "\"voltage\":3.28"
        "}";

    char request[1024];

    int request_len = snprintf(
        request,
        sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "X-API-Key: %s\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        SERVER_PATH,
        SERVER_HOST,
        SERVER_PORT,
        (unsigned int)strlen(json_body),
        API_KEY,
        json_body
    );

    if (request_len <= 0 || request_len >= (int)sizeof(request)) {
        printf("Request buffer too small\n");
        client->complete = true;
        client->success = false;
        return ERR_BUF;
    }

    err_t write_err = tcp_write(
        pcb,
        request,
        request_len,
        TCP_WRITE_FLAG_COPY
    );

    if (write_err != ERR_OK) {
        printf("tcp_write failed: %d\n", write_err);
        client->complete = true;
        client->success = false;
        return write_err;
    }

    err_t output_err = tcp_output(pcb);

    if (output_err != ERR_OK) {
        printf("tcp_output failed: %d\n", output_err);
        client->complete = true;
        client->success = false;
        return output_err;
    }

    printf("HTTP POST sent\n");

    return ERR_OK;
}


static void http_client_connect(http_client_t *client) {
    client->pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);

    if (client->pcb == NULL) {
        printf("Failed to create TCP PCB\n");
        client->complete = true;
        client->success = false;
        return;
    }

    tcp_arg(client->pcb, client);
    tcp_err(client->pcb, http_client_err);
    tcp_recv(client->pcb, http_client_recv);
    tcp_sent(client->pcb, http_client_sent);

    printf("Connecting to server...\n");

    err_t err = tcp_connect(
        client->pcb,
        &client->remote_addr,
        SERVER_PORT,
        http_client_connected
    );

    if (err != ERR_OK) {
        printf("tcp_connect failed: %d\n", err);
        http_client_close(client);
        client->complete = true;
        client->success = false;
    }
}


static void dns_found_callback(
    const char *hostname,
    const ip_addr_t *ipaddr,
    void *arg
) {
    http_client_t *client = (http_client_t *)arg;

    if (ipaddr == NULL) {
        printf("DNS lookup failed for %s\n", hostname);
        client->complete = true;
        client->success = false;
        return;
    }

    client->remote_addr = *ipaddr;

    printf("DNS resolved %s to %s\n", hostname, ipaddr_ntoa(ipaddr));

    cyw43_arch_lwip_begin();
    http_client_connect(client);
    cyw43_arch_lwip_end();
}


static void start_http_post(http_client_t *client) {
    memset(client, 0, sizeof(*client));

    ip_addr_t addr;

    cyw43_arch_lwip_begin();

    if (ipaddr_aton(SERVER_HOST, &addr)) {
        client->remote_addr = addr;
        printf("Using server IP: %s\n", ipaddr_ntoa(&addr));
        http_client_connect(client);
    } else {
        printf("Resolving DNS: %s\n", SERVER_HOST);

        err_t err = dns_gethostbyname(
            SERVER_HOST,
            &addr,
            dns_found_callback,
            client
        );

        if (err == ERR_OK) {
            client->remote_addr = addr;
            http_client_connect(client);
        } else if (err == ERR_INPROGRESS) {
            printf("DNS lookup in progress\n");
        } else {
            printf("DNS lookup failed immediately: %d\n", err);
            client->complete = true;
            client->success = false;
        }
    }

    cyw43_arch_lwip_end();
}


int main() {
    stdio_init_all();

    sleep_ms(3000);
    printf("Pico HTTP POST example starting...\n");

    if (cyw43_arch_init()) {
        printf("cyw43_arch_init failed\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();

    printf("Connecting to Wi-Fi SSID: %s\n", WIFI_SSID);

    int wifi_result = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID,
        WIFI_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK,
        30000
    );

    if (wifi_result != 0) {
        printf("Wi-Fi connect failed: %d\n", wifi_result);
        cyw43_arch_deinit();
        return 1;
    }

    printf("Wi-Fi connected\n");

    http_client_t client;
    start_http_post(&client);

    while (!client.complete) {
        sleep_ms(100);
    }

    if (client.success) {
        printf("HTTP POST completed OK\n");
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    } else {
        printf("HTTP POST failed\n");
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    }

    sleep_ms(2000);

    cyw43_arch_deinit();

    return client.success ? 0 : 1;
}
```
</details>
   
---

## File: `CMakeLists.txt`

<details>
<summary>File: CMakeLists.txt</summary>

```cmake
cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(pico_http_post C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_http_post
    main.c
)

target_include_directories(pico_http_post PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(pico_http_post PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
    API_KEY=\"${API_KEY}\"
)

target_link_libraries(pico_http_post
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
)

pico_enable_stdio_usb(pico_http_post 1)
pico_enable_stdio_uart(pico_http_post 0)

pico_add_extra_outputs(pico_http_post)
```

</details>

---

## File: `lwipopts.h`

<details>
<summary>File: lwipopts.h</summary>
   
```c
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1

#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

#define LWIP_IPV4                   1
#define LWIP_ICMP                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_DHCP                   1

#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000

#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_PBUF               16

#define PBUF_POOL_SIZE              24
#define PBUF_POOL_BUFSIZE           1500

#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1

#define LWIP_TIMEVAL_PRIVATE        0

#endif
```

</details>

---

## Build the Pico Program

From the Pico project folder:

```bash
mkdir build
cd build
```

For **Raspberry Pi Pico 2 W**:

```bash
cmake -DPICO_BOARD=pico2_w \
      -DWIFI_SSID="YourWifi" \
      -DWIFI_PASSWORD="YourWifiPassword" \
      -DAPI_KEY="my-secret-key" \
      ..
```

Build:

```bash
make -j4
```

For the older **Raspberry Pi Pico W**, use:

```bash
cmake -DPICO_BOARD=pico_w \
      -DWIFI_SSID="YourWifi" \
      -DWIFI_PASSWORD="YourWifiPassword" \
      -DAPI_KEY="my-secret-key" \
      ..
```

---

# Part 2 — C Endpoint Server with SQLite

## Function

The C endpoint server:

- Listens on TCP port `5000`
- Accepts `POST /measurements`
- Validates the HTTP header `X-API-Key`
- Reads the expected API key from the environment variable `PICO_API_KEY`
- Reads the JSON request body
- Stores the JSON payload in SQLite
- Returns a JSON response to the Pico

---

## Server Project Structure

```text
endpoint_server/
└── endpoint_server.c
```

---

## File: `endpoint_server.c`

<details>
<summary>File: endpoint_server.c</summary>
   
```c
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <sqlite3.h>


#define SERVER_PORT 5000
#define MAX_HEADER_SIZE 8192
#define API_KEY_ENV_NAME "PICO_API_KEY"
#define DB_FILE "pico_measurements.db"


static sqlite3 *db = NULL;


static void send_response(
    int client_fd,
    int status_code,
    const char *status_text,
    const char *body
) {
    char response[1024];

    int body_len = body ? strlen(body) : 0;

    int len = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status_code,
        status_text,
        body_len,
        body ? body : ""
    );

    send(client_fd, response, len, 0);
}


static int init_database(void) {
    int rc = sqlite3_open(DB_FILE, &db);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS measurements ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "received_at INTEGER NOT NULL,"
        "payload TEXT NOT NULL"
        ");";

    char *err_msg = NULL;

    rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    return 0;
}


static int save_payload_to_database(const char *payload) {
    const char *sql =
        "INSERT INTO measurements (received_at, payload) "
        "VALUES (?, ?);";

    sqlite3_stmt *stmt = NULL;

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_prepare_v2 failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
    sqlite3_bind_text(stmt, 2, payload, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "sqlite3_step failed: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_finalize(stmt);

    return 0;
}


static char *find_header_value(const char *headers, const char *header_name) {
    static char value[512];

    value[0] = '\0';

    size_t name_len = strlen(header_name);
    const char *p = headers;

    while (*p) {
        const char *line_end = strstr(p, "\r\n");

        if (!line_end) {
            break;
        }

        size_t line_len = line_end - p;

        if (
            line_len > name_len + 1 &&
            strncasecmp(p, header_name, name_len) == 0 &&
            p[name_len] == ':'
        ) {
            const char *v = p + name_len + 1;

            while (*v == ' ' || *v == '\t') {
                v++;
            }

            size_t v_len = line_end - v;

            if (v_len >= sizeof(value)) {
                v_len = sizeof(value) - 1;
            }

            memcpy(value, v, v_len);
            value[v_len] = '\0';

            return value;
        }

        p = line_end + 2;
    }

    return NULL;
}


static int get_content_length(const char *headers) {
    char *value = find_header_value(headers, "Content-Length");

    if (!value) {
        return -1;
    }

    return atoi(value);
}


static bool api_key_is_valid(const char *headers) {
    char *value = find_header_value(headers, "X-API-Key");

    if (!value) {
        return false;
    }

    const char *expected_key = getenv(API_KEY_ENV_NAME);

    if (expected_key == NULL || expected_key[0] == '\0') {
        fprintf(stderr, "Environment variable %s is not set\n", API_KEY_ENV_NAME);
        return false;
    }

    return strcmp(value, expected_key) == 0;
}


static bool request_line_is_valid(const char *request) {
    return strncmp(request, "POST /measurements HTTP/1.1", 27) == 0 ||
           strncmp(request, "POST /measurements HTTP/1.0", 27) == 0;
}


static void handle_client(int client_fd) {
    char header_buffer[MAX_HEADER_SIZE + 1];
    int total_read = 0;
    int header_end_offset = -1;

    memset(header_buffer, 0, sizeof(header_buffer));

    while (total_read < MAX_HEADER_SIZE) {
        int n = recv(
            client_fd,
            header_buffer + total_read,
            MAX_HEADER_SIZE - total_read,
            0
        );

        if (n <= 0) {
            send_response(
                client_fd,
                400,
                "Bad Request",
                "{\"error\":\"failed to read request\"}"
            );
            return;
        }

        total_read += n;
        header_buffer[total_read] = '\0';

        char *header_end = strstr(header_buffer, "\r\n\r\n");

        if (header_end) {
            header_end_offset = header_end - header_buffer + 4;
            break;
        }
    }

    if (header_end_offset < 0) {
        send_response(
            client_fd,
            413,
            "Payload Too Large",
            "{\"error\":\"headers too large\"}"
        );
        return;
    }

    if (!request_line_is_valid(header_buffer)) {
        send_response(
            client_fd,
            404,
            "Not Found",
            "{\"error\":\"use POST /measurements\"}"
        );
        return;
    }

    if (!api_key_is_valid(header_buffer)) {
        send_response(
            client_fd,
            401,
            "Unauthorized",
            "{\"error\":\"invalid api key\"}"
        );
        return;
    }

    int content_length = get_content_length(header_buffer);

    if (content_length <= 0) {
        send_response(
            client_fd,
            411,
            "Length Required",
            "{\"error\":\"missing or invalid content length\"}"
        );
        return;
    }

    if (content_length > 1024 * 1024) {
        send_response(
            client_fd,
            413,
            "Payload Too Large",
            "{\"error\":\"payload too large\"}"
        );
        return;
    }

    char *body = calloc(content_length + 1, 1);

    if (!body) {
        send_response(
            client_fd,
            500,
            "Internal Server Error",
            "{\"error\":\"out of memory\"}"
        );
        return;
    }

    int already_have = total_read - header_end_offset;

    if (already_have > content_length) {
        already_have = content_length;
    }

    if (already_have > 0) {
        memcpy(body, header_buffer + header_end_offset, already_have);
    }

    int body_read = already_have;

    while (body_read < content_length) {
        int n = recv(
            client_fd,
            body + body_read,
            content_length - body_read,
            0
        );

        if (n <= 0) {
            free(body);
            send_response(
                client_fd,
                400,
                "Bad Request",
                "{\"error\":\"failed to read body\"}"
            );
            return;
        }

        body_read += n;
    }

    body[content_length] = '\0';

    printf("Received payload:\n%s\n", body);

    if (save_payload_to_database(body) != 0) {
        free(body);
        send_response(
            client_fd,
            500,
            "Internal Server Error",
            "{\"error\":\"database insert failed\"}"
        );
        return;
    }

    free(body);

    send_response(
        client_fd,
        200,
        "OK",
        "{\"status\":\"ok\"}"
    );
}


int main(void) {
    const char *api_key = getenv(API_KEY_ENV_NAME);

    if (api_key == NULL || api_key[0] == '\0') {
        fprintf(stderr, "Error: environment variable %s is not set\n", API_KEY_ENV_NAME);
        fprintf(stderr, "Example:\n");
        fprintf(stderr, "  %s=\"my-secret-key\" ./endpoint_server\n", API_KEY_ENV_NAME);
        return 1;
    }

    if (init_database() != 0) {
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;

    if (
        setsockopt(
            server_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &opt,
            sizeof(opt)
        ) < 0
    ) {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (
        bind(
            server_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)
        ) < 0
    ) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("C endpoint server running on port %d\n", SERVER_PORT);
    printf("Endpoint: POST /measurements\n");
    printf("Database: %s\n", DB_FILE);
    printf("API key source: environment variable %s\n", API_KEY_ENV_NAME);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_addr,
            &client_len
        );

        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        printf(
            "Connection from %s:%d\n",
            inet_ntoa(client_addr.sin_addr),
            ntohs(client_addr.sin_port)
        );

        handle_client(client_fd);

        close(client_fd);
    }

    close(server_fd);
    sqlite3_close(db);

    return 0;
}
```

</details>

---

## Build the Endpoint Server

On Linux/Raspberry Pi:

```bash
sudo apt update
sudo apt install build-essential libsqlite3-dev sqlite3
```

Compile:

```bash
gcc endpoint_server.c -o endpoint_server -lsqlite3
```

Start the server with the API key as an environment variable:

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

# Test with `curl`

Before testing the Pico, verify the endpoint server with `curl`:

```bash
curl -X POST http://192.168.1.50:5000/measurements \
  -H "Content-Type: application/json" \
  -H "X-API-Key: my-secret-key" \
  -d '{"sensor_id":"pico-01","temperature":22.5,"humidity":45.0,"voltage":3.28}'
```

Expected response:

```json
{"status":"ok"}
```

If the API key is wrong, the server returns:

```json
{"error":"invalid api key"}
```

---

# Check the SQLite Database

The database is created automatically as:

```text
pico_measurements.db
```

Open the database:

```bash
sqlite3 pico_measurements.db
```

Show stored measurements:

```sql
SELECT id,
       datetime(received_at, 'unixepoch', 'localtime'),
       payload
FROM measurements;
```

Example result:

```text
1|2026-06-10 21:05:12|{"sensor_id":"pico-01","temperature":22.5,"humidity":45.0,"voltage":3.28}
```

Exit SQLite:

```sql
.quit
```

---

# Important Configuration

## Pico Configuration

In `main.c`, make sure this matches the endpoint server:

```c
#define SERVER_HOST "192.168.1.50"
#define SERVER_PORT 5000
#define SERVER_PATH "/measurements"
```

The API key is passed during the CMake build:

```bash
-DAPI_KEY="my-secret-key"
```

The Pico sends it as this HTTP header:

```http
X-API-Key: my-secret-key
```

---

## Server Configuration

The server reads the expected API key from the environment variable:

```bash
PICO_API_KEY="my-secret-key" ./endpoint_server
```

This means:

```text
Pico API_KEY must match server PICO_API_KEY.
```

---

# Troubleshooting

## Pico prints `HTTP POST failed`

Check:

- The endpoint server is running
- `SERVER_HOST` is the correct server IP address
- Pico and server are on the same network
- Port `5000` is not blocked by a firewall
- The server path is `/measurements`

---

## Server returns `401 Unauthorized`

The API key does not match.

Check that the Pico is built with:

```bash
-DAPI_KEY="my-secret-key"
```

and that the server is started with:

```bash
PICO_API_KEY="my-secret-key" ./endpoint_server
```

---

## Server returns `404 Not Found`

The Pico is sending to the wrong path.

Check:

```c
#define SERVER_PATH "/measurements"
```

---

## Server returns `411 Length Required`

The HTTP request does not contain a valid `Content-Length` header.

The Pico request must include:

```http
Content-Length: <number of bytes>
```

---

## No Data Appears in SQLite

Check the server console for:

```text
Received payload:
...
```

Then check the database:

```bash
sqlite3 pico_measurements.db
```

```sql
SELECT * FROM measurements;
```

---

# Production Notes

This is a useful prototype for a local network or lab setup.

For more robust operation, consider:

- HTTPS or a secure reverse proxy
- JSON validation
- Separate SQLite columns for `sensor_id`, `temperature`, `humidity`, and `voltage`
- A multi-threaded server or a `poll()`/`select()` based server
- Running the endpoint as a `systemd` service
- Log rotation
- Watchdog support on the Pico
- Retry and buffering on the Pico if the server is unavailable

---

# Summary

The complete solution is:

```text
Pico 2 W:
  C code
  Wi-Fi
  HTTP POST
  JSON payload
  X-API-Key header

Linux/Raspberry Pi endpoint:
  C server
  Socket API
  API key from PICO_API_KEY
  SQLite storage
```

Minimal test sequence:

```text
1. Start the endpoint server
2. Test with curl
3. Build the Pico code using the same API_KEY
4. Flash the Pico
5. Check the server log
6. Check SQLite
```
