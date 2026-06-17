#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "sht30.h"

#define DEBUG

#ifdef DEBUG
#define DEBUG_print printf
#else
#define DEBUG_print // macros
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "your-ssid"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your-password"
#endif

#ifndef API_KEY
#define API_KEY "default-dev-key"
#endif

#ifndef DEVICE_ID
#define DEVICE_ID "pico-01"
#endif

#define SERVER_HOST "192.168.1.99"
#define SERVER_PORT 5000
#define SERVER_PATH "/measurements"

#define SEND_INTERVAL_MS                 10000u
#define WIFI_CONNECT_TIMEOUT_MS          30000u
#define HTTP_TIMEOUT_MS                  10000u
#define RETRY_DELAY_MS                    5000u

#define MEASUREMENT_QUEUE_CAPACITY          64u
#define MAX_BATCH_SIZE                        6u
#define MAX_BATCH_REQUESTS_PER_CYCLE          4u
#define PAYLOAD_BUFFER_SIZE                2048u

#define NTP_SERVER                    "pool.ntp.org"
#define NTP_PORT                      123
#define NTP_MSG_LEN                   48
#define NTP_DELTA                     2208988800UL
#define NTP_RESEND_MS                 10000u
#define NTP_REFRESH_INTERVAL_MS    3600000u
#define NTP_STARTUP_WAIT_MS           15000u

#define APP_I2C_PORT     i2c0
#define APP_I2C_SDA_PIN  4
#define APP_I2C_SCL_PIN  5
#define APP_I2C_BAUDRATE 100000


typedef struct {
    uint32_t seq;
    uint32_t uptime_s;
    uint32_t timestamp_utc;
    bool time_valid;
    float temperature;
    float humidity;
} measurement_t;

typedef struct {
    struct tcp_pcb *pcb;
    ip_addr_t remote_addr;
    bool complete;
    bool success;
    bool got_http_status;
    int http_status;
    char response_head[128];
    size_t response_head_len;
    char payload[PAYLOAD_BUFFER_SIZE];
} http_client_t;

typedef struct {
    ip_addr_t server_addr;
    struct udp_pcb *udp_pcb;
    bool dns_in_progress;
    bool request_in_flight;
    bool time_valid;
    uint32_t epoch_at_sync;
    uint64_t ms_at_sync;
    uint64_t next_sync_ms;
    uint64_t request_deadline_ms;
} ntp_state_t;

static int status;
static sht30_t sensor;
static measurement_t measurement_queue[MEASUREMENT_QUEUE_CAPACITY];
static size_t queue_head = 0;
static size_t queue_tail = 0;
static size_t queue_count = 0;
static uint32_t next_sequence_number = 1;
static uint32_t dropped_measurements = 0;
static ntp_state_t g_ntp;

/* Forward declarations */
static bool wifi_is_connected(void);
static bool ensure_wifi_connected(void);
static uint64_t now_ms(void);
static void sleep_with_housekeeping(uint32_t total_ms);

static void ntp_init_module(void);
static void ntp_poll(void);
static bool ntp_time_is_valid(void);
static uint32_t ntp_now_utc(void);
static bool ntp_wait_for_initial_sync(uint32_t timeout_ms);

static measurement_t create_measurement(void);
static bool build_batch_payload(char *out, size_t out_size, size_t *batched_count);
static void try_send_buffered_measurements(void);
static bool send_one_batch(size_t *sent_count);

static void http_client_reset(http_client_t *client);
static void http_client_abort(http_client_t *client);
static void http_client_close(http_client_t *client);
static void start_http_post(http_client_t *client, const char *payload);
static bool wait_for_http_completion(http_client_t *client, uint32_t timeout_ms);

/* =========================================================
 * Utility
 * ========================================================= */

static uint64_t now_ms(void) {
    return (uint64_t)to_ms_since_boot(get_absolute_time());
}

static void sleep_with_housekeeping(uint32_t total_ms) {
    uint32_t remaining = total_ms;
    while (remaining > 0) {
        uint32_t step = remaining > 100u ? 100u : remaining;
        sleep_ms(step);
        ntp_poll();
        remaining -= step;
    }
}

/* =========================================================
 * Queue helpers
 * ========================================================= */

static size_t queue_size(void) {
    return queue_count;
}

static bool queue_is_empty(void) {
    return queue_count == 0;
}

static bool queue_is_full(void) {
    return queue_count >= MEASUREMENT_QUEUE_CAPACITY;
}

