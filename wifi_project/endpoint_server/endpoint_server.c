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
#define API_KEY "byt-till-en-hemlig-nyckel"
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

    return strcmp(value, API_KEY) == 0;
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
