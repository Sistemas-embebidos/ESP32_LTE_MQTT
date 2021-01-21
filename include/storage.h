#ifndef _NVS_H_
#define _NVS_H_

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

// No-Volatile Storage
#include "nvs.h"
#include "nvs_flash.h"

#define STORAGE_NAMESPACE "storage"
#define MAX_BUFFER_RING 200
#define STRING_LENGTH_SMALL 30
#define STRING_LENGTH_BIG 50
#define DATA_FORMAT "{\"t\":%u,\"T\":[%u,%u,%u],\"B\":%.2f}"
#define STATE_FORMAT "{\"t\":%u,\"s\":\"%s\"}"
#define NVS_TAG "[NVS]"

#define THRESHOLD_MIN_DEFAULT 0
#define THRESHOLD_MAX_DEFAULT 50
#define RATE_DEFAULT 60
#define RATE_MIN 10
#define TEMP_MIN -20
#define TEMP_MAX 70

typedef struct
{
    uint32_t timestamp;
    uint32_t temperature_1;
    uint32_t temperature_2;
    uint32_t temperature_3;
    uint32_t battery;
    bool temperature_alert;
} data_t;

esp_err_t save_read_data(data_t datas);
esp_err_t load_config(void);
esp_err_t save_config(void);
int parserJsonIntValues( char const* json, int* parsedValues );

#endif