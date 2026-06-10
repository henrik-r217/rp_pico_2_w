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


#define SERVER_HOST "192.168.1.211"
#define SERVER_PORT 5000
#define SERVER_PATH "/measurements"

#define API_KEY "byt-till-en-hemlig-nyckel"


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


static err_t http_client_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
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


static err_t http_client_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    http_client_t *client = (http_client_t *)arg;

    if (err != ERR_OK) {
        printf("Connect failed: %d\n", err);
        client->complete = true;
        client->success = false;
        return err;
    }

    printf("Connected to server\n");

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

    err_t write_err = tcp_write(pcb, request, request_len, TCP_WRITE_FLAG_COPY);
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


static void my_dns_found_callback(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
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

        err_t err = dns_gethostbyname(SERVER_HOST,&addr,my_dns_found_callback,client);

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
