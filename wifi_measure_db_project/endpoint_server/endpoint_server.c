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
#include <cjson/cJSON.h>


#define SERVER_PORT 5000
#define MAX_HEADER_SIZE 8192
#define MAX_BODY_SIZE (1024 * 1024)

#define API_KEY_ENV_NAME "PICO_API_KEY"
#define DB_FILE "pico_measurements.db"


static sqlite3 *db = NULL;


/* =========================================================
 * HTTP response helper
 * ========================================================= */

static void send_response(
    int client_fd,
    int status_code,
    const char *status_text,
    const char *body
) {
    char response[1024];

    int body_len = body ? (int)strlen(body) : 0;

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


/* =========================================================
 * SQLite helpers
 * ========================================================= */

static int exec_sql(const char *sql) {
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg ? err_msg : "unknown");
        sqlite3_free(err_msg);
        return -1;
    }

    return 0;
}

static int init_database(void) {
    int rc = sqlite3_open(DB_FILE, &db);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    if (exec_sql("PRAGMA foreign_keys = ON;") != 0) {
        return -1;
    }

    if (exec_sql(
        "CREATE TABLE IF NOT EXISTS batches ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "server_received_at INTEGER NOT NULL,"
        "remote_ip TEXT,"
        "device_id TEXT,"
        "queued_total INTEGER NOT NULL DEFAULT 0,"
        "dropped_total INTEGER NOT NULL DEFAULT 0,"
        "time_valid INTEGER NOT NULL DEFAULT 0,"
        "raw_json TEXT NOT NULL"
        ");"
    ) != 0) {
        return -1;
    }

    if (exec_sql(
        "CREATE TABLE IF NOT EXISTS measurements ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "batch_id INTEGER NOT NULL,"
        "server_received_at INTEGER NOT NULL,"
        "device_id TEXT NOT NULL,"
        "seq INTEGER NOT NULL,"
        "device_timestamp_utc INTEGER NOT NULL DEFAULT 0,"
        "time_valid INTEGER NOT NULL DEFAULT 0,"
        "uptime_s INTEGER NOT NULL DEFAULT 0,"
        "temperature REAL,"
        "humidity REAL,"
        "UNIQUE(device_id, seq),"
        "FOREIGN KEY(batch_id) REFERENCES batches(id)"
        ");"
    ) != 0) {
        return -1;
    }

    /* Indexes */
    if (exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_measurements_device_timestamp "
        "ON measurements(device_id, device_timestamp_utc);"
    ) != 0) {
        return -1;
    }

    if (exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_measurements_server_received "
        "ON measurements(server_received_at);"
    ) != 0) {
        return -1;
    }

    if (exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_batches_server_received "
        "ON batches(server_received_at);"
    ) != 0) {
        return -1;
    }

    if (exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_batches_device_id "
        "ON batches(device_id);"
    ) != 0) {
        return -1;
    }

    if (exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_batches_device_received "
        "ON batches(device_id, server_received_at);"
    ) != 0) {
        return -1;
    }

    return 0;
}

static int begin_transaction(void) {
    return exec_sql("BEGIN IMMEDIATE TRANSACTION;");
}

static int commit_transaction(void) {
    return exec_sql("COMMIT;");
}

static void rollback_transaction(void) {
    exec_sql("ROLLBACK;");
}

static int insert_batch_raw(
    time_t server_received_at,
    const char *remote_ip,
    const char *device_id,
    sqlite3_int64 queued_total,
    sqlite3_int64 dropped_total,
    int time_valid,
    const char *raw_json,
    sqlite3_int64 *batch_id_out
) {
    const char *sql =
        "INSERT INTO batches ("
        "server_received_at, remote_ip, device_id, queued_total, dropped_total, time_valid, raw_json"
        ") VALUES (?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "insert_batch_raw prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)server_received_at);
    sqlite3_bind_text(stmt, 2, remote_ip ? remote_ip : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, device_id ? device_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, queued_total);
    sqlite3_bind_int64(stmt, 5, dropped_total);
    sqlite3_bind_int(stmt, 6, time_valid);
    sqlite3_bind_text(stmt, 7, raw_json, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "insert_batch_raw step failed: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_finalize(stmt);

    *batch_id_out = sqlite3_last_insert_rowid(db);
    return 0;
}

static int insert_measurement_row(
    sqlite3_int64 batch_id,
    time_t server_received_at,
    const char *device_id,
    sqlite3_int64 seq,
    sqlite3_int64 device_timestamp_utc,
    int time_valid,
    sqlite3_int64 uptime_s,
    double temperature,
    double humidity,
    bool *inserted
) {
    const char *sql =
        "INSERT OR IGNORE INTO measurements ("
        "batch_id, server_received_at, device_id, seq, "
        "device_timestamp_utc, time_valid, uptime_s, "
        "temperature, humidity"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "insert_measurement_row prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, batch_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)server_received_at);
    sqlite3_bind_text(stmt, 3, device_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, seq);
    sqlite3_bind_int64(stmt, 5, device_timestamp_utc);
    sqlite3_bind_int(stmt, 6, time_valid);
    sqlite3_bind_int64(stmt, 7, uptime_s);
    sqlite3_bind_double(stmt, 8, temperature);
    sqlite3_bind_double(stmt, 9, humidity);


    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "insert_measurement_row step failed: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_finalize(stmt);

    *inserted = (sqlite3_changes(db) > 0);
    return 0;
}


/* =========================================================
 * Header parsing helpers
 * ========================================================= */

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

        size_t line_len = (size_t)(line_end - p);

        if (
            line_len > name_len + 1 &&
            strncasecmp(p, header_name, name_len) == 0 &&
            p[name_len] == ':'
        ) {
            const char *v = p + name_len + 1;

            while (*v == ' ' || *v == '\t') {
                v++;
            }

            size_t v_len = (size_t)(line_end - v);
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


/* =========================================================
 * JSON helpers
 * ========================================================= */

static const char *json_get_string_or_null(const cJSON *obj, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return NULL;
}

static bool json_get_required_int64(
    const cJSON *obj,
    const char *name,
    sqlite3_int64 *out_value
) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }

    *out_value = (sqlite3_int64)item->valuedouble;
    return true;
}

static sqlite3_int64 json_get_int64_or_default(
    const cJSON *obj,
    const char *name,
    sqlite3_int64 def_value
) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);
    if (cJSON_IsNumber(item)) {
        return (sqlite3_int64)item->valuedouble;
    }
    return def_value;
}

static double json_get_double_or_default(
    const cJSON *obj,
    const char *name,
    double def_value
) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    return def_value;
}

static int json_get_bool_or_default(
    const cJSON *obj,
    const char *name,
    int def_value
) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) ? 1 : 0;
    }
    return def_value;
}


/* =========================================================
 * Batch root metadata helpers
 * ========================================================= */

static const char *extract_batch_device_id(const cJSON *root) {
    const char *device_id = json_get_string_or_null(root, "device_id");
    if (device_id) {
        return device_id;
    }

    const cJSON *measurements = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "measurements");
    if (cJSON_IsArray(measurements) && cJSON_GetArraySize((cJSON *)measurements) > 0) {
        const cJSON *first = cJSON_GetArrayItem((cJSON *)measurements, 0);
        if (cJSON_IsObject(first)) {
            device_id = json_get_string_or_null(first, "device_id");
            if (device_id) {
                return device_id;
            }

            device_id = json_get_string_or_null(first, "sensor_id");
            if (device_id) {
                return device_id;
            }
        }
    }

    device_id = json_get_string_or_null(root, "sensor_id");
    if (device_id) {
        return device_id;
    }

    return "unknown";
}


/* =========================================================
 * Ingest logic
 * ========================================================= */

static int ingest_single_measurement_object(
    const cJSON *root,
    const cJSON *measurement_obj,
    sqlite3_int64 batch_id,
    time_t server_received_at,
    int *inserted_count,
    int *duplicate_count
) {
    const char *device_id = json_get_string_or_null(measurement_obj, "sensor_id");
    if (!device_id) {
        device_id = json_get_string_or_null(measurement_obj, "device_id");
    }
    if (!device_id) {
        device_id = json_get_string_or_null(root, "device_id");
    }
    if (!device_id) {
        device_id = json_get_string_or_null(root, "sensor_id");
    }
    if (!device_id) {
        device_id = "unknown";
    }

    sqlite3_int64 seq = 0;
    if (!json_get_required_int64(measurement_obj, "seq", &seq)) {
        fprintf(stderr, "Measurement missing required seq\n");
        return -1;
    }

    sqlite3_int64 device_timestamp_utc =
        json_get_int64_or_default(measurement_obj, "timestamp_utc", 0);

    int time_valid =
        json_get_bool_or_default(
            measurement_obj,
            "time_valid",
            json_get_bool_or_default(root, "time_valid", 0)
        );

    sqlite3_int64 uptime_s =
        json_get_int64_or_default(measurement_obj, "uptime_s", 0);

    double temperature =
        json_get_double_or_default(measurement_obj, "temperature", 0.0);

    double humidity =
        json_get_double_or_default(measurement_obj, "humidity", 0.0);


    bool inserted = false;
    if (insert_measurement_row(
            batch_id,
            server_received_at,
            device_id,
            seq,
            device_timestamp_utc,
            time_valid,
            uptime_s,
            temperature,
            humidity,
            &inserted
        ) != 0) {
        return -1;
    }

    if (inserted) {
        (*inserted_count)++;
    } else {
        (*duplicate_count)++;
    }

    return 0;
}

