/*
 * main_low_power.c
 *
 * Low-power oriented Raspberry Pi Pico W / Pico 2 W telemetry client.
 *
 * Purpose:
 *   - Reduce energy consumption when running from 3x AAA batteries.
 *   - Measure often, upload less often.
 *   - Keep Wi-Fi OFF most of the time.
 *   - Keep the SHT30 sensor powered only during measurement if sensor power switching is available.
 *   - Keep retry timeouts short to avoid draining batteries when Wi-Fi/server is unavailable.
 *
 * Notes:
 *   - This file keeps the same JSON structure as the MySQL endpoint server:
 *       device_id, queued_total, dropped_total, time_valid, measurements[]
 *   - voltage is intentionally NOT included.
 *   - Replace read_sht30_temperature_humidity() with your real SHT30 driver call.
 *   - For maximum sleep current reduction on Pico W/Pico 2 W, hardware matters a lot:
 *       remove/avoid LEDs, use a low-Iq regulator, and optionally power-gate the sensor.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/clocks.h"



//#include "hardware/timer.h"



//#include "pico/sleep.h"
#include "hardware/structs/rosc.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/clocks.h"
//#include "hardware/rtc.h"

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#include "sht30.h"

#ifndef WIFI_SSID
#define WIFI_SSID "your-ssid"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your-password"
#endif

#ifndef API_KEY
#define API_KEY "default-dev-key"
#endif
/*
 * GPIO device-id support for main_low_power.c
 *
 * Wiring for pico-01 with default active-low mapping:
 *   GPIO13 -> GND  = bit0 = 1
 *   GPIO14 open    = bit1 = 0
 *   GPIO15 open    = bit2 = 0
 * Result: binary 001 -> pico-01
 */
#ifndef DEVICE_ID_PREFIX
#define DEVICE_ID_PREFIX "pico"
#endif

#define DEVICE_ID_GPIO_BIT0 13
#define DEVICE_ID_GPIO_BIT1 14
#define DEVICE_ID_GPIO_BIT2 15
#define DEVICE_ID_BITS_ACTIVE_LOW 1
#define DEVICE_ID_STR_LEN 16

static char g_device_id[DEVICE_ID_STR_LEN] = "pico-00";


/*
#ifndef DEVICE_ID
#define DEVICE_ID "pico-01"
#endif
*/

/* =========================================================
 * Server configuration
 * ========================================================= */

#ifndef SERVER_HOST
#define SERVER_HOST "192.168.1.99"
#endif

#ifndef SERVER_PORT
#define SERVER_PORT 5000
#endif

#ifndef SERVER_PATH
#define SERVER_PATH "/measurements"
#endif

/* =========================================================
 * Low-power policy
 * =========================================================
 * Recommended battery profile:
 *   - Measure every 60 seconds.
 *   - Upload every 10 minutes.
 *   - Do at most 2 HTTP batch attempts per upload wake.
 *   - Short Wi-Fi timeout; if unavailable, keep buffering and go back to sleep.
 */

#define MEASUREMENT_INTERVAL_MS              (60u * 1000u)
#define UPLOAD_INTERVAL_MS                   (30u * 60u * 1000u)
#define NTP_SYNC_INTERVAL_MS                 (5u * 60u * 60u * 1000u)

//#define WIFI_CONNECT_TIMEOUT_MS              30000u
#define WIFI_CONNECT_TIMEOUT_MS              8000u
#define HTTP_TIMEOUT_MS                      4000u
#define RETRY_AFTER_WIFI_FAIL_MS             (5u * 60u * 1000u)

#define MEASUREMENT_QUEUE_CAPACITY           128u
#define MAX_BATCH_SIZE                       32u
#define MAX_BATCH_REQUESTS_PER_UPLOAD_WAKE   1u
#define PAYLOAD_BUFFER_SIZE                  2048u

/* Lower CPU clock reduces active current during non-radio work.
 * Keep 48 MHz as a conservative USB-compatible-ish clock target.
 */
#define LOW_POWER_SYS_CLOCK_KHZ              48000u
//#define LOW_POWER_SYS_CLOCK_KHZ              125000u
#define WIFI_SYS_CLOCK_KHZ                   125000u

/* Set to 1 to keep stdio logs. Set to 0 for production battery operation. */
#define DEBUG_LOG                            1

/* Optional GPIO to power-gate the SHT30 sensor through a MOSFET/load switch.
 * Set to -1 if the sensor is always powered.
 * Example: #define SENSOR_POWER_GPIO 15
 */
#define SENSOR_POWER_GPIO                    (-1)
#define SENSOR_WARMUP_MS                     15u

/* NTP */
#define NTP_SERVER                           "pool.ntp.org"
#define NTP_PORT                             123
#define NTP_MSG_LEN                          48
#define NTP_DELTA                            2208988800UL
#define NTP_RESEND_MS                        8000u

#define APP_I2C_PORT     i2c0
#define APP_I2C_SDA_PIN  4
#define APP_I2C_SCL_PIN  5
#define APP_I2C_BAUDRATE 100000


#if DEBUG_LOG
#define LOG_PRINTF(...) printf(__VA_ARGS__)
#else
#define LOG_PRINTF(...) do { } while (0)
#endif

/* =========================================================
 * Types
 * ========================================================= */

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

/* =========================================================
 * Globals
 * ========================================================= */
static sht30_t sensor;
static measurement_t measurement_queue[MEASUREMENT_QUEUE_CAPACITY];
static size_t queue_head = 0;
static size_t queue_tail = 0;
static size_t queue_count = 0;
static uint32_t next_sequence_number = 1;
static uint32_t dropped_measurements = 0;
static ntp_state_t g_ntp;

static bool g_wifi_stack_initialized = false;
static uint64_t g_next_measurement_ms = 0;
static uint64_t g_next_upload_ms = 0;
static uint64_t g_next_ntp_sync_ms = 0;

/* =========================================================
 * Forward declarations
 * ========================================================= */

static uint64_t now_ms(void);
static bool wifi_stack_on(void);
static bool wifi_is_connected(void);
static bool wifi_on_and_connect(void);
static void wifi_off(void);
static void low_power_wait_until(uint64_t target_ms);

static void ntp_init_state(void);
static void ntp_start_if_needed(void);
static void ntp_poll(void);
static bool ntp_time_is_valid(void);
static uint32_t ntp_now_utc(void);

static measurement_t create_measurement(void);
static bool build_batch_payload(char *out, size_t out_size, size_t *batched_count);
static bool send_one_batch(size_t *sent_count);
static void try_upload_buffered_measurements(void);

/* =========================================================
 * Utility
 * ========================================================= */

static uint64_t now_ms(void) {
    return (uint64_t)to_ms_since_boot(get_absolute_time());
}

static void configure_low_power_clock(void) {
    /* If this ever fails on a specific board/build, comment it out first.
     * Wi-Fi operations are generally OK at this level, but test your setup.
     */
    bool ok = set_sys_clock_khz(LOW_POWER_SYS_CLOCK_KHZ, true);
    LOG_PRINTF("Clock set to %u kHz: %s\n", LOW_POWER_SYS_CLOCK_KHZ, ok ? "OK" : "FAILED");
}
static void low_power_wait_until(uint64_t target_ms) {
    while (true) {
        uint64_t ms = now_ms();

        if (ms >= target_ms) {
            return;
        }

        uint64_t remaining = target_ms - ms;

        /*
         * Long sleeps in larger chunks.
         * This avoids waking every second unnecessarily.
         */
        if (remaining > 60000u) {
            sleep_ms(60000u);
        } else if (remaining > 5000u) {
            sleep_ms((uint32_t)(remaining - 1000u));
        } else {
            sleep_ms((uint32_t)remaining);
        }
    }
}

static void set_clock_low_power(void) {
    set_sys_clock_khz(48000, true);
}

static void set_clock_wifi_mode(void) {
    set_sys_clock_khz(125000, true);
}












/* =========================================================
 * Init GPIO
 * ========================================================= */

static void device_id_gpio_init(void) {
    const uint pins[3] = {
        DEVICE_ID_GPIO_BIT0,
        DEVICE_ID_GPIO_BIT1,
        DEVICE_ID_GPIO_BIT2
    };

    for (size_t i = 0; i < 3; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }

    sleep_ms(100);
}
static uint8_t read_device_id_bits(void) {
    uint8_t raw0 = gpio_get(DEVICE_ID_GPIO_BIT0) ? 1u : 0u;
    uint8_t raw1 = gpio_get(DEVICE_ID_GPIO_BIT1) ? 1u : 0u;
    uint8_t raw2 = gpio_get(DEVICE_ID_GPIO_BIT2) ? 1u : 0u;

#if DEVICE_ID_BITS_ACTIVE_LOW
    uint8_t b0 = raw0 ? 0u : 1u;
    uint8_t b1 = raw1 ? 0u : 1u;
    uint8_t b2 = raw2 ? 0u : 1u;
#else
    uint8_t b0 = raw0;
    uint8_t b1 = raw1;
    uint8_t b2 = raw2;
#endif

    return (uint8_t)((b2 << 2u) | (b1 << 1u) | b0);
}


static void build_device_id_from_gpio(char *out, size_t out_len) {
    uint8_t id = read_device_id_bits();
    snprintf(out, out_len, "%s-%02u", DEVICE_ID_PREFIX, (unsigned)id);
}






/* =========================================================
 * Optional sensor power gating
 * ========================================================= */

static void sensor_power_init(void) {
#if SENSOR_POWER_GPIO >= 0
    gpio_init(SENSOR_POWER_GPIO);
    gpio_set_dir(SENSOR_POWER_GPIO, GPIO_OUT);
    gpio_put(SENSOR_POWER_GPIO, 0);
#endif
}

static void sensor_power_on(void) {
#if SENSOR_POWER_GPIO >= 0
    gpio_put(SENSOR_POWER_GPIO, 1);
    sleep_ms(SENSOR_WARMUP_MS);
#endif
}

static void sensor_power_off(void) {
#if SENSOR_POWER_GPIO >= 0
    gpio_put(SENSOR_POWER_GPIO, 0);
#endif
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
        LOG_PRINTF("Queue full -> dropped oldest measurement (total dropped=%lu)\n",
                   (unsigned long)dropped_measurements);
    }

    measurement_queue[queue_tail] = *m;
    queue_tail = (queue_tail + 1u) % MEASUREMENT_QUEUE_CAPACITY;
    queue_count++;
}

/* =========================================================
 * Wi-Fi power management
 * ========================================================= */

static bool wifi_stack_on(void) {
    return g_wifi_stack_initialized;
}

static bool wifi_is_connected(void) {
    if (!wifi_stack_on()) {
        return false;
    }
    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    return status == CYW43_LINK_UP;
}

static bool wifi_on_and_connect(void) {
    if (!g_wifi_stack_initialized) {

        int rc = cyw43_arch_init();
        if (rc != 0) {
            LOG_PRINTF("cyw43_arch_init failed: %d\n", rc);

            /*
             * KRITISKT:
             * Säkerställ att vi inte tror att stacken är OK
             */
            g_wifi_stack_initialized = false;

            return false;
        }

        g_wifi_stack_initialized = true;
        cyw43_arch_enable_sta_mode();

        cyw43_wifi_pm(&cyw43_state, CYW43_AGGRESSIVE_PM);
    }

    if (wifi_is_connected()) {
        return true;
    }

    LOG_PRINTF("Connecting to Wi-Fi SSID: %s\n", WIFI_SSID);

    int result = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID,
        WIFI_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK,
        WIFI_CONNECT_TIMEOUT_MS
    );

    if (result != 0) {
        LOG_PRINTF("Wi-Fi connect failed: %d\n", result);

        /*
         * 🔥 VIKTIGT: resetta stacken vid fel
         */
        cyw43_arch_deinit();
        g_wifi_stack_initialized = false;

        return false;
    }

    LOG_PRINTF("Wi-Fi connected\n");
    return true;
}


/*
static void wifi_off(void) {
    if (!g_wifi_stack_initialized) {
        return;
    }

    LOG_PRINTF("Turning Wi-Fi stack off\n");
    cyw43_arch_deinit();
    g_wifi_stack_initialized = false;

*/

/* After cyw43_arch_deinit(), any lwIP PCB is gone. Recreate NTP state next time. */

/*  memset(&g_ntp, 0, sizeof(g_ntp));
}
*/
static void wifi_off(void) {
    if (!g_wifi_stack_initialized) {
        return;
    }

    LOG_PRINTF("Turning Wi-Fi stack off (keep time)\n");

    cyw43_arch_deinit();
    g_wifi_stack_initialized = false;

    /*
     * ✅ Behåll NTP state:
     *  - epoch_at_sync
     *  - ms_at_sync
     *  - time_valid
     */
}





/* =========================================================
 * NTP
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
  g_ntp.next_sync_ms = now_ms() + NTP_SYNC_INTERVAL_MS;
  g_next_ntp_sync_ms = g_ntp.next_sync_ms;

  LOG_PRINTF("NTP synced: epoch=%lu\n", (unsigned long)epoch_utc);
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
            ntp_mark_synced(seconds_since_1900 - NTP_DELTA);
        } else {
            LOG_PRINTF("NTP invalid reply (mode=%u stratum=%u)\n", mode, stratum);
            g_ntp.request_in_flight = false;
            g_ntp.next_sync_ms = now_ms() + NTP_RESEND_MS;
        }
    }

    pbuf_free(p);
}

static void ntp_send_request(void) {
    if (!wifi_is_connected()) {
        return;
    }

    if (!g_ntp.udp_pcb) {
        cyw43_arch_lwip_begin();
        g_ntp.udp_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
        if (g_ntp.udp_pcb) {
            udp_recv(g_ntp.udp_pcb, ntp_recv_cb, NULL);
        }
        cyw43_arch_lwip_end();
    }

    if (!g_ntp.udp_pcb) {
        LOG_PRINTF("NTP: failed to create UDP PCB\n");
        return;
    }

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
    if (!p) {
        LOG_PRINTF("NTP: pbuf_alloc failed\n");
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
        LOG_PRINTF("NTP udp_sendto failed: %d\n", err);
        return;
    }

    g_ntp.request_in_flight = true;
    g_ntp.request_deadline_ms = now_ms() + NTP_RESEND_MS;
    LOG_PRINTF("NTP request sent\n");
}

static void ntp_dns_found_cb(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
    (void)hostname;
    (void)arg;
    g_ntp.dns_in_progress = false;

    if (!ipaddr) {
        LOG_PRINTF("NTP DNS failed\n");
        g_ntp.next_sync_ms = now_ms() + NTP_RESEND_MS;
        return;
    }

    g_ntp.server_addr = *ipaddr;
    ntp_send_request();
}

static void ntp_init_state(void) {
    memset(&g_ntp, 0, sizeof(g_ntp));
    g_ntp.next_sync_ms = now_ms();
}

static void ntp_start_if_needed(void) {
    if (!wifi_is_connected()) {
        return;
    }

    if (g_ntp.time_valid && now_ms() < g_next_ntp_sync_ms) {
        return;
    }

    if (g_ntp.dns_in_progress || g_ntp.request_in_flight) {
        return;
    }

    cyw43_arch_lwip_begin();
    ip_addr_t tmp_addr;
    err_t err = dns_gethostbyname(NTP_SERVER, &tmp_addr, ntp_dns_found_cb, NULL);
    cyw43_arch_lwip_end();

    if (err == ERR_OK) {
        g_ntp.server_addr = tmp_addr;
        ntp_send_request();
    } else if (err == ERR_INPROGRESS) {
        g_ntp.dns_in_progress = true;
    } else {
        LOG_PRINTF("NTP dns_gethostbyname failed: %d\n", err);
        g_ntp.next_sync_ms = now_ms() + NTP_RESEND_MS;
    }
}

static void ntp_poll(void) {
    if (!wifi_is_connected()) {
        return;
    }

    uint64_t ms = now_ms();
    if (g_ntp.request_in_flight && ms >= g_ntp.request_deadline_ms) {
        LOG_PRINTF("NTP timeout\n");
        g_ntp.request_in_flight = false;
        g_ntp.next_sync_ms = ms + NTP_RESEND_MS;
    }
}

/* =========================================================
 * Sensor reading
 * ========================================================= */

static bool read_sht30_temperature_humidity(float *temperature, float *humidity) {
    /* TODO: Replace this stub with real SHT30 I2C reading.
     * Recommendation for lower power:
     *   - single-shot mode
     *   - low repeatability if acceptable
     *   - no periodic mode
     *   - power-gate sensor if your board supports it
     */
    *temperature = 22.50f;
    *humidity = 45.00f;
    return true;
}

static measurement_t create_measurement(void) {
    measurement_t m;
    memset(&m, 0, sizeof(m));
    float temperature_c = 0.0f;
    float humidity_rh   = 0.0f;
    m.seq = next_sequence_number++;
    m.uptime_s = (uint32_t)(now_ms() / 1000u);
    m.timestamp_utc = ntp_now_utc();
    m.time_valid = ntp_time_is_valid();

    sensor_power_on();
    //bool ok = read_sht30_temperature_humidity(&m.temperature, &m.humidity);

    int status = sht30_read(&sensor, &temperature_c, &humidity_rh);
    if (status == SHT30_OK)
      {
        LOG_PRINTF("Temperature: %.2f C, Humidity: %.2f %%RH\n",
                  temperature_c,
                    humidity_rh);
        
        m.temperature = temperature_c;
        m.humidity = humidity_rh;

      }
    else
      {
        LOG_PRINTF("sht30_read failed: %s\n", sht30_strerror(status));
        m.temperature = 0.0f;
        m.humidity = 0.0f;

      }
    
    sensor_power_off();
    
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

    if (n < 0 || (size_t)n >= (buf_size - *used)) {
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
            g_device_id,
            (unsigned long)queue_size(),
            (unsigned long)dropped_measurements,
            ntp_time_is_valid() ? "true" : "false")) {
        return false;
    }

    size_t max_items = queue_size();
    if (max_items > MAX_BATCH_SIZE) {
        max_items = MAX_BATCH_SIZE;
    }

    for (size_t i = 0; i < max_items; i++) {
        measurement_t *m = queue_get(i);
        if (!m) {
            break;
        }

        char item_buf[224];
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
            break;
        }

        size_t reserve_tail = 3;
        if (used + (size_t)item_len + reserve_tail >= out_size) {
            break;
        }

        memcpy(out + used, item_buf, (size_t)item_len);
        used += (size_t)item_len;
        out[used] = '\0';
        count++;
    }

    if (count == 0u) {
        return false;
    }

    if (!append_to_buffer(out, out_size, &used, "]}")) {
        return false;
    }

    *batched_count = count;
    return true;
}

/* =========================================================
 * HTTP client
 * ========================================================= */

static void http_client_reset(http_client_t *client) {
    memset(client, 0, sizeof(*client));
}

static void http_client_abort(http_client_t *client) {
    if (client->pcb) {
        tcp_arg(client->pcb, NULL);
        tcp_sent(client->pcb, NULL);
        tcp_recv(client->pcb, NULL);
        tcp_err(client->pcb, NULL);
        tcp_abort(client->pcb);
        client->pcb = NULL;
    }
}

static void http_client_close(http_client_t *client) {
    if (client->pcb) {
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
    LOG_PRINTF("HTTP TCP error: %d\n", err);
    if (client) {
        client->pcb = NULL;
        client->complete = true;
        client->success = false;
    }
}

static err_t http_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_client_t *client = (http_client_t *)arg;
    if (!client) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }

    if (p == NULL) {
        client->complete = true;
        client->success = client->got_http_status && client->http_status >= 200 && client->http_status < 300;
        http_client_close(client);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        client->complete = true;
        client->success = false;
        return err;
    }

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        append_response_head(client, q->payload, q->len);
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t http_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    http_client_t *client = (http_client_t *)arg;
    if (!client) return ERR_ARG;

    if (err != ERR_OK) {
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
        client->complete = true;
        client->success = false;
        return ERR_BUF;
    }

    err_t werr = tcp_write(pcb, request, (u16_t)request_len, TCP_WRITE_FLAG_COPY);
    if (werr != ERR_OK) {
        client->complete = true;
        client->success = false;
        return werr;
    }

    err_t oerr = tcp_output(pcb);
    if (oerr != ERR_OK) {
        client->complete = true;
        client->success = false;
        return oerr;
    }

    return ERR_OK;
}

static void http_dns_found_cb(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
    http_client_t *client = (http_client_t *)arg;
    if (!client) return;

    if (!ipaddr) {
        LOG_PRINTF("HTTP DNS lookup failed for %s\n", hostname);
        client->complete = true;
        client->success = false;
        return;
    }

    client->remote_addr = *ipaddr;
    cyw43_arch_lwip_begin();
    err_t err = tcp_connect(client->pcb, &client->remote_addr, SERVER_PORT, http_connected_cb);
    cyw43_arch_lwip_end();

    if (err != ERR_OK) {
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
    if (!client->pcb) {
        client->complete = true;
        client->success = false;
        return;
    }

    tcp_arg(client->pcb, client);
    tcp_recv(client->pcb, http_recv_cb);
    tcp_err(client->pcb, http_err_cb);

    ip_addr_t addr;
    if (ipaddr_aton(SERVER_HOST, &addr)) {
        client->remote_addr = addr;
        cyw43_arch_lwip_begin();
        err_t err = tcp_connect(client->pcb, &client->remote_addr, SERVER_PORT, http_connected_cb);
        cyw43_arch_lwip_end();
        if (err != ERR_OK) {
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
            client->complete = true;
            client->success = false;
            http_client_abort(client);
        }
    } else if (err != ERR_INPROGRESS) {
        client->complete = true;
        client->success = false;
        http_client_abort(client);
    }
}

static bool wait_for_http_completion(http_client_t *client, uint32_t timeout_ms) {
    uint64_t deadline = now_ms() + timeout_ms;
    while (!client->complete && now_ms() < deadline) {
        sleep_ms(50);
        ntp_poll();
    }

    if (!client->complete) {
        LOG_PRINTF("HTTP timeout after %lu ms\n", (unsigned long)timeout_ms);
        http_client_abort(client);
        client->complete = true;
        client->success = false;
    }

    return client->success;
}

/* =========================================================
 * Upload logic
 * ========================================================= */

static bool send_one_batch(size_t *sent_count) {
    *sent_count = 0;

    if (queue_is_empty()) {
        return true;
    }

    char payload[PAYLOAD_BUFFER_SIZE];
    size_t batch_count = 0;
    if (!build_batch_payload(payload, sizeof(payload), &batch_count)) {
        LOG_PRINTF("Failed to build batch payload\n");
        return false;
    }

    http_client_t client;
    start_http_post(&client, payload);
    bool ok = wait_for_http_completion(&client, HTTP_TIMEOUT_MS);

    if (ok) {
        queue_pop_n(batch_count);
        *sent_count = batch_count;
        LOG_PRINTF("POST OK -> removed %lu measurements from queue\n", (unsigned long)batch_count);
        return true;
    }

    LOG_PRINTF("POST failed -> keeping batch in queue\n");
    return false;
}

static void try_upload_buffered_measurements(void) {
    if (queue_is_empty()) {
        return;
    }
    set_clock_wifi_mode();
    if (!wifi_on_and_connect()) {
        g_next_upload_ms = now_ms() + RETRY_AFTER_WIFI_FAIL_MS;
        wifi_off();
        set_clock_low_power();
        return;
    }

    ntp_start_if_needed();
    uint64_t ntp_deadline = now_ms() + 2500u;
    while (!ntp_time_is_valid() && now_ms() < ntp_deadline) {
        ntp_poll();
        sleep_ms(50);
    }

    size_t total_sent = 0;
    for (size_t i = 0; i < MAX_BATCH_REQUESTS_PER_UPLOAD_WAKE; i++) {
        if (queue_is_empty()) {
            break;
        }

        size_t sent_count = 0;
        bool ok = send_one_batch(&sent_count);
        if (!ok || sent_count == 0u) {
            break;
        }
        total_sent += sent_count;
    }

    LOG_PRINTF("Upload result: sent=%lu queued=%lu dropped_total=%lu\n",
               (unsigned long)total_sent,
               (unsigned long)queue_size(),
               (unsigned long)dropped_measurements);

    g_next_upload_ms = now_ms() + UPLOAD_INTERVAL_MS;
    wifi_off();
    set_clock_low_power();
}

/* =========================================================
 * Main low-power scheduler
 * ========================================================= */

int main(void) {
#if DEBUG_LOG
    stdio_init_all();
    sleep_ms(2500);
#endif
  
    
    device_id_gpio_init();
    build_device_id_from_gpio(g_device_id, sizeof(g_device_id));
    LOG_PRINTF("Device ID from GPIO13/14/15: %s\n", g_device_id);
    LOG_PRINTF("Pico low-power telemetry client starting...\n");

    configure_low_power_clock();
    sensor_power_init();
    ntp_init_state();

    sensor_power_on();
/*
     * Initiera sensorn.
     *
     * Här använder vi autodetektering av adress:
     *   0x44 eller 0x45
     */
  int  status = sht30_init(&sensor,
                            APP_I2C_PORT,
                            APP_I2C_SDA_PIN,
                            APP_I2C_SCL_PIN,
                            APP_I2C_BAUDRATE,
                            SHT30_ADDR_AUTO);

    if (status != SHT30_OK) {
        LOG_PRINTF("sht30_init failed: %s\n", sht30_strerror(status));

        /*
         * Om init misslyckas stannar vi här så felet blir tydligt.
         */
        //        while (true) {
        //  sleep_ms(1000);
        // }
    }
    
    LOG_PRINTF("SHT30 initialized successfully at address 0x%02X\n", sensor.addr);
















    
    uint64_t start = now_ms();
    g_next_measurement_ms = start;
    g_next_upload_ms = start + 5000u;     /* Upload soon after first samples */
    g_next_ntp_sync_ms = start;           /* Sync on first upload wake */

    while (true) {
        uint64_t ms = now_ms();

        if (ms >= g_next_measurement_ms) {
            measurement_t m = create_measurement();
            queue_push(&m);
            LOG_PRINTF("Queued seq=%lu time_valid=%s timestamp_utc=%lu temp=%.2f hum=%.2f queue=%lu\n",
                       (unsigned long)m.seq,
                       m.time_valid ? "true" : "false",
                       (unsigned long)m.timestamp_utc,
                       m.temperature,
                       m.humidity,
                       (unsigned long)queue_size());

            g_next_measurement_ms = ms + MEASUREMENT_INTERVAL_MS;
        }

        /* Upload only at scheduled upload time, or earlier if queue is nearly full. */
        bool queue_pressure = queue_size() >= (MEASUREMENT_QUEUE_CAPACITY * 3u / 4u);
        if ((ms >= g_next_upload_ms || queue_pressure) && !queue_is_empty()) {
            try_upload_buffered_measurements();
        }

        uint64_t next_event = g_next_measurement_ms;
        if (!queue_is_empty() && g_next_upload_ms < next_event) {
            next_event = g_next_upload_ms;
        }

        low_power_wait_until(next_event);
    }

    return 0;
}
