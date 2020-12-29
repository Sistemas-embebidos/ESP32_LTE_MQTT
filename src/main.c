
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#include "driver/gpio.h"

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

#include "nvs_flash.h"
#include "tcpip_adapter.h"
//#include "protocol_examples_common.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h" 

#include "esp_log.h"
#include "mqtt_client.h"

/* The examples use WiFi configuration that you can set via project configuration menu
   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define ESP_WIFI_SSID      "PruebaTBA"
#define ESP_WIFI_PASS      "pruebaTBA"
#define ESP_MAXIMUM_RETRY  5

#define LED_RATE 1000 / portTICK_PERIOD_MS
#define MQTT_RATE  30 * 1000 / portTICK_PERIOD_MS
#define N_QUEUE 10

#define MSG_FORMAT "{\"t\":%u,\"T\":[%u,%u,%u],\"B\":%u}"

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t wifi_event_group;

QueueHandle_t Queue_data;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define CONNECTED_BIT BIT0
#define FAIL_BIT      BIT1
#define APSTARTED_BIT BIT2

#define CONFIG_BROKER_URL "mqtt://f8caa162:4dfa0cbff7b885ea@broker.shiftr.io"

#define NOMBRE "ESP32_MARTIN"

/* Can use project configuration menu (idf.py menuconfig) to choose the GPIO to blink,
   or you can edit the following line and set a number here.
*/
#define BLINK_GPIO 2


#define BROKER_URL "mqtt://f8caa162:4dfa0cbff7b885ea@broker.shiftr.io"
#define NAME "ESP32_MARTIN"

static const char *LTE_TAG = "[LTE]";
static const char *WIFI_TAG = "[WiFi]";
static const char *MQTT_TAG = "[MQTT]";
static const char *SYSTEM_TAG = "[System]";

static int s_retry_num = 0;

esp_mqtt_client_config_t mqtt_cfg = {
        .uri = BROKER_URL,
        .client_id = NAME,
    };

esp_mqtt_client_handle_t client;

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            //s_retry_num++;
            ESP_LOGI(WIFI_TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(wifi_event_group, FAIL_BIT);
        }
        ESP_LOGI(WIFI_TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(WIFI_TAG, "got ip:%s",
                 ip4addr_ntoa(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
}

esp_err_t esp32_wifi_eventHandler(void *ctx, system_event_t *event) {
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_AP_START:
        xEventGroupSetBits(wifi_event_group, APSTARTED_BIT);
        ESP_LOGD(LTE_TAG, "AP Started");
        break;
    case SYSTEM_EVENT_AP_STOP:
        xEventGroupClearBits(wifi_event_group, APSTARTED_BIT);
        ESP_LOGD(LTE_TAG, "AP Stopped");
        break;
    case SYSTEM_EVENT_AP_STACONNECTED:
        ESP_LOGD(LTE_TAG, "station connected to access point. Connected stations:");
        wifi_sta_list_t sta_list;
        ESP_ERROR_CHECK( esp_wifi_ap_get_sta_list(&sta_list));
        for(int i = 0; i < sta_list.num; i++) {
            //Print the mac address of the connected station
            wifi_sta_info_t sta = sta_list.sta[i];
            ESP_LOGD(LTE_TAG, "Station %d MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", i, sta.mac[0], sta.mac[1], sta.mac[2], sta.mac[3], sta.mac[4], sta.mac[5]);
        }
        break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
        ESP_LOGD(LTE_TAG, "station disconnected from access point");
        break;
    default:
        break;
    }
	return ESP_OK;
}

void wifi_init_sta()
{
    wifi_event_group = xEventGroupCreate();

    tcpip_adapter_init();

    #ifdef USING_WIFI
        ESP_ERROR_CHECK(esp_event_loop_create_default());
    #else
        ESP_ERROR_CHECK( esp_event_loop_init(esp32_wifi_eventHandler, NULL));
    #endif


    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	        .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT | FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually happened. */
    if (bits & CONNECTED_BIT) {
        ESP_LOGI(WIFI_TAG, "connected to ap SSID:%s password:%s", ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else if (bits & FAIL_BIT) {
        ESP_LOGI(WIFI_TAG, "Failed to connect to SSID:%s, password:%s", ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else {
        ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
    }

    //ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    //ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    //vEventGroupDelete(wifi_event_group);
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_publish(client, "/configuracion/conexion", "dia/mes/año hh:mm:ss", 0, 1, 0);
            ESP_LOGI(MQTT_TAG, "sent publish successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "/configuracion/conexion", 0);
            ESP_LOGI(MQTT_TAG, "sent subscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            msg_id = esp_mqtt_client_publish(client, "/configuracion/conexion", "suscripto", 0, 0, 0);
            ESP_LOGI(MQTT_TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(MQTT_TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(MQTT_TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

static void task_led(void *arg)
{
      gpio_reset_pin(BLINK_GPIO);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    while(1) {       
        gpio_set_level(BLINK_GPIO, 0);          // Blink off (output low)
        vTaskDelay(LED_RATE);  
        gpio_set_level(BLINK_GPIO, 1);          // Blink on (output high)
        vTaskDelay(LED_RATE);
    } 
}

static void task_data(void *arg)
{
    int valor = 0;

    while(1) {   
        valor ++;
        xQueueSend( Queue_data, &valor, portMAX_DELAY);
        vTaskDelay(MQTT_RATE);
    } 
}

static void task_mqtt(void *arg)
{
    ESP_LOGI(MQTT_TAG, "Iniciando MQTT" );

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);

    int valor;
    char dato[30];

    while(1) {   
        xQueueReceive( Queue_data, &valor, portMAX_DELAY);

        sprintf(dato,MSG_FORMAT,valor,valor,valor,valor,valor);
        esp_mqtt_client_publish(client, "/datos", dato , 0, 1, 0); 
        ESP_LOGI(MQTT_TAG, "Enviando dato por MQTT" );
        vTaskDelay(MQTT_RATE);
    } 
}

void app_main(void)
{
    printf("#################################################################\n");

    // Print chip information 
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(SYSTEM_TAG,"This is %s chip with %d CPU core(s), WiFi%s%s, ",
            CONFIG_IDF_TARGET, chip_info.cores,
            (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    ESP_LOGI(SYSTEM_TAG,"silicon revision %d, ", chip_info.revision);

    ESP_LOGI(SYSTEM_TAG,"%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    ESP_LOGI(SYSTEM_TAG,"Minimum free heap size: %d bytes\n", esp_get_minimum_free_heap_size());
    
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(WIFI_TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    Queue_data = xQueueCreate( N_QUEUE , sizeof( int  ) );

    if( Queue_data == NULL )
    {
        ESP_LOGE(SYSTEM_TAG,"No se pudo crear la cola");
        while(1);
    }

    xTaskCreate(task_led, "task_led", 2048, NULL, 5, NULL);
    xTaskCreate(task_data, "task_data", 2048, NULL, 5, NULL);
    xTaskCreate(task_mqtt, "task_mqtt", 2048, NULL, 5, NULL);
}