static int ingest_payload(
    const char *body,
    const char *remote_ip,
    sqlite3_int64 *batch_id_out,
    int *inserted_count_out,
    int *duplicate_count_out
) {
    *batch_id_out = 0;
    *inserted_count_out = 0;
    *duplicate_count_out = 0;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        fprintf(stderr, "JSON parse failed\n");
        return -1;
    }

    time_t server_received_at = time(NULL);

    const char *batch_device_id = extract_batch_device_id(root);

    sqlite3_int64 queued_total =
        json_get_int64_or_default(root, "queued_total", 0);

    sqlite3_int64 dropped_total =
        json_get_int64_or_default(root, "dropped_total", 0);

    int batch_time_valid =
        json_get_bool_or_default(root, "time_valid", 0);

    if (begin_transaction() != 0) {
        cJSON_Delete(root);
        return -1;
    }

    sqlite3_int64 batch_id = 0;
    if (insert_batch_raw(
            server_received_at,
            remote_ip,
            batch_device_id,
            queued_total,
            dropped_total,
            batch_time_valid,
            body,
            &batch_id
        ) != 0) {
        rollback_transaction();
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *measurements = cJSON_GetObjectItemCaseSensitive(root, "measurements");

    if (cJSON_IsArray(measurements)) {
        int count = cJSON_GetArraySize((cJSON *)measurements);

        for (int i = 0; i < count; i++) {
            const cJSON *item = cJSON_GetArrayItem((cJSON *)measurements, i);

            if (!cJSON_IsObject(item)) {
                fprintf(stderr, "measurements[%d] is not an object\n", i);
                rollback_transaction();
                cJSON_Delete(root);
                return -1;
            }

            if (ingest_single_measurement_object(
                    root,
                    item,
                    batch_id,
                    server_received_at,
                    inserted_count_out,
                    duplicate_count_out
                ) != 0) {
                rollback_transaction();
                cJSON_Delete(root);
                return -1;
            }
        }
    }
    else if (cJSON_IsObject(root)) {
        if (ingest_single_measurement_object(
                root,
                root,
                batch_id,
                server_received_at,
                inserted_count_out,
                duplicate_count_out
            ) != 0) {
            rollback_transaction();
            cJSON_Delete(root);
            return -1;
        }
    }
    else {
        fprintf(stderr, "Unsupported JSON payload\n");
        rollback_transaction();
        cJSON_Delete(root);
        return -1;
    }

    if (commit_transaction() != 0) {
        rollback_transaction();
        cJSON_Delete(root);
        return -1;
    }

    cJSON_Delete(root);
    *batch_id_out = batch_id;
    return 0;
}


/* =========================================================
 * HTTP request handling
 * ========================================================= */

static void handle_client(int client_fd, const char *remote_ip) {
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
            header_end_offset = (int)(header_end - header_buffer) + 4;
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

    if (content_length > MAX_BODY_SIZE) {
        send_response(
            client_fd,
            413,
            "Payload Too Large",
            "{\"error\":\"payload too large\"}"
        );
        return;
    }

    char *body = calloc((size_t)content_length + 1, 1);
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
        memcpy(body, header_buffer + header_end_offset, (size_t)already_have);
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

    printf("Received payload from %s:\n%s\n",
           remote_ip ? remote_ip : "unknown",
           body);

    sqlite3_int64 batch_id = 0;
    int inserted_count = 0;
    int duplicate_count = 0;

    if (ingest_payload(
            body,
            remote_ip,
            &batch_id,
            &inserted_count,
            &duplicate_count
        ) != 0) {
        free(body);
        send_response(
            client_fd,
            400,
            "Bad Request",
            "{\"error\":\"invalid or unsupported JSON payload\"}"
        );
        return;
    }

    free(body);

    char ok_body[256];
    snprintf(
        ok_body,
        sizeof(ok_body),
        "{\"status\":\"ok\",\"batch_id\":%lld,"
        "\"inserted_measurements\":%d,"
        "\"duplicate_measurements\":%d}",
        (long long)batch_id,
        inserted_count,
        duplicate_count
    );

    send_response(client_fd, 200, "OK", ok_body);
}


/* =========================================================
 * Main
 * ========================================================= */

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
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
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

        const char *remote_ip = inet_ntoa(client_addr.sin_addr);

        printf("Connection from %s:%d\n",
               remote_ip,
               ntohs(client_addr.sin_port));

        handle_client(client_fd, remote_ip);
        close(client_fd);
    }

    close(server_fd);
    sqlite3_close(db);

    return 0;
}