static measurement_t *queue_get(size_t index_from_head) {
    if (index_from_head >= queue_count) {
        return NULL;
    }

    size_t idx = (queue_head + index_from_head) % MEASUREMENT_QUEUE_CAPACITY;
    return &measurement_queue[idx];
}

static void queue_pop_one(void) {
    if (queue_is_empty()) {
        return;
    }

    queue_head = (queue_head + 1u) % MEASUREMENT_QUEUE_CAPACITY;
    queue_count--;
}

static void queue_pop_n(size_t n) {
    while (n > 0 && !queue_is_empty()) {
        queue_pop_one();
        n--;
    }
}

static void queue_push(const measurement_t *m) {
    if (queue_is_full()) {
        queue_head = (queue_head + 1u) % MEASUREMENT_QUEUE_CAPACITY;
        queue_count--;
        dropped_measurements++;
        printf("Queue full -> dropped oldest measurement (total dropped=%lu)\n",
               (unsigned long)dropped_measurements);
    }

    measurement_queue[queue_tail] = *m;
    queue_tail = (queue_tail + 1u) % MEASUREMENT_QUEUE_CAPACITY;
    queue_count++;
}

/* =========================================================
 * Wi-Fi helpers
 * ========================================================= */

static bool wifi_is_connected(void) {
    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    return status == CYW43_LINK_UP;
}

static bool ensure_wifi_connected(void) {
    if (wifi_is_connected()) {
        return true;
    }

    printf("Wi-Fi is down, trying to connect...\n");
    cyw43_arch_enable_sta_mode();

    int result = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID,
        WIFI_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK,
        WIFI_CONNECT_TIMEOUT_MS
    );

    if (result != 0) {
        printf("Wi-Fi connect failed: %d\n", result);
        return false;
    }

    printf("Wi-Fi connected\n");
    return true;
}

/* =========================================================
 * NTP helpers
 * ========================================================= */

static bool ntp_time_is_valid(void) {
    return g_ntp.time_valid;
}

static uint32_t ntp_now_utc(void) {
    if (!g_ntp.time_valid) {
        return 0;
    }

    uint64_t elapsed_ms = now_ms() - g_ntp.ms_at_sync;
    return g_ntp.epoch_at_sync + (uint32_t)(elapsed_ms / 1000u);
}

static void ntp_mark_synced(uint32_t epoch_utc) {
    g_ntp.epoch_at_sync = epoch_utc;
    g_ntp.ms_at_sync = now_ms();
    g_ntp.time_valid = true;
    g_ntp.request_in_flight = false;
    g_ntp.next_sync_ms = now_ms() + NTP_REFRESH_INTERVAL_MS;
    printf("NTP synced: epoch=%lu\n", (unsigned long)epoch_utc);
}

static void ntp_send_request(void) {
    if (g_ntp.udp_pcb == NULL) {
        printf("NTP: no UDP PCB\n");
        return;
    }

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
    if (!p) {
        printf("NTP: pbuf_alloc failed\n");
        return;
    }

    uint8_t *req = (uint8_t *)p->payload;
    memset(req, 0, NTP_MSG_LEN);
    req[0] = 0x1b;

    cyw43_arch_lwip_begin();
    err_t err = udp_sendto(g_ntp.udp_pcb, p, &g_ntp.server_addr, NTP_PORT);
    cyw43_arch_lwip_end();

    pbuf_free(p);

    if (err != ERR_OK) {
        printf("NTP: udp_sendto failed: %d\n", err);
        return;
    }

    g_ntp.request_in_flight = true;
    g_ntp.request_deadline_ms = now_ms() + NTP_RESEND_MS;
    printf("NTP: request sent\n");
}

static void ntp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port) {
    (void)arg;
    (void)pcb;

    if (!p) {
        return;
    }

    if (addr && ip_addr_cmp(addr, &g_ntp.server_addr) && port == NTP_PORT && p->tot_len == NTP_MSG_LEN) {
        uint8_t buf[NTP_MSG_LEN];
        pbuf_copy_partial(p, buf, NTP_MSG_LEN, 0);

        uint8_t mode = buf[0] & 0x7u;
        uint8_t stratum = buf[1];

        if (mode == 0x4u && stratum != 0u) {
            uint32_t seconds_since_1900 =
                ((uint32_t)buf[40] << 24) |
                ((uint32_t)buf[41] << 16) |
                ((uint32_t)buf[42] << 8)  |
                ((uint32_t)buf[43]);

            uint32_t epoch_utc = seconds_since_1900 - NTP_DELTA;
            ntp_mark_synced(epoch_utc);
        } else {
            printf("NTP: invalid reply (mode=%u stratum=%u)\n", mode, stratum);
            g_ntp.request_in_flight = false;
            g_ntp.next_sync_ms = now_ms() + NTP_RESEND_MS;
        }
    }

    pbuf_free(p);
}

static void ntp_dns_found_cb(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
    (void)hostname;
    (void)arg;

    g_ntp.dns_in_progress = false;

    if (!ipaddr) {
        printf("NTP: DNS lookup failed\n");
        g_ntp.next_sync_ms = now_ms() + NTP_RESEND_MS;
        return;
    }

    g_ntp.server_addr = *ipaddr;
    printf("NTP: server %s\n", ipaddr_ntoa(ipaddr));
    ntp_send_request();
}

static void ntp_init_module(void) {
    memset(&g_ntp, 0, sizeof(g_ntp));

    cyw43_arch_lwip_begin();
    g_ntp.udp_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (g_ntp.udp_pcb) {
        udp_recv(g_ntp.udp_pcb, ntp_recv_cb, NULL);
    }
    cyw43_arch_lwip_end();

    if (!g_ntp.udp_pcb) {
        printf("NTP: failed to create UDP PCB\n");
        return;
    }

    g_ntp.next_sync_ms = now_ms();
}

static void ntp_start_sync(void) {
    if (g_ntp.dns_in_progress || g_ntp.request_in_flight) {
        return;
    }

    printf("NTP: resolving %s\n", NTP_SERVER);

    cyw43_arch_lwip_begin();
    ip_addr_t tmp_addr;
    err_t err = dns_gethostbyname(NTP_SERVER, &tmp_addr, ntp_dns_found_cb, NULL);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        g_ntp.server_addr = tmp_addr;
        printf("NTP: DNS cached -> %s\n", ipaddr_ntoa(&tmp_addr));
        ntp_send_request();
    } else if (err == ERR_INPROGRESS) {
        g_ntp.dns_in_progress = true;
    } else {
        printf("NTP: dns_gethostbyname failed: %d\n", err);
        g_ntp.next_sync_ms = now_ms() + NTP_RESEND_MS;
    }
}

static void ntp_poll(void) {
    if (!wifi_is_connected()) {
        return;
    }

    uint64_t ms = now_ms();

    if (g_ntp.request_in_flight && ms >= g_ntp.request_deadline_ms) {
        printf("NTP: request timeout, retry later\n");
        g_ntp.request_in_flight = false;
        g_ntp.next_sync_ms = ms + NTP_RESEND_MS;
    }

    if (!g_ntp.request_in_flight && !g_ntp.dns_in_progress && ms >= g_ntp.next_sync_ms) {
        ntp_start_sync();
    }
}

static bool ntp_wait_for_initial_sync(uint32_t timeout_ms) {
    uint64_t deadline = now_ms() + timeout_ms;

    while (!ntp_time_is_valid() && now_ms() < deadline) {
        ntp_poll();
        sleep_ms(100);
    }

    return ntp_time_is_valid();
}

/* =========================================================
 * Measurement generation
 * ========================================================= */

static measurement_t create_measurement(void) {
    measurement_t m;
    float temperature_c = 0.0f;
    float humidity_rh   = 0.0f;

    m.seq = next_sequence_number++;
    m.uptime_s = (uint32_t)(now_ms() / 1000u);
    m.timestamp_utc = ntp_now_utc();
    m.time_valid = ntp_time_is_valid();

    status = sht30_read(&sensor, &temperature_c, &humidity_rh);
    if (status == SHT30_OK) {
      printf("Temperature: %.2f C, Humidity: %.2f %%RH\n",
             temperature_c,
             humidity_rh);
    } else {
      printf("sht30_read failed: %s\n", sht30_strerror(status));
    }
    
    /* TODO: Replace with real sensor reads */
    m.temperature = temperature_c;
    m.humidity = humidity_rh;

    return m;
}

/* =========================================================
 * JSON helpers
 * ========================================================= */

static bool append_to_buffer(char *buf, size_t buf_size, size_t *used, const char *text) {
    size_t text_len = strlen(text);
    if (*used + text_len >= buf_size) {
        return false;
    }

    memcpy(buf + *used, text, text_len);
    *used += text_len;
    buf[*used] = '\0';
    return true;
}

static bool append_format(char *buf, size_t buf_size, size_t *used, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *used, buf_size - *used, fmt, args);
    va_end(args);

    if (n < 0) {
        return false;
    }

    if ((size_t)n >= (buf_size - *used)) {
        return false;
    }

    *used += (size_t)n;
    return true;
}

static bool build_batch_payload(char *out, size_t out_size, size_t *batched_count) {
    size_t used = 0;
    size_t count = 0;

    out[0] = '\0';
    *batched_count = 0;

    if (!append_format(
            out, out_size, &used,
            "{"
            "\"device_id\":\"%s\","
            "\"queued_total\":%lu,"
            "\"dropped_total\":%lu,"
            "\"time_valid\":%s,"
            "\"measurements\":[",
            DEVICE_ID,
            (unsigned long)queue_size(),
            (unsigned long)dropped_measurements,
            ntp_time_is_valid() ? "true" : "false")) {
        printf("build_batch_payload: failed to write JSON header (used=%lu size=%lu)\n",
               (unsigned long)used,
               (unsigned long)out_size);
        return false;
    }

    size_t max_items = queue_size();
    if (max_items > MAX_BATCH_SIZE) {
        max_items = MAX_BATCH_SIZE;
    }

    for (size_t i = 0; i < max_items; i++) {
        measurement_t *m = queue_get(i);
        if (m == NULL) {
            break;
        }

        char item_buf[256];
        int item_len = snprintf(
            item_buf,
            sizeof(item_buf),
            "%s{"
            "\"seq\":%lu,"
            "\"timestamp_utc\":%lu,"
            "\"time_valid\":%s,"
            "\"uptime_s\":%lu,"
            "\"temperature\":%.2f,"
            "\"humidity\":%.2f"
            "}",
            (count > 0u) ? "," : "",
            (unsigned long)m->seq,
            (unsigned long)m->timestamp_utc,
            m->time_valid ? "true" : "false",
            (unsigned long)m->uptime_s,
            m->temperature,
            m->humidity
        );

        if (item_len <= 0 || item_len >= (int)sizeof(item_buf)) {
            printf("build_batch_payload: measurement format failed for seq=%lu\n",
                   (unsigned long)m->seq);
            break;
        }

        size_t reserve_tail = 3; /* ]} + nul */
        if (used + (size_t)item_len + reserve_tail >= out_size) {
            printf("build_batch_payload: stopping at count=%lu, next seq=%lu would exceed buffer (used=%lu size=%lu)\n",
                   (unsigned long)count,
                   (unsigned long)m->seq,
                   (unsigned long)used,
                   (unsigned long)out_size);
            break;
        }

        memcpy(out + used, item_buf, (size_t)item_len);
        used += (size_t)item_len;
        out[used] = '\0';
        count++;
    }

    if (count == 0u) {
        printf("build_batch_payload: no measurement fits in payload buffer (size=%lu)\n",
               (unsigned long)out_size);
        return false;
    }

    if (!append_to_buffer(out, out_size, &used, "]}")) {
        printf("build_batch_payload: failed to append JSON trailer (used=%lu size=%lu count=%lu)\n",
               (unsigned long)used,
               (unsigned long)out_size,
               (unsigned long)count);
        return false;
    }

    *batched_count = count;

    printf("build_batch_payload: built batch with %lu measurements, payload_bytes=%lu\n",
           (unsigned long)count,
           (unsigned long)used);

    return true;
}

/* =========================================================
 * HTTP client helpers
 * ========================================================= */

static void http_client_reset(http_client_t *client) {
    memset(client, 0, sizeof(*client));
}

static void http_client_abort(http_client_t *client) {
    if (client->pcb != NULL) {
        tcp_arg(client->pcb, NULL);
        tcp_sent(client->pcb, NULL);
        tcp_recv(client->pcb, NULL);
        tcp_err(client->pcb, NULL);
        tcp_abort(client->pcb);
        client->pcb = NULL;
    }
}

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

static void parse_http_status(http_client_t *client) {
    if (client->got_http_status) {
        return;
    }

    char *line_end = strstr(client->response_head, "\r\n");
    if (!line_end) {
        return;
    }

    int status = 0;
    if (sscanf(client->response_head, "HTTP/%*s %d", &status) == 1) {
        client->http_status = status;
        client->got_http_status = true;
        printf("Parsed HTTP status: %d\n", status);
    }
}

static void append_response_head(http_client_t *client, const void *data, size_t len) {
    if (client->got_http_status) {
        return;
    }

    size_t remaining = sizeof(client->response_head) - 1u - client->response_head_len;
    if (remaining == 0u) {
        return;
    }

    if (len > remaining) {
        len = remaining;
    }

    memcpy(client->response_head + client->response_head_len, data, len);
    client->response_head_len += len;
    client->response_head[client->response_head_len] = '\0';
    parse_http_status(client);
}

static void http_err_cb(void *arg, err_t err) {
    http_client_t *client = (http_client_t *)arg;
    printf("HTTP TCP error: %d\n", err);

    if (client != NULL) {
        client->pcb = NULL;
        client->complete = true;
        client->success = false;
    }
}

static err_t http_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_client_t *client = (http_client_t *)arg;

    if (client == NULL) {
        if (p) {
            pbuf_free(p);
        }
        return ERR_OK;
    }

    if (p == NULL) {
        client->complete = true;
        if (client->got_http_status) {
            client->success = (client->http_status >= 200 && client->http_status < 300);
        } else {
            client->success = false;
        }
        http_client_close(client);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        client->complete = true;
        client->success = false;
        return err;
    }

    struct pbuf *q = p;
    while (q != NULL) {
        append_response_head(client, q->payload, q->len);
        fwrite(q->payload, 1, q->len, stdout);
        q = q->next;
    }
    printf("\n");

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t http_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)arg;
    (void)pcb;
    printf("HTTP sent %u bytes\n", len);
    return ERR_OK;
}

static err_t http_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    http_client_t *client = (http_client_t *)arg;

    if (client == NULL) {
        return ERR_ARG;
    }

    if (err != ERR_OK) {
        printf("tcp_connect callback error: %d\n", err);
        client->complete = true;
        client->success = false;
        return err;
    }

    char request[3072];
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
        (unsigned)strlen(client->payload),
        API_KEY,
        client->payload
    );

    if (request_len <= 0 || request_len >= (int)sizeof(request)) {
        printf("HTTP request too large\n");
        client->complete = true;
        client->success = false;
        return ERR_BUF;
    }

    err_t werr = tcp_write(pcb, request, (u16_t)request_len, TCP_WRITE_FLAG_COPY);
    if (werr != ERR_OK) {
        printf("tcp_write failed: %d\n", werr);
        client->complete = true;
        client->success = false;
        return werr;
    }

    err_t oerr = tcp_output(pcb);
    if (oerr != ERR_OK) {
        printf("tcp_output failed: %d\n", oerr);
        client->complete = true;
        client->success = false;
        return oerr;
    }

    printf("HTTP POST sent, payload_bytes=%u\n", (unsigned)strlen(client->payload));
    return ERR_OK;
}

static void http_dns_found_cb(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
    http_client_t *client = (http_client_t *)arg;

    if (client == NULL) {
        return;
    }

    if (ipaddr == NULL) {
        printf("HTTP DNS lookup failed for %s\n", hostname);
        client->complete = true;
        client->success = false;
        return;
    }

    client->remote_addr = *ipaddr;
    printf("HTTP DNS resolved %s -> %s\n", hostname, ipaddr_ntoa(ipaddr));

    cyw43_arch_lwip_begin();
    err_t err = tcp_connect(client->pcb, &client->remote_addr, SERVER_PORT, http_connected_cb);
    cyw43_arch_lwip_end();

    if (err != ERR_OK) {
        printf("tcp_connect failed after DNS: %d\n", err);
        client->complete = true;
        client->success = false;
        http_client_abort(client);
    }
}

static void start_http_post(http_client_t *client, const char *payload) {
    http_client_reset(client);
    strncpy(client->payload, payload, sizeof(client->payload) - 1u);
    client->payload[sizeof(client->payload) - 1u] = '\0';

    client->pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (client->pcb == NULL) {
        printf("tcp_new_ip_type failed\n");
        client->complete = true;
        client->success = false;
        return;
    }

    tcp_arg(client->pcb, client);
    tcp_recv(client->pcb, http_recv_cb);
    tcp_sent(client->pcb, http_sent_cb);
    tcp_err(client->pcb, http_err_cb);

    ip_addr_t addr;
    if (ipaddr_aton(SERVER_HOST, &addr)) {
        client->remote_addr = addr;
        cyw43_arch_lwip_begin();
        err_t err = tcp_connect(client->pcb, &client->remote_addr, SERVER_PORT, http_connected_cb);
        cyw43_arch_lwip_end();

        if (err != ERR_OK) {
            printf("tcp_connect failed immediately: %d\n", err);
            client->complete = true;
            client->success = false;
            http_client_abort(client);
        }
        return;
    }

    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(SERVER_HOST, &addr, http_dns_found_cb, client);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        client->remote_addr = addr;
        cyw43_arch_lwip_begin();
        err = tcp_connect(client->pcb, &client->remote_addr, SERVER_PORT, http_connected_cb);
        cyw43_arch_lwip_end();
        if (err != ERR_OK) {
            printf("tcp_connect failed after cached DNS: %d\n", err);
            client->complete = true;
            client->success = false;
            http_client_abort(client);
        }
    } else if (err == ERR_INPROGRESS) {
        /* Wait for DNS callback */
    } else {
        printf("dns_gethostbyname failed: %d\n", err);
        client->complete = true;
        client->success = false;
        http_client_abort(client);
    }
}

static bool wait_for_http_completion(http_client_t *client, uint32_t timeout_ms) {
    uint64_t deadline = now_ms() + timeout_ms;

    while (!client->complete && now_ms() < deadline) {
        sleep_ms(100);
        ntp_poll();
    }

    if (!client->complete) {
        printf("HTTP timeout after %lu ms\n", (unsigned long)timeout_ms);
        http_client_abort(client);
        client->complete = true;
        client->success = false;
        return false;
    }

    return client->success;
}

/* =========================================================
 * Batch sending
 * ========================================================= */

static bool send_one_batch(size_t *sent_count) {
    *sent_count = 0;

    if (queue_is_empty()) {
        printf("send_one_batch: queue is empty\n");
        return true;
    }

    if (!ensure_wifi_connected()) {
        printf("send_one_batch: cannot send, Wi-Fi unavailable\n");
        return false;
    }

    char payload[PAYLOAD_BUFFER_SIZE];
    size_t batch_count = 0;

    if (!build_batch_payload(payload, sizeof(payload), &batch_count)) {
        printf("send_one_batch: failed to build batch payload (queue=%lu, buffer=%u, max_batch=%u)\n",
               (unsigned long)queue_size(),
               (unsigned)PAYLOAD_BUFFER_SIZE,
               (unsigned)MAX_BATCH_SIZE);
        return false;
    }

    if (batch_count == 0) {
        printf("send_one_batch: batch_count == 0\n");
        return false;
    }

    printf("send_one_batch: prepared batch with %lu measurements (queue depth before send=%lu)\n",
           (unsigned long)batch_count,
           (unsigned long)queue_size());

    http_client_t client;
    start_http_post(&client, payload);
    bool ok = wait_for_http_completion(&client, HTTP_TIMEOUT_MS);

    if (ok) {
        printf("send_one_batch: POST OK -> removing %lu measurements from queue\n",
               (unsigned long)batch_count);
        queue_pop_n(batch_count);
        *sent_count = batch_count;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        printf("send_one_batch: queue depth after send=%lu\n",
               (unsigned long)queue_size());
        return true;
    }

    printf("send_one_batch: POST failed -> keeping %lu measurements in queue\n",
           (unsigned long)batch_count);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    return false;
}

static void try_send_buffered_measurements(void) {
    size_t initial_queue = queue_size();
    size_t total_sent_this_cycle = 0;

    if (initial_queue == 0) {
        printf("try_send_buffered_measurements: queue empty\n");
        return;
    }

    if (!wifi_is_connected()) {
        printf("try_send_buffered_measurements: Wi-Fi down, skipping send (queued=%lu dropped_total=%lu)\n",
               (unsigned long)queue_size(),
               (unsigned long)dropped_measurements);
        return;
    }

    size_t allowed_batch_attempts = 1;

    if (initial_queue > MAX_BATCH_SIZE * 3u) {
        allowed_batch_attempts = MAX_BATCH_REQUESTS_PER_CYCLE;
    } else if (initial_queue > MAX_BATCH_SIZE * 2u) {
        allowed_batch_attempts = (MAX_BATCH_REQUESTS_PER_CYCLE >= 3u) ? 3u : MAX_BATCH_REQUESTS_PER_CYCLE;
    } else if (initial_queue > MAX_BATCH_SIZE) {
        allowed_batch_attempts = (MAX_BATCH_REQUESTS_PER_CYCLE >= 2u) ? 2u : MAX_BATCH_REQUESTS_PER_CYCLE;
    }

    if (allowed_batch_attempts > MAX_BATCH_REQUESTS_PER_CYCLE) {
        allowed_batch_attempts = MAX_BATCH_REQUESTS_PER_CYCLE;
    }

    printf("try_send_buffered_measurements: start (queued=%lu, allowed_batch_attempts=%lu)\n",
           (unsigned long)initial_queue,
           (unsigned long)allowed_batch_attempts);

    for (size_t batch_no = 0; batch_no < allowed_batch_attempts; batch_no++) {
        if (queue_is_empty()) {
            printf("try_send_buffered_measurements: queue drained early after %lu batch attempts\n",
                   (unsigned long)batch_no);
            break;
        }

        size_t before_send = queue_size();
        size_t sent_count = 0;
        bool ok = send_one_batch(&sent_count);

        if (!ok) {
            printf("try_send_buffered_measurements: batch attempt %lu failed (queued=%lu)\n",
                   (unsigned long)(batch_no + 1u),
                   (unsigned long)queue_size());
            break;
        }

        if (sent_count == 0u) {
            printf("try_send_buffered_measurements: batch attempt %lu sent 0 items (stopping)\n",
                   (unsigned long)(batch_no + 1u));
            break;
        }

        total_sent_this_cycle += sent_count;

        printf("try_send_buffered_measurements: batch attempt %lu OK (sent=%lu, queue_before=%lu, queue_after=%lu)\n",
               (unsigned long)(batch_no + 1u),
               (unsigned long)sent_count,
               (unsigned long)before_send,
               (unsigned long)queue_size());

        if (queue_size() <= (MAX_BATCH_SIZE / 2u)) {
            printf("try_send_buffered_measurements: queue now small (%lu), stopping early\n",
                   (unsigned long)queue_size());
            break;
        }
    }

    printf("Cycle result: sent=%lu, queued=%lu, dropped_total=%lu\n",
           (unsigned long)total_sent_this_cycle,
           (unsigned long)queue_size(),
           (unsigned long)dropped_measurements);
}

/* =========================================================
 * Main
 * ========================================================= */

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("Pico buffered batch HTTP POST example with NTP starting...\n");

    
    if (cyw43_arch_init()) {
        printf("cyw43_arch_init failed\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();
    ntp_init_module();

 
/*
     * Initiera sensorn.
     *
     * Här använder vi autodetektering av adress:
     *   0x44 eller 0x45
     */
    status = sht30_init(&sensor,
                            APP_I2C_PORT,
                            APP_I2C_SDA_PIN,
                            APP_I2C_SCL_PIN,
                            APP_I2C_BAUDRATE,
                            SHT30_ADDR_AUTO);

    if (status != SHT30_OK) {
        printf("sht30_init failed: %s\n", sht30_strerror(status));

        /*
         * Om init misslyckas stannar vi här så felet blir tydligt.
         */
        while (true) {
            sleep_ms(1000);
        }
    }

    printf("SHT30 initialized successfully at address 0x%02X\n", sensor.addr);

    
    while (true) {
      
      if (ensure_wifi_connected()) {
        ntp_poll();
        
	if (!ntp_time_is_valid()) {
	  printf("Waiting for initial NTP sync...\n");
	  if (ntp_wait_for_initial_sync(NTP_STARTUP_WAIT_MS)) {
	    printf("Initial NTP sync OK\n");
      } else {
	    printf("Initial NTP sync timeout, continuing with time_valid=false\n");
                }
	}
      } else {
        printf("Wi-Fi unavailable, will keep buffering measurements\n");
        sleep_ms(RETRY_DELAY_MS);
      }
      
      measurement_t m = create_measurement();
      queue_push(&m);
      
      printf("Queued measurement seq=%lu time_valid=%s timestamp_utc=%lu (queue depth=%lu)\n",
             (unsigned long)m.seq,
             m.time_valid ? "true" : "false",
             (unsigned long)m.timestamp_utc,
             (unsigned long)queue_size());
      
      try_send_buffered_measurements();
      ntp_poll();
      sleep_with_housekeeping(SEND_INTERVAL_MS);
    }
    
    cyw43_arch_deinit();
    return 0;
}
