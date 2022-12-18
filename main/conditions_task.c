#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "conditions_task.h"

#include "gpio.h"
#include "http_client.h"
#include "json.h"
#include "log.h"
#include "nvs.h"
#include "ota_task.h"
#include "screen_img_handler.h"
#include "sleep_handler.h"
#include "sntp_time.h"
#include "timer.h"
#include "wifi.h"

#define TAG "sc-conditions-task"

#define NUM_DIFFERENTIAL_UPDATES 3
#define NUM_DISCRETE_UPDATES 4

#define TIME_UPDATE_INTERVAL_SECONDS (1 * SECS_PER_MIN)
#define CONDITIONS_UPDATE_INTERVAL_SECONDS (20 * SECS_PER_MIN)
#define CHARTS_UPDATE_INTERVAL_SECONDS (MINS_PER_HOUR * SECS_PER_MIN)
#define OTA_CHECK_INTERVAL_SECONDS (CONFIG_OTA_CHECK_INTERVAL_HOURS * MINS_PER_HOUR * SECS_PER_MIN)

#define UPDATE_CONDITIONS_BIT (1 << 0)
#define UPDATE_TIDE_CHART_BIT (1 << 1)
#define UPDATE_SWELL_CHART_BIT (1 << 2)
#define UPDATE_TIME_BIT (1 << 3)
#define UPDATE_SPOT_NAME_BIT (1 << 4)
#define CHECK_OTA_BIT (1 << 5)

typedef struct {
    char   name[11];
    time_t update_interval_secs;
    time_t last_executed_epoch_secs;
    bool   force_next_update;
    void (*execute)(void);
} differential_update_t;

typedef struct {
    char    name[14];
    uint8_t hour;
    uint8_t minute;
    bool    executed_already;
    void (*execute)(void);
    bool force_next_update;
} discrete_update_t;

static void conditions_trigger_ota_check();

static TaskHandle_t conditions_update_task_handle;

static volatile unsigned int seconds_elapsed;
static conditions_t          last_retrieved_conditions;

// Execute function cannot be blocking! Will execute from 1 sec timer interrupt callback
static differential_update_t differential_updates[NUM_DIFFERENTIAL_UPDATES] = {
    {
        .name                 = "time",
        .force_next_update    = true,
        .update_interval_secs = TIME_UPDATE_INTERVAL_SECONDS,
        .execute              = conditions_trigger_time_update,
    },
    {
        .name                 = "conditions",
        .force_next_update    = true,
        .update_interval_secs = CONDITIONS_UPDATE_INTERVAL_SECONDS,
        .execute              = conditions_trigger_conditions_update,
    },
    {
        .name                 = "ota",
        .force_next_update    = true,
        .update_interval_secs = OTA_CHECK_INTERVAL_SECONDS,
        .execute              = conditions_trigger_ota_check,
    },
};

static discrete_update_t discrete_updates[NUM_DISCRETE_UPDATES] = {
    {
        .name              = "tide",
        .force_next_update = true,
        .hour              = 3,
        .minute            = 0,
        .executed_already  = false,
        .execute           = conditions_trigger_tide_chart_update,
    },
    {
        .name              = "swell_morning",
        .force_next_update = true,
        .hour              = 12,
        .minute            = 0,
        .executed_already  = false,
        .execute           = conditions_trigger_swell_chart_update,
    },
    {
        .name              = "swell_midday",
        .force_next_update = true,
        .hour              = 21,
        .minute            = 0,
        .executed_already  = false,
        .execute           = conditions_trigger_swell_chart_update,
    },
    {
        .name              = "swell_evening",
        .force_next_update = true,
        .hour              = 17,
        .minute            = 0,
        .executed_already  = false,
        .execute           = conditions_trigger_swell_chart_update,
    },
};

/*
 * Returns whether or not the two time values (hour or min) match, OR always true if value is 0xFF wildcard (similar to
 * cron's '*')
 */
static inline bool discrete_time_matches(int current, uint8_t check) {
    return (current == check) || (check == 0xFF);
}

static void conditions_timer_expired_callback(void *timer_args) {
    struct tm now_local;
    sntp_time_get_local_time(&now_local);
    time_t now_epoch_secs = mktime(&now_local);

    // If time differential has passed OR force execute flag set, execute and bring up to date.
    differential_update_t *diff_check = NULL;
    for (int i = 0; i < (NUM_DIFFERENTIAL_UPDATES - 1); i++) {
        diff_check = &differential_updates[i];

        if (((now_epoch_secs - diff_check->last_executed_epoch_secs) > diff_check->update_interval_secs) ||
            diff_check->force_next_update) {
            // Printing time_t is fucked, have to convert to double with difftime
            log_printf(LOG_LEVEL_DEBUG,
                       "Executing polling diff update '%s' (last: %.0f, now: %.0f, intvl: %.0f, force: %u)",
                       diff_check->name,
                       difftime(diff_check->last_executed_epoch_secs, 0),
                       difftime(now_epoch_secs, 0),
                       difftime(diff_check->update_interval_secs, 0),
                       diff_check->force_next_update);

            diff_check->execute();
            diff_check->last_executed_epoch_secs = now_epoch_secs;
            diff_check->force_next_update        = false;
        }
    }

    // If matching time has arrived OR force execute flag set, execute.
    discrete_update_t *discrete_check = NULL;
    for (int i = 0; i < (NUM_DISCRETE_UPDATES - 1); i++) {
        discrete_check = &discrete_updates[i];

        if ((discrete_time_matches(now_local.tm_hour, discrete_check->hour) &&
             discrete_time_matches(now_local.tm_min, discrete_check->minute)) ||
            discrete_check->force_next_update) {
            if (!discrete_check->executed_already) {
                log_printf(LOG_LEVEL_DEBUG,
                           "Executing discrete update '%s' (curr hr: %u, curr min: %u, check hr: %u, check min: %u, "
                           "force: %u)",
                           discrete_check->name,
                           now_local.tm_hour,
                           now_local.tm_min,
                           discrete_check->hour,
                           discrete_check->minute,
                           discrete_check->force_next_update);

                discrete_check->execute();
                discrete_check->force_next_update = false;
                discrete_check->executed_already  = true;
            }
        } else if (discrete_check->executed_already) {
            discrete_check->executed_already = false;
        }
    }
}

/*
 * Returns success
 */
static bool conditions_refresh() {
    spot_check_config *config = nvs_get_config();
    char               url_buf[strlen(URL_BASE) + 80];
    query_param        params[3];
    request            request = http_client_build_request("conditions", config, url_buf, params, 3);

    char                    *server_response    = NULL;
    size_t                   response_data_size = 0;
    esp_http_client_handle_t client;
    bool                     success = http_client_perform_request(&request, &client);

    // This MUST be here to short circuit execution. If http_client_read_response_to_* is called after a failure of
    // http_client_perform_request, the inner call to client cleanup function will assert and crash and there's nothing
    // we can do to wrap or error check it
    if (!success) {
        log_printf(LOG_LEVEL_ERROR,
                   "Received false success trying to perform req before reading response, bailing out of process");
        return false;
    }

    esp_err_t http_err = http_client_read_response_to_buffer(&client, &server_response, &response_data_size);

    if (http_err == ESP_OK && response_data_size != 0) {
        cJSON *json               = parse_json(server_response);
        cJSON *data_value         = cJSON_GetObjectItem(json, "data");
        cJSON *temperature_object = cJSON_GetObjectItem(data_value, "temp");
        cJSON *wind_speed_object  = cJSON_GetObjectItem(data_value, "wind_speed");
        cJSON *wind_dir_object    = cJSON_GetObjectItem(data_value, "wind_dir");
        cJSON *tide_height_object = cJSON_GetObjectItem(data_value, "tide_height");

        log_printf(LOG_LEVEL_DEBUG, "Server response: %s", server_response);
        if (server_response != NULL) {
            free(server_response);
            server_response = NULL;
        }

        if (wind_dir_object == NULL || tide_height_object == NULL || wind_speed_object == NULL ||
            temperature_object == NULL) {
            log_printf(LOG_LEVEL_ERROR,
                       "Parsed at least one field to a null cJSON object. That means the field wasn't in the response "
                       "at all but a successful request response "
                       "code (usually  a wifi login portal default login page)");
            return false;
        }

        // char *temperature_debug_str = cJSON_Print(temperature_object);
        // char *wind_speed_debug_str  = cJSON_Print(wind_speed_object);
        // char *wind_dir_debug_str    = cJSON_Print(wind_dir_object);
        // char *tide_height_debug_str = cJSON_Print(tide_height_object);
        // log_printf(LOG_LEVEL_DEBUG, "cJSON_Print value for temperature: %s", temperature_debug_str);
        // log_printf(LOG_LEVEL_DEBUG, "cJSON_Print value for wind_speed: %s", wind_speed_debug_str);
        // log_printf(LOG_LEVEL_DEBUG, "cJSON_Print value for wind_dir: %s", wind_dir_debug_str);
        // log_printf(LOG_LEVEL_DEBUG, "cJSON_Print value for tide_height: %s", tide_height_debug_str);
        // free(temperature_debug_str);
        // free(wind_speed_debug_str);
        // free(wind_dir_debug_str);
        // free(tide_height_debug_str);

        // Parse out end-result values with fallbacks in case value for key is not expected type
        int8_t temperature = 0;
        if (cJSON_IsNumber(temperature_object)) {
            temperature = temperature_object->valueint;
        } else {
            log_printf(LOG_LEVEL_WARN, "Expecting number from api for temp key, did not get one. Defaulting to -99");
            temperature = -99;
        }

        uint8_t wind_speed = 0;
        if (cJSON_IsNumber(wind_speed_object)) {
            wind_speed = wind_speed_object->valueint;
        } else {
            log_printf(LOG_LEVEL_WARN,
                       "Expecting number from api for wind_speed key, did not get one. Defaulting to 99");
            wind_speed = 99;
        }

        char *wind_dir_str = NULL;
        if (cJSON_IsString(wind_dir_object)) {
            wind_dir_str = cJSON_GetStringValue(wind_dir_object);
        } else {
            log_printf(LOG_LEVEL_WARN, "Expecting string from api for wind_dir key, did not get one. Defaulting to ?");
            wind_dir_str = "X";
        }

        char *tide_height_str = NULL;
        if (cJSON_IsString(tide_height_object)) {
            tide_height_str = cJSON_GetStringValue(tide_height_object);
        } else {
            log_printf(LOG_LEVEL_WARN,
                       "Expecting string from api for tide_height key, did not get one. Defaulting to ?");
            tide_height_str = "?";
        }

        // Copy into global conditions after every field set
        last_retrieved_conditions.temperature = temperature;
        last_retrieved_conditions.wind_speed  = wind_speed;
        strcpy(last_retrieved_conditions.wind_dir, wind_dir_str);
        strcpy(last_retrieved_conditions.tide_height, tide_height_str);

        cJSON_free(data_value);
        cJSON_free(json);
    } else {
        log_printf(LOG_LEVEL_INFO, "Failed to get new conditions, leaving last saved values displayed");
        return false;
    }

    // Caller responsible for freeing buffer if non-null on return
    if (server_response != NULL) {
        free(server_response);
        server_response = NULL;
    }

    return true;
}

static void conditions_update_task(void *args) {
    log_printf(LOG_LEVEL_DEBUG, "List of all time differential updates:");
    differential_update_t *diff_check = NULL;
    for (int i = 0; i < (NUM_DIFFERENTIAL_UPDATES - 1); i++) {
        diff_check = &differential_updates[i];
        log_printf(LOG_LEVEL_DEBUG,
                   "'%s' executing every %.0f seconds",
                   diff_check->name,
                   diff_check->update_interval_secs);
    }

    log_printf(LOG_LEVEL_DEBUG, "List of all discrete updates:");
    discrete_update_t *discrete_check = NULL;
    for (int i = 0; i < (NUM_DISCRETE_UPDATES - 1); i++) {
        discrete_check = &discrete_updates[i];
        log_printf(LOG_LEVEL_DEBUG,
                   "'%s' executing at %u:%02u",
                   discrete_check->name,
                   discrete_check->hour,
                   discrete_check->minute);
    }

    timer_info_handle conditions_handle = timer_init("conditions", conditions_timer_expired_callback, NULL, MS_PER_SEC);
    timer_reset(conditions_handle, true);

    // Wait forever until connected
    wifi_block_until_connected();

    uint32_t update_bits        = 0;
    bool     full_clear         = false;
    bool     conditions_success = false;
    while (1) {
        // Wait forever until a notification received. Clears all bits on exit since we'll handle every set bit in one
        // go
        xTaskNotifyWait(0x0, UINT32_MAX, &update_bits, portMAX_DELAY);

        log_printf(LOG_LEVEL_DEBUG,
                   "update-conditions task received task notification of value 0x%02X, updating accordingly",
                   update_bits);

        // If we're doing all of them, it means this is the first time we're refreshing after boot, and it should wait
        // and do a full clear before redrawing everything. Otherwise it's very piecemeal and slow
        full_clear = (update_bits & UPDATE_SPOT_NAME_BIT) && (update_bits & UPDATE_SPOT_NAME_BIT) &&
                     (update_bits & UPDATE_CONDITIONS_BIT) && (update_bits & UPDATE_TIDE_CHART_BIT) &&
                     (update_bits & UPDATE_SWELL_CHART_BIT);

        /***************************************
         * Network update section
         **************************************/
        if (update_bits & UPDATE_CONDITIONS_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_CONDITIONS_BIT);
            conditions_success = conditions_refresh();
            sleep_handler_set_idle(SYSTEM_IDLE_CONDITIONS_BIT);
        }

        if (update_bits & UPDATE_TIDE_CHART_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_TIDE_CHART_BIT);
            screen_img_handler_download_and_save(SCREEN_IMG_TIDE_CHART);
            sleep_handler_set_idle(SYSTEM_IDLE_TIDE_CHART_BIT);
        }

        if (update_bits & UPDATE_SWELL_CHART_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_SWELL_CHART_BIT);
            screen_img_handler_download_and_save(SCREEN_IMG_SWELL_CHART);
            sleep_handler_set_idle(SYSTEM_IDLE_SWELL_CHART_BIT);
        }

        if (update_bits & CHECK_OTA_BIT) {
            // Just kicks off the task non-blocking so this won't actually disrupt anything with rest of conditions
            // update loop
            ota_task_start();
        }

        /***************************************
         * Framebuffer update section
         **************************************/
        if (full_clear) {
            log_printf(LOG_LEVEL_DEBUG,
                       "Performing full screen clear from conditions_task since every piece was updated");
            screen_img_handler_full_clear();
        }

        if (update_bits & UPDATE_TIME_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_TIME_BIT);
            if (!full_clear) {
                screen_img_handler_clear_time();
                screen_img_handler_clear_date(false);
            }
            screen_img_handler_draw_time();
            screen_img_handler_draw_date();
            log_printf(LOG_LEVEL_INFO, "update-conditions task updated time");
            sleep_handler_set_idle(SYSTEM_IDLE_TIME_BIT);
        }

        if (update_bits & UPDATE_SPOT_NAME_BIT) {
            // Slightly unique case as in it requires no network update, just used as a display update trigger
            sleep_handler_set_busy(SYSTEM_IDLE_CONDITIONS_BIT);
            spot_check_config *config = nvs_get_config();

            // TODO :: would be nice to have a 'previous_spot_name' key in config so we could pass to clear function
            // to smart erase with text inverse instead of block erasing max spot name width
            if (!full_clear) {
                screen_img_handler_clear_spot_name();
            }
            screen_img_handler_draw_spot_name(config->spot_name);
            sleep_handler_set_idle(SYSTEM_IDLE_CONDITIONS_BIT);
        }

        if (update_bits & UPDATE_CONDITIONS_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_CONDITIONS_BIT);
            // TODO :: don't support clearing spot name logic when changing location yet. Need a way to pass more info
            // to this case if we're clearing for a regular update or becase location changed and spot name will need to
            // be cleared too.
            if (!full_clear) {
                screen_img_handler_clear_conditions(true, true, true);
            }
            if (conditions_success) {
                screen_img_handler_draw_conditions(&last_retrieved_conditions);
            } else {
                screen_img_handler_draw_conditions_error();
            }
            log_printf(LOG_LEVEL_INFO, "update-conditions task updated conditions");
            sleep_handler_set_idle(SYSTEM_IDLE_CONDITIONS_BIT);
        }

        if (update_bits & UPDATE_TIDE_CHART_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_TIDE_CHART_BIT);
            if (!full_clear) {
                screen_img_handler_clear_screen_img(SCREEN_IMG_TIDE_CHART);
            }
            screen_img_handler_draw_screen_img(SCREEN_IMG_TIDE_CHART);
            log_printf(LOG_LEVEL_INFO, "update-conditions task updated tide chart");
            sleep_handler_set_idle(SYSTEM_IDLE_TIDE_CHART_BIT);
        }

        if (update_bits & UPDATE_SWELL_CHART_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_SWELL_CHART_BIT);
            if (!full_clear) {
                screen_img_handler_clear_screen_img(SCREEN_IMG_SWELL_CHART);
            }
            screen_img_handler_draw_screen_img(SCREEN_IMG_SWELL_CHART);
            log_printf(LOG_LEVEL_INFO, "update-conditions task updated swell chart");
            sleep_handler_set_idle(SYSTEM_IDLE_SWELL_CHART_BIT);
        }

        /***************************************
         * Render section
         **************************************/
        if (update_bits) {
            // If any other bits besides time are set, mark full screen as dirty so it refreshes all faded pixels
            if (update_bits & ~(UPDATE_TIME_BIT)) {
                screen_img_handler_mark_all_lines_dirty();
            }

            screen_img_handler_render(__func__, __LINE__);
        }
    }
}

static void conditions_trigger_ota_check() {
    xTaskNotify(conditions_update_task_handle, CHECK_OTA_BIT, eSetBits);
}

void conditions_trigger_time_update() {
    xTaskNotify(conditions_update_task_handle, UPDATE_TIME_BIT, eSetBits);
}

void conditions_trigger_spot_name_update() {
    xTaskNotify(conditions_update_task_handle, UPDATE_SPOT_NAME_BIT, eSetBits);
}

void conditions_trigger_conditions_update() {
    xTaskNotify(conditions_update_task_handle, UPDATE_CONDITIONS_BIT, eSetBits);
}

void conditions_trigger_tide_chart_update() {
    xTaskNotify(conditions_update_task_handle, UPDATE_TIDE_CHART_BIT, eSetBits);
}

void conditions_trigger_swell_chart_update() {
    xTaskNotify(conditions_update_task_handle, UPDATE_SWELL_CHART_BIT, eSetBits);
}

void conditions_trigger_both_charts_update() {
    xTaskNotify(conditions_update_task_handle, UPDATE_SWELL_CHART_BIT | UPDATE_TIDE_CHART_BIT, eSetBits);
}

void conditions_update_task_init() {
}

void conditions_update_task_start() {
    xTaskCreate(&conditions_update_task,
                "conditions-update",
                SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 4,
                NULL,
                tskIDLE_PRIORITY,
                &conditions_update_task_handle);
}
