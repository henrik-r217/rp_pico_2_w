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

#include <mysql/mysql.h>
#include <cjson/cJSON.h>

#define SERVER_PORT 5000
#define MAX_HEADER_SIZE 8192
#define MAX_BODY_SIZE (1024 * 1024)

#define API_KEY_ENV_NAME "PICO_API_KEY"

#define DB_HOST_ENV     "MYSQL_HOST"
#define DB_PORT_ENV     "MYSQL_PORT"
#define DB_USER_ENV     "MYSQL_USER"
#define DB_PASS_ENV     "MYSQL_PASSWORD"
#define DB_NAME_ENV     "MYSQL_DATABASE"

static MYSQL *g_db = NULL;

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
 * DB helpers
 * ========================================================= */

static const char *env_or_default(const char *name, const char *def) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : def;
}

static unsigned int env_u32_or_default(const char *name, unsigned int def) {
    const char *v = getenv(name);
    if (!v || !v[0]) return def;
    unsigned long n = strtoul(v, NULL, 10);
    return (unsigned int)n;
}

static int db_exec(const char *sql) {
    if (mysql_query(g_db, sql) != 0) {
        fprintf(stderr, "MySQL query failed: %s\n", mysql_error(g_db));
        return -1;
    }
    return 0;
}

static int db_connect(void) {
    const char *host = env_or_default(DB_HOST_ENV, "127.0.0.1");
    const char *user = env_or_default(DB_USER_ENV, "pico_user");
    const char *pass = env_or_default(DB_PASS_ENV, "");
    const char *db   = env_or_default(DB_NAME_ENV, "pico_telemetry");
    unsigned int port = env_u32_or_default(DB_PORT_ENV, 3306);

    g_db = mysql_init(NULL);
    if (!g_db) {
        fprintf(stderr, "mysql_init failed\n");
        return -1;
    }

    if (!mysql_real_connect(g_db, host, user, pass, db, port, NULL, 0)) {
        fprintf(stderr, "mysql_real_connect failed: %s\n", mysql_error(g_db));
        return -1;
    }

    if (mysql_set_character_set(g_db, "utf8mb4") != 0) {
        fprintf(stderr, "mysql_set_character_set failed: %s\n", mysql_error(g_db));
        return -1;
    }

    return 0;
}

static int db_init_schema(void) {
    const char *sql1 =
        "CREATE TABLE IF NOT EXISTS batches ("
        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "server_received_at BIGINT NOT NULL,"
        "remote_ip VARCHAR(64) NULL,"
        "device_id VARCHAR(128) NULL,"
        "queued_total BIGINT NOT NULL DEFAULT 0,"
        "dropped_total BIGINT NOT NULL DEFAULT 0,"
        "time_valid TINYINT(1) NOT NULL DEFAULT 0,"
        "raw_json LONGTEXT NOT NULL,"
        "INDEX idx_batches_device_received (device_id, server_received_at),"
        "INDEX idx_batches_server_received (server_received_at)"
        ") ENGINE=InnoDB";

    const char *sql2 =
        "CREATE TABLE IF NOT EXISTS measurements ("
        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "batch_id BIGINT UNSIGNED NOT NULL,"
        "server_received_at BIGINT NOT NULL,"
        "device_id VARCHAR(128) NOT NULL,"
        "seq BIGINT NOT NULL,"
        "device_timestamp_utc BIGINT NOT NULL DEFAULT 0,"
        "time_valid TINYINT(1) NOT NULL DEFAULT 0,"
        "uptime_s BIGINT NOT NULL DEFAULT 0,"
        "temperature DOUBLE NULL,"
        "humidity DOUBLE NULL,"
        "UNIQUE KEY uq_device_seq (device_id, seq),"
        "INDEX idx_measurements_device_timestamp (device_id, device_timestamp_utc),"
        "INDEX idx_measurements_server_received (server_received_at),"
        "CONSTRAINT fk_measurements_batch "
        "FOREIGN KEY (batch_id) REFERENCES batches(id) "
        "ON DELETE CASCADE"
        ") ENGINE=InnoDB";

    if (db_exec(sql1) != 0) return -1;
    if (db_exec(sql2) != 0) return -1;
    return 0;
}

static int db_begin(void)    { return db_exec("START TRANSACTION"); }
static int db_commit(void)   { return db_exec("COMMIT"); }
static int db_rollback(void) { return db_exec("ROLLBACK"); }

/* =========================================================
 * Prepared statement helpers
 * ========================================================= */

static int insert_batch_row(
    time_t server_received_at,
    const char *remote_ip,
    const char *device_id,
    long long queued_total,
    long long dropped_total,
    int time_valid,
    const char *raw_json,
    unsigned long long *batch_id_out
) {
    const char *sql =
        "INSERT INTO batches "
        "(server_received_at, remote_ip, device_id, queued_total, dropped_total, time_valid, raw_json) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";

    MYSQL_STMT *stmt = mysql_stmt_init(g_db);
    if (!stmt) {
        fprintf(stderr, "mysql_stmt_init failed\n");
        return -1;
    }

    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) {
        fprintf(stderr, "insert_batch_row prepare failed: %s\n", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    MYSQL_BIND bind[7];
    memset(bind, 0, sizeof(bind));

    long long server_ts = (long long)server_received_at;
    unsigned long remote_ip_len = remote_ip ? (unsigned long)strlen(remote_ip) : 0;
    unsigned long device_id_len = device_id ? (unsigned long)strlen(device_id) : 0;
    unsigned long raw_json_len  = (unsigned long)strlen(raw_json);
    bool remote_ip_is_null = (remote_ip == NULL);
    bool device_id_is_null = (device_id == NULL);



    
    bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[0].buffer = &server_ts;

    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void *)remote_ip;
    bind[1].buffer_length = remote_ip_len;
    bind[1].length = &remote_ip_len;
    bind[1].is_null = &remote_ip_is_null;

    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void *)device_id;
    bind[2].buffer_length = device_id_len;
    bind[2].length = &device_id_len;
    bind[2].is_null = &device_id_is_null;

    bind[3].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[3].buffer = &queued_total;

    bind[4].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[4].buffer = &dropped_total;

    bind[5].buffer_type = MYSQL_TYPE_TINY;
    bind[5].buffer = &time_valid;

    bind[6].buffer_type = MYSQL_TYPE_STRING;
    bind[6].buffer = (void *)raw_json;
    bind[6].buffer_length = raw_json_len;
    bind[6].length = &raw_json_len;

    if (mysql_stmt_bind_param(stmt, bind) != 0) {
        fprintf(stderr, "insert_batch_row bind failed: %s\n", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    if (mysql_stmt_execute(stmt) != 0) {
        fprintf(stderr, "insert_batch_row execute failed: %s\n", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    *batch_id_out = mysql_insert_id(g_db);

    mysql_stmt_close(stmt);
    return 0;
}

static int insert_measurement_row(
    unsigned long long batch_id,
    time_t server_received_at,
    const char *device_id,
    long long seq,
    long long device_timestamp_utc,
    int time_valid,
    long long uptime_s,
    double temperature,
    double humidity,
    bool *inserted
) {
    const char *sql =
        "INSERT IGNORE INTO measurements "
        "(batch_id, server_received_at, device_id, seq, device_timestamp_utc, time_valid, uptime_s, temperature, humidity) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

    MYSQL_STMT *stmt = mysql_stmt_init(g_db);
    if (!stmt) {
        fprintf(stderr, "mysql_stmt_init failed\n");
        return -1;
    }

    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) {
        fprintf(stderr, "insert_measurement_row prepare failed: %s\n", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    MYSQL_BIND bind[10];
    memset(bind, 0, sizeof(bind));

    long long batch_id_ll = (long long)batch_id;
    long long server_ts = (long long)server_received_at;
    unsigned long device_id_len = (unsigned long)strlen(device_id);

    bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[0].buffer = &batch_id_ll;

    bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[1].buffer = &server_ts;

    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void *)device_id;
    bind[2].buffer_length = device_id_len;
    bind[2].length = &device_id_len;

    bind[3].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[3].buffer = &seq;

    bind[4].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[4].buffer = &device_timestamp_utc;

    bind[5].buffer_type = MYSQL_TYPE_TINY;
    bind[5].buffer = &time_valid;

    bind[6].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[6].buffer = &uptime_s;

    bind[7].buffer_type = MYSQL_TYPE_DOUBLE;
    bind[7].buffer = &temperature;

    bind[8].buffer_type = MYSQL_TYPE_DOUBLE;
    bind[8].buffer = &humidity;

    if (mysql_stmt_bind_param(stmt, bind) != 0) {
        fprintf(stderr, "insert_measurement_row bind failed: %s\n", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    if (mysql_stmt_execute(stmt) != 0) {
        fprintf(stderr, "insert_measurement_row execute failed: %s\n", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    *inserted = (mysql_stmt_affected_rows(stmt) > 0);

    mysql_stmt_close(stmt);
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
        if (!line_end) break;

        size_t line_len = (size_t)(line_end - p);

        if (
            line_len > name_len + 1 &&
            strncasecmp(p, header_name, name_len) == 0 &&
            p[name_len] == ':'
        ) {
            const char *v = p + name_len + 1;
            while (*v == ' ' || *v == '\t') v++;

            size_t v_len = (size_t)(line_end - v);
            if (v_len >= sizeof(value)) v_len = sizeof(value) - 1;

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
    if (!value) return -1;
    return atoi(value);
}

static bool api_key_is_valid(const char *headers) {
    char *value = find_header_value(headers, "X-API-Key");
    if (!value) return false;

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
    if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    return NULL;
}

static bool json_get_required_int64(const cJSON *obj, const char *name, long long *out_value) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);
    if (!cJSON_IsNumber(item)) return false;
    *out_value = (long long)item->valuedouble;
    return true;
}

static long long json_get_int64_or_default(const cJSON *obj, const char *name, long long def_value) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);
    if (cJSON_IsNumber(item)) return (long long)item->valuedouble;
    return def_value;
}

static double json_get_double_or_default(const cJSON *obj, const char *name, double def_value) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);
    if (cJSON_IsNumber(item)) return item->valuedouble;
    return def_value;
}

static int json_get_bool_or_default(const cJSON *obj, const char *name, int def_value) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item) ? 1 : 0;
    return def_value;
}

static const char *extract_batch_device_id(const cJSON *root) {
    const char *device_id = json_get_string_or_null(root, "device_id");
    if (device_id) return device_id;

    const cJSON *measurements = cJSON_GetObjectItemCaseSensitive((cJSON *)root, "measurements");
    if (cJSON_IsArray(measurements) && cJSON_GetArraySize((cJSON *)measurements) > 0) {
        const cJSON *first = cJSON_GetArrayItem((cJSON *)measurements, 0);
        if (cJSON_IsObject(first)) {
            device_id = json_get_string_or_null(first, "device_id");
            if (device_id) return device_id;

            device_id = json_get_string_or_null(first, "sensor_id");
            if (device_id) return device_id;
        }
    }

    device_id = json_get_string_or_null(root, "sensor_id");
    if (device_id) return device_id;

    return "unknown";
}

/* =========================================================
 * Ingest logic
 * ========================================================= */

static int ingest_single_measurement_object(
    const cJSON *root,
    const cJSON *measurement_obj,
    unsigned long long batch_id,
    time_t server_received_at,
    int *inserted_count,
    int *duplicate_count
) {
    const char *device_id = json_get_string_or_null(measurement_obj, "sensor_id");
    if (!device_id) device_id = json_get_string_or_null(measurement_obj, "device_id");
    if (!device_id) device_id = json_get_string_or_null(root, "device_id");
    if (!device_id) device_id = json_get_string_or_null(root, "sensor_id");
    if (!device_id) device_id = "unknown";

    long long seq = 0;
    if (!json_get_required_int64(measurement_obj, "seq", &seq)) {
        fprintf(stderr, "Measurement missing required seq\n");
        return -1;
    }

    long long device_timestamp_utc = json_get_int64_or_default(measurement_obj, "timestamp_utc", 0);
    int time_valid = json_get_bool_or_default(
        measurement_obj, "time_valid",
        json_get_bool_or_default(root, "time_valid", 0));
    long long uptime_s = json_get_int64_or_default(measurement_obj, "uptime_s", 0);
    double temperature = json_get_double_or_default(measurement_obj, "temperature", 0.0);
    double humidity = json_get_double_or_default(measurement_obj, "humidity", 0.0);

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

    if (inserted) (*inserted_count)++;
    else (*duplicate_count)++;

    return 0;
}

static int ingest_payload(
    const char *body,
    const char *remote_ip,
    unsigned long long *batch_id_out,
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

    long long queued_total = json_get_int64_or_default(root, "queued_total", 0);
    long long dropped_total = json_get_int64_or_default(root, "dropped_total", 0);
    int batch_time_valid = json_get_bool_or_default(root, "time_valid", 0);

    if (db_begin() != 0) {
        cJSON_Delete(root);
        return -1;
    }

    unsigned long long batch_id = 0;
    if (insert_batch_row(
            server_received_at,
            remote_ip,
            batch_device_id,
            queued_total,
            dropped_total,
            batch_time_valid,
            body,
            &batch_id
        ) != 0) {
        db_rollback();
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
                db_rollback();
                cJSON_Delete(root);
                return -1;
            }

            if (ingest_single_measurement_object(
                    root, item, batch_id, server_received_at,
                    inserted_count_out, duplicate_count_out) != 0) {
                db_rollback();
                cJSON_Delete(root);
                return -1;
            }
        }
    }
    else if (cJSON_IsObject(root)) {
        if (ingest_single_measurement_object(
                root, root, batch_id, server_received_at,
                inserted_count_out, duplicate_count_out) != 0) {
            db_rollback();
            cJSON_Delete(root);
            return -1;
        }
    }
    else {
        fprintf(stderr, "Unsupported JSON payload\n");
        db_rollback();
        cJSON_Delete(root);
        return -1;
    }

    if (db_commit() != 0) {
        db_rollback();
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
        int n = recv(client_fd, header_buffer + total_read, MAX_HEADER_SIZE - total_read, 0);
        if (n <= 0) {
            send_response(client_fd, 400, "Bad Request", "{\"error\":\"failed to read request\"}");
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
        send_response(client_fd, 413, "Payload Too Large", "{\"error\":\"headers too large\"}");
        return;
    }

    if (!request_line_is_valid(header_buffer)) {
        send_response(client_fd, 404, "Not Found", "{\"error\":\"use POST /measurements\"}");
        return;
    }

    if (!api_key_is_valid(header_buffer)) {
        send_response(client_fd, 401, "Unauthorized", "{\"error\":\"invalid api key\"}");
        return;
    }

    int content_length = get_content_length(header_buffer);
    if (content_length <= 0) {
        send_response(client_fd, 411, "Length Required", "{\"error\":\"missing or invalid content length\"}");
        return;
    }

    if (content_length > MAX_BODY_SIZE) {
        send_response(client_fd, 413, "Payload Too Large", "{\"error\":\"payload too large\"}");
        return;
    }

    char *body = calloc((size_t)content_length + 1, 1);
    if (!body) {
        send_response(client_fd, 500, "Internal Server Error", "{\"error\":\"out of memory\"}");
        return;
    }

    int already_have = total_read - header_end_offset;
    if (already_have > content_length) already_have = content_length;

    if (already_have > 0) {
        memcpy(body, header_buffer + header_end_offset, (size_t)already_have);
    }

    int body_read = already_have;
    while (body_read < content_length) {
        int n = recv(client_fd, body + body_read, content_length - body_read, 0);
        if (n <= 0) {
            free(body);
            send_response(client_fd, 400, "Bad Request", "{\"error\":\"failed to read body\"}");
            return;
        }
        body_read += n;
    }

    body[content_length] = '\0';

    printf("Received payload from %s:\n%s\n", remote_ip ? remote_ip : "unknown", body);

    unsigned long long batch_id = 0;
    int inserted_count = 0;
    int duplicate_count = 0;

    if (ingest_payload(body, remote_ip, &batch_id, &inserted_count, &duplicate_count) != 0) {
        free(body);
        send_response(client_fd, 400, "Bad Request", "{\"error\":\"invalid or unsupported JSON payload\"}");
        return;
    }

    free(body);

    char ok_body[256];
    snprintf(
        ok_body,
        sizeof(ok_body),
        "{\"status\":\"ok\",\"batch_id\":%llu,\"inserted_measurements\":%d,\"duplicate_measurements\":%d}",
        batch_id,
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
        return 1;
    }

    if (db_connect() != 0) {
        return 1;
    }

    if (db_init_schema() != 0) {
        mysql_close(g_db);
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        mysql_close(g_db);
        return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        mysql_close(g_db);
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
        mysql_close(g_db);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        mysql_close(g_db);
        return 1;
    }

    printf("Endpoint server running on port %d\n", SERVER_PORT);
    printf("Endpoint: POST /measurements\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        const char *remote_ip = inet_ntoa(client_addr.sin_addr);
        printf("Connection from %s:%d\n", remote_ip, ntohs(client_addr.sin_port));

        handle_client(client_fd, remote_ip);
        close(client_fd);
    }

    close(server_fd);
    mysql_close(g_db);
    return 0;
}
