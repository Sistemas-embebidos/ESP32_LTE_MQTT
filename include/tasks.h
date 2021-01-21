#ifndef _TASK_H_
#define _TASK_H_

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"
#include "driver/adc.h"

#include "wifi.h"

#define ONE_SEC 1000 / portTICK_PERIOD_MS
#define LED_RATE 0.25 * ONE_SEC
#define ADC_RATE 0.5 * ONE_SEC
#define MQTT_RATE  rate * ONE_SEC
#define LTE_RATE 5 * ONE_SEC
#define CONFIG_RATE 1 * ONE_SEC
#define N_QUEUE 2*MAX_BUFFER_RING

#define LED_GPIO 2
#define OFF 0
#define ON 1

#define DC_DISCONNECTED_MSG "BATTERY_ONLY"
#define DC_CONNECTED_MSG "DC_CONNECTED"
#define HIGH_TEMPERATURE "HIGH_TEMP"
#define LOW_TEMPERATURE "LOW_TEMP"
#define NORMAL_TEMPERATURE "NORMAL_TEMP"

#define ADC_TAG "[ADC]"
#define MQTT_TAG "[MQTT]"
#define SYSTEM_TAG "[System]"
#define LTE_TAG "[LTE]"

typedef enum
{
    DC_DISCONNECTED,
    DC_CONNECTING,
    DC_CONNECTED,
    DC_DISCONNECTING
} dc_state_t;

static void aws_iot_task(void *param);
static void task_led(void *arg);
static void task_lte(void *arg);
static void task_adc_read(void *arg);
static void task_nvs(void *arg);
static void task_config(void *arg);
static void task_connector(void* arg);

#endif