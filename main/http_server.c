#include "constants.h"

#include <esp_http_server.h>
#include <esp_system.h>
#include <log.h>
#include <sys/param.h>

#include "constants.h"
#include "http_server.h"
#include "json.h"
#include "nvs.h"

#define TAG SC_TAG_HTTP_SERVER

static httpd_handle_t server_handle = NULL;

static esp_err_t health_get_handler(httpd_req_t *req);
static esp_err_t configure_post_handler(httpd_req_t *req);
static esp_err_t current_config_get_handler(httpd_req_t *req);
static esp_err_t clear_nvs_post_handler(httpd_req_t *req);

static const httpd_uri_t health_uri = {.uri      = "/health",
                                       .method   = HTTP_GET,
                                       .handler  = health_get_handler,
                                       .user_ctx = NULL};

static const httpd_uri_t configure_uri = {.uri      = "/configure",
                                          .method   = HTTP_POST,
                                          .handler  = configure_post_handler,
                                          .user_ctx = NULL};

static const httpd_uri_t current_config_uri = {.uri      = "/current_configuration",
                                               .method   = HTTP_GET,
                                               .handler  = current_config_get_handler,
                                               .user_ctx = NULL};

static const httpd_uri_t clear_nvs_uri = {.uri      = "/clear_nvs",
                                          .method   = HTTP_POST,
                                          .handler  = clear_nvs_post_handler,
                                          .user_ctx = NULL};

static esp_err_t health_get_handler(httpd_req_t *req) {
    httpd_resp_send(req, "Surviving not thriving", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t configure_post_handler(httpd_req_t *req) {
    const int rx_buf_size = 300;
    char      buf[rx_buf_size];

    if (req->content_len > rx_buf_size) {
        log_printf(LOG_LEVEL_ERROR, "Payload is too big (%d bytes), bailing out", req->content_len);
        httpd_resp_send(req, "err", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int bytes_received = httpd_req_recv(req, buf, MIN(req->content_len, rx_buf_size));
    if (bytes_received <= 0) {
        if (bytes_received == HTTPD_SOCK_ERR_TIMEOUT) {
            /* Retry receiving if timeout occurred */
            // continue;
            log_printf(LOG_LEVEL_ERROR, "Received timeout, bailing out (could retry though)");
            httpd_resp_send(req, "err", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        return ESP_FAIL;
    }

    /* Log data received */
    log_printf(LOG_LEVEL_INFO, "=========== RECEIVED DATA ==========");
    log_printf(LOG_LEVEL_INFO, "%.*s", bytes_received, buf);
    log_printf(LOG_LEVEL_INFO, "====================================");

    // Should really be parsing this with prealloced buf. If user sends fat payload
    // we'll crash from heap overflow
    cJSON *payload = parse_json(buf);
    if (payload == NULL) {
        log_printf(LOG_LEVEL_ERROR, "Couldn't parse json (TODO return the right code");
        httpd_resp_send(req, "err", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Load all our values into here to save to nvs. No need to alloc
    // any more memory than the json takes because nvs will use those
    // pointers to write directly to flash
    spot_check_config config;
    char             *default_spot_name  = "The Wedge";
    char             *default_spot_lat   = "33.5930302087";
    char             *default_spot_lon   = "-117.8819918632";
    char             *default_spot_uid   = "5842041f4e65fad6a770882b";
    int8_t            default_utc_offset = 0;

    cJSON *json_spot_name = cJSON_GetObjectItem(payload, "spot_name");
    if (cJSON_IsString(json_spot_name)) {
        config.spot_name = cJSON_GetStringValue(json_spot_name);
        if (strlen(config.spot_name) > MAX_LENGTH_SPOT_NAME_PARAM) {
            log_printf(LOG_LEVEL_INFO,
                       "Received spot_name > %d chars, invalid. Defaulting to %s",
                       MAX_LENGTH_SPOT_NAME_PARAM,
                       default_spot_name);
            config.spot_name = default_spot_name;
        }
    } else {
        log_printf(LOG_LEVEL_INFO, "Unable to parse spot_name param, defaulting to %s", default_spot_name);
        config.spot_name = default_spot_name;
    }

    cJSON *json_spot_lat = cJSON_GetObjectItem(payload, "spot_lat");
    if (cJSON_IsString(json_spot_lat)) {
        config.spot_lat = cJSON_GetStringValue(json_spot_lat);
        if (strlen(config.spot_lat) > MAX_LENGTH_SPOT_LAT_PARAM) {
            log_printf(LOG_LEVEL_INFO,
                       "Received spot_lat > %d chars, invalid. Defaulting to %s",
                       MAX_LENGTH_SPOT_LAT_PARAM,
                       default_spot_lat);
            config.spot_lat = default_spot_lat;
        }
    } else {
        log_printf(LOG_LEVEL_INFO, "Unable to parse spot_lat param, defaulting to %s", default_spot_lat);
        config.spot_lat = default_spot_lat;
    }

    cJSON *json_spot_lon = cJSON_GetObjectItem(payload, "spot_lon");
    if (cJSON_IsString(json_spot_lon)) {
        config.spot_lon = cJSON_GetStringValue(json_spot_lon);
        if (strlen(config.spot_lon) > MAX_LENGTH_SPOT_LON_PARAM) {
            log_printf(LOG_LEVEL_INFO,
                       "Received spot_lon > %d chars, invalid. Defaulting to %s",
                       MAX_LENGTH_SPOT_LON_PARAM,
                       default_spot_lon);
            config.spot_lon = default_spot_lon;
        }
    } else {
        log_printf(LOG_LEVEL_INFO, "Unable to parse spot_lon param, defaulting to %s", default_spot_lon);
        config.spot_lon = default_spot_lon;
    }

    cJSON *json_spot_uid = cJSON_GetObjectItem(payload, "spot_uid");
    if (cJSON_IsString(json_spot_uid)) {
        config.spot_uid = cJSON_GetStringValue(json_spot_uid);
        if (strlen(config.spot_uid) > MAX_LENGTH_SPOT_UID_PARAM) {
            log_printf(LOG_LEVEL_INFO,
                       "Received spot_uid > %d chars, invalid. Defaulting to wedge uid (%s)",
                       MAX_LENGTH_SPOT_UID_PARAM,
                       default_spot_uid);
            config.spot_uid = default_spot_uid;
        }
    } else {
        log_printf(LOG_LEVEL_INFO, "Unable to parse spot_uid param, defaulting to wedge uid (%s)", default_spot_uid);
        config.spot_uid = default_spot_uid;
    }

    cJSON *json_utc_offset = cJSON_GetObjectItem(payload, "utc_offset");
    if (cJSON_IsNumber(json_utc_offset)) {
        config.utc_offset = cJSON_GetNumberValue(json_utc_offset);
    } else {
        log_printf(LOG_LEVEL_INFO, "Unable to parse utc_offset param, defaulting to %d", default_utc_offset);
        config.utc_offset = default_utc_offset;
    }

    nvs_save_config(&config);

    // End response
    cJSON_Delete(payload);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t current_config_get_handler(httpd_req_t *req) {
    spot_check_config *current_config = nvs_get_config();

    cJSON *root            = cJSON_CreateObject();
    cJSON *spot_name_json  = cJSON_CreateString(current_config->spot_name);
    cJSON *spot_lat_json   = cJSON_CreateString(current_config->spot_lat);
    cJSON *spot_lon_json   = cJSON_CreateString(current_config->spot_lon);
    cJSON *spot_uid_json   = cJSON_CreateString(current_config->spot_uid);
    cJSON *utc_offset_json = cJSON_CreateNumber(current_config->utc_offset);
    cJSON_AddItemToObject(root, "spot_name", spot_name_json);
    cJSON_AddItemToObject(root, "spot_lat", spot_lat_json);
    cJSON_AddItemToObject(root, "spot_lon", spot_lon_json);
    cJSON_AddItemToObject(root, "spot_uid", spot_uid_json);
    cJSON_AddItemToObject(root, "utc_offset", utc_offset_json);

    char *response_json = cJSON_Print(root);
    httpd_resp_send(req, response_json, HTTPD_RESP_USE_STRLEN);

    cJSON_Delete(root);
    cJSON_free(response_json);
    return ESP_OK;
}

static esp_err_t clear_nvs_post_handler(httpd_req_t *req) {
    int  query_buf_len = 30;
    char query_buf[query_buf_len];
    int  actual_query_len = httpd_req_get_url_query_len(req) + 1;
    if (actual_query_len > query_buf_len) {
        log_printf(LOG_LEVEL_INFO,
                   "Query str too long for buffer (%d long, can only fit %d)",
                   actual_query_len,
                   query_buf_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid query string");
        return ESP_OK;
    }

    if (httpd_req_get_url_query_str(req, query_buf, actual_query_len) == ESP_OK) {
        char value[15];
        if (httpd_query_key_value(query_buf, "key", value, sizeof(value)) == ESP_OK) {
            if (strcmp(value, "sekrit") == 0) {
                ESP_ERROR_CHECK(nvs_full_erase());
                httpd_resp_send(req, "Successfully cleared nvs, restarting", HTTPD_RESP_USE_STRLEN);
                esp_restart();
            } else {
                log_printf(LOG_LEVEL_INFO, "Received incorrect key for erasing flash: %s", value);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid query string");
            }
        }
    } else {
        log_printf(LOG_LEVEL_INFO, "Failed to get query string");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get query string");
    }

    return ESP_OK;
}

void http_server_start() {
    if (server_handle) {
        log_printf(LOG_LEVEL_WARN, "http_server already started and http_server_start called, ignoring and bailing");
        return;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    log_printf(LOG_LEVEL_INFO, "Starting server on port: '%d'", 80);
    esp_err_t err;
    if ((err = httpd_start(&server, &config)) != ESP_OK) {
        log_printf(LOG_LEVEL_INFO, "Error starting webserver (%s), trying one more time", esp_err_to_name(err));
        err = httpd_start(&server, &config);
        if (err != ESP_OK) {
            log_printf(LOG_LEVEL_INFO,
                       "Error starting webserver (%s) for the second time, rebooting...",
                       esp_err_to_name(err));
            esp_restart();
        }
    }

    httpd_register_uri_handler(server, &configure_uri);
    httpd_register_uri_handler(server, &health_uri);
    httpd_register_uri_handler(server, &current_config_uri);
    httpd_register_uri_handler(server, &clear_nvs_uri);

    server_handle = server;
}

void http_server_stop() {
    if (server_handle == NULL) {
        log_printf(LOG_LEVEL_WARN, "http_server not running and http_server_stop called, ignoring.");
        return;
    }

    ESP_ERROR_CHECK(httpd_stop(server_handle));
    server_handle = NULL;
}
