#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/adc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_event_loop.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_modem.h"
#include "esp_modem_netif.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"

#include "sim800.h"
#include "bg96.h"
#include "sim7600.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h" 

#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"

//#include "mqtt_client.h"

#include "tasks.h"
#include "modem.h"

#define LTE_READY false

#define AWS_HOST "aqhwxv05d6bur-ats.iot.us-east-1.amazonaws.com"
#define AWS_CLIENT_ID "ESP32_EXO"

#define ESP_WIFI_SSID      "PruebaTBA"
#define ESP_WIFI_PASS      "pruebaTBA"
#define ESP_MAXIMUM_RETRY  5

#define THRESHOLD_MIN_DEFAULT 0
#define THRESHOLD_MAX_DEFAULT 50
#define RATE_DEFAULT 60
#define RATE_MIN 10
#define TEMP_MIN -20
#define TEMP_MAX 70
#define STRING_LENGTH_SMALL 30
#define STRING_LENGTH_BIG 50

#define MAX_BUFFER_RING 200

#define ONE_SEC 1000 / portTICK_PERIOD_MS
#define LED_RATE 0.25 * ONE_SEC
#define ADC_RATE 0.5 * ONE_SEC
#define MQTT_RATE  rate * ONE_SEC
#define LTE_RATE 5 * ONE_SEC
#define CONFIG_RATE 1 * ONE_SEC
#define N_QUEUE 2*MAX_BUFFER_RING

#define DATA_FORMAT "{\"t\":%u,\"T\":[%u,%u,%u],\"B\":%.2f}"
#define STATE_FORMAT "{\"t\":%u,\"s\":\"%s\"}"

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define CONNECT_BIT     BIT0
#define STOP_BIT        BIT1
#define GOT_DATA_BIT    BIT2
#define LED_GPIO 2
#define OFF 0
#define ON 1

#define BROKER_USER "f8caa162"
#define BROKER_PASS "4dfa0cbff7b885ea"
#define BROKER_HOST "broker.shiftr.io"

//#define BROKER_URL "mqtt://f8caa162:4dfa0cbff7b885ea@broker.shiftr.io"
#define BROKER_URL "mqtt://" BROKER_USER ":" BROKER_PASS "@" BROKER_HOST

#define NAME "ESP32_MARTIN"

#define DC_DISCONNECTED_MSG "BATTERY_ONLY"
#define DC_CONNECTED_MSG "DC_CONNECTED"
#define HIGH_TEMPERATURE "HIGH_TEMP"
#define LOW_TEMPERATURE "LOW_TEMP"
#define NORMAL_TEMPERATURE "NORMAL_TEMP"

#define LTE_TAG "[LTE]"
#define WIFI_TAG "[WiFi]"
#define MQTT_TAG "[MQTT]"
#define AMAZON_TAG "[Amazon]"
#define SYSTEM_TAG "[System]"
#define NVS_TAG "[NVS]"
#define ADC_TAG "[ADC]"

#define STATE_TOPIC "/states"
#define DATA_TOPIC "/data"
#define CONFIG_TOPIC "/configuration"

#define STORAGE_NAMESPACE "storage"