
#include "main.h"

#define ESP_WIFI_SSID      "PruebaTBA"
#define ESP_WIFI_PASS      "pruebaTBA"
#define ESP_MAXIMUM_RETRY  5

#define THRESHOLD_DEFAULT 50
#define RATE_DEFAULT 10
#define RATE_MIN 10
#define TEMP_MIN 0
#define STRING_LENGTH_SMALL 30
#define STRING_LENGTH_BIG 50

#define MAX_BUFFER_RING 10

char configuration[STRING_LENGTH_SMALL];
int threshold = THRESHOLD_DEFAULT;
int rate = RATE_DEFAULT;

//uint32_t data_saved_counter = 0, data_sent_counter = 0;
bool energia = false;

#define ONE_SEC 1000 / portTICK_PERIOD_MS
#define LED_RATE 0.5 * ONE_SEC
#define ADC_RATE rate * ONE_SEC
#define MQTT_RATE  rate * ONE_SEC
#define CONFIG_RATE 1 * ONE_SEC
#define N_QUEUE 2*MAX_BUFFER_RING

#define DATA_FORMAT "{\"t\":%u,\"T\":[%u,%u,%u],\"B\":%.2f}"
#define STATE_FORMAT "{\"t\":%u,\"s\":\"%s\"}"

static EventGroupHandle_t wifi_event_group; /* FreeRTOS event group to signal when we are connected*/

QueueHandle_t Queue_data,Queue_config;

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
#define NORMAL_TEMPERATURE "NORMAL_TEMP"

#define LTE_TAG "[LTE]"
#define WIFI_TAG "[WiFi]"
#define MQTT_TAG "[MQTT]"
#define SYSTEM_TAG "[System]"
#define NVS_TAG "[NVS]"
#define ADC_TAG "[ADC]"

#define STATE_TOPIC "/states"
#define DATA_TOPIC "/data"
#define CONFIG_TOPIC "/configuration"

#define STORAGE_NAMESPACE "storage"

static int s_retry_num = 0;

esp_mqtt_client_config_t mqtt_cfg = {
        .uri = BROKER_URL,
        .client_id = NAME,
    };

esp_mqtt_client_handle_t client;

typedef struct
{
    uint32_t timestamp;
    uint32_t temperature_1;
    uint32_t temperature_2;
    uint32_t temperature_3;
    uint32_t battery;
    bool temperature_alert;
} data_t;

typedef enum
{
    T_DATA,
    T_STATE,
    T_CONFIG
} type_t;

typedef struct
{
    char* payload;
    type_t type;
} message_t;

typedef enum
{
    DC_DISCONNECTED,
    DC_CONNECTING,
    DC_CONNECTED,
    DC_DISCONNECTING
} dc_state_t;

dc_state_t dc_state = DC_DISCONNECTED;

static void modem_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ESP_MODEM_EVENT_PPP_START:
        ESP_LOGI(LTE_TAG, "Modem PPP Started");
        break;
    case ESP_MODEM_EVENT_PPP_STOP:
        ESP_LOGI(LTE_TAG, "Modem PPP Stopped");
        xEventGroupSetBits(wifi_event_group, STOP_BIT);
        break;
    case ESP_MODEM_EVENT_UNKNOWN:
        ESP_LOGW(LTE_TAG, "Unknow line received: %s", (char *)event_data);
        break;
    default:
        break;
    }
}

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
            xEventGroupSetBits(wifi_event_group, STOP_BIT);
        }
        ESP_LOGI(WIFI_TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(WIFI_TAG, "got ip:%s",
                 ip4addr_ntoa(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, CONNECT_BIT);
    }
}

esp_err_t esp32_wifi_eventHandler(void *ctx, system_event_t *event) {
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECT_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECT_BIT);
        break;
    case SYSTEM_EVENT_AP_START:
        xEventGroupSetBits(wifi_event_group, GOT_DATA_BIT);
        ESP_LOGD(LTE_TAG, "AP Started");
        break;
    case SYSTEM_EVENT_AP_STOP:
        xEventGroupClearBits(wifi_event_group, GOT_DATA_BIT);
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

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_subscribe(client, CONFIG_TOPIC, 0);
            ESP_LOGI(MQTT_TAG, "sent subscribe successful, msg_id=%d", msg_id);  
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DATA");
            //printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            //printf("DATA=%.*s\r\n", event->data_len, event->data);
            memcpy(configuration,event->data,event->data_len);
            //printf("DATO=%s\r\n",configuration);

            xQueueSend( Queue_config, &configuration, portMAX_DELAY);

            xEventGroupSetBits(wifi_event_group, GOT_DATA_BIT);
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

    /* Waiting until either the connection is established (WIFI_CONNECT_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_STOP_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECT_BIT | STOP_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually happened. */
    if (bits & CONNECT_BIT) {
        ESP_LOGI(WIFI_TAG, "connected to ap SSID:%s password:%s", ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else if (bits & STOP_BIT) {
        ESP_LOGI(WIFI_TAG, "Failed to connect to SSID:%s, password:%s", ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else {
        ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
    }

    //ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    //ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    //vEventGroupDelete(wifi_event_group);
}

static void on_ppp_changed(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ESP_LOGI(LTE_TAG, "PPP state changed event %d", event_id);
    if (event_id == NETIF_PPP_ERRORUSER) {
        /* User interrupted event from esp-netif */
        esp_netif_t *netif = event_data;
        ESP_LOGI(LTE_TAG, "User interrupted event from netif:%p", netif);
    }
}

static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ESP_LOGD(LTE_TAG, "IP event! %d", event_id);
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        esp_netif_dns_info_t dns_info;

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_t *netif = event->esp_netif;

        ESP_LOGI(LTE_TAG, "Modem Connect to PPP Server");
        ESP_LOGI(LTE_TAG, "~~~~~~~~~~~~~~");
        ESP_LOGI(LTE_TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(LTE_TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(LTE_TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
        esp_netif_get_dns_info(netif, 0, &dns_info);
        ESP_LOGI(LTE_TAG, "Name Server1: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        esp_netif_get_dns_info(netif, 1, &dns_info);
        ESP_LOGI(LTE_TAG, "Name Server2: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        ESP_LOGI(LTE_TAG, "~~~~~~~~~~~~~~");
        xEventGroupSetBits(wifi_event_group, CONNECT_BIT);

        ESP_LOGI(LTE_TAG, "GOT ip event!!!");
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(LTE_TAG, "Modem Disconnect from PPP Server");
    } else if (event_id == IP_EVENT_GOT_IP6) {
        ESP_LOGI(LTE_TAG, "GOT IPv6 event!");

        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        ESP_LOGI(LTE_TAG, "Got IPv6 address " IPV6STR, IPV62STR(event->ip6_info.ip));
    }
}

esp_err_t save_read_data(data_t datas)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Open
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    // Read
    int32_t data_saved_counter = 0; // value will default to 0, if not set yet in NVS
    err = nvs_get_i32(my_handle, "saved_counter", &data_saved_counter);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;

    // Write
    data_saved_counter++;
    err = nvs_set_i32(my_handle, "saved_counter", data_saved_counter);
    if (err != ESP_OK) return err;

    char prueba[STRING_LENGTH_BIG];
    char idx[10];

    sprintf(idx,"idx_%d",data_saved_counter % MAX_BUFFER_RING);
    sprintf(prueba,DATA_FORMAT,datas.timestamp,datas.temperature_1,datas.temperature_2,datas.temperature_3,3.3*(float)(datas.battery)/0xFFF);
    ESP_LOGI(NVS_TAG,"Saving <%s> %s",idx,prueba);

    nvs_set_str(my_handle,idx,prueba);

    // Commit written value.
    err = nvs_commit(my_handle);
    if (err != ESP_OK) return err;

    // Close
    nvs_close(my_handle);
    return ESP_OK;
}

esp_err_t load_config(void)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Open
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    // Read threshold
    //int32_t threshold = 0; // value will default to 0, if not set yet in NVS
    err = nvs_get_i32(my_handle, "threshold", &threshold);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    printf("threshold = %d\n", threshold);

    // Read rate
    //int32_t rate = 0; // value will default to 0, if not set yet in NVS
    err = nvs_get_i32(my_handle, "rate", &rate);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    printf("rate = %d\n", rate);

    // Close
    nvs_close(my_handle);
    return ESP_OK;
}

esp_err_t save_config(void)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Open
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    // Write
    err = nvs_set_i32(my_handle, "threshold", threshold);
    if (err != ESP_OK) return err;

    // Write
    err = nvs_set_i32(my_handle, "rate", rate);
    if (err != ESP_OK) return err;

    // Commit written value.
    err = nvs_commit(my_handle);
    if (err != ESP_OK) return err;

    // Close
    nvs_close(my_handle);
    return ESP_OK;
}

static void task_led(void *arg)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT); // Set the GPIO as a push/pull output

    while(1) {       
        gpio_set_level(LED_GPIO, OFF);          // Blink off (output low)
        vTaskDelay(LED_RATE);  
        gpio_set_level(LED_GPIO, ON);          // Blink on (output high)
        vTaskDelay(LED_RATE);
    } 
}

static void task_adc_read(void *arg)
{
    data_t datas;
    datas.temperature_alert = false;

    char data[STRING_LENGTH_BIG];

    while(1) {  
        datas.timestamp = esp_log_timestamp();
        datas.temperature_1 = esp_random()/100000000;
        datas.temperature_2 = esp_random()/100000000;
        datas.temperature_3 = esp_random()/100000000;
        datas.battery = adc1_get_raw(ADC1_CHANNEL_6);

        //printf("%f || %d\n",3.3*(float)(datas.battery)/0xFFF,threshold);

        if ( datas.temperature_1 < threshold && datas.temperature_2 < threshold && datas.temperature_3 < threshold )
        {
            if ( datas.temperature_alert )
            {
                ESP_LOGI(MQTT_TAG, "Normal Temperature" );
                sprintf(data,STATE_FORMAT,esp_log_timestamp(),NORMAL_TEMPERATURE);
                esp_mqtt_client_publish(client, STATE_TOPIC, data , 0, 1, 0); 
                datas.temperature_alert = false;
            }
        }
        else
        {
            if (! datas.temperature_alert )
            {
                ESP_LOGI(MQTT_TAG, "High Temperature" );
                sprintf(data,STATE_FORMAT,esp_log_timestamp(),HIGH_TEMPERATURE);
                esp_mqtt_client_publish(client, STATE_TOPIC, data , 0, 1, 0); 
                datas.temperature_alert = true;
            }
        }  

        switch(dc_state)
        {
            case DC_DISCONNECTED:
            {
                energia = false;
                if (datas.battery > 0)
                    dc_state = DC_CONNECTING;
                break;
            }
            case DC_CONNECTING:
            {
                if (datas.battery > 0)
                {
                    sprintf(data,STATE_FORMAT,esp_log_timestamp(),DC_CONNECTED_MSG);
                    esp_mqtt_client_publish(client, STATE_TOPIC, data , 0, 1, 0); 
                    dc_state = DC_CONNECTED;
                    ESP_LOGI(SYSTEM_TAG,DC_CONNECTED_MSG);
                }
                break;
            }
            case DC_CONNECTED:
            {
                energia = true;
                if (datas.battery == 0)
                    dc_state = DC_DISCONNECTING;
                break;
            }
            case DC_DISCONNECTING:
            {
                if (datas.battery == 0)
                {
                    sprintf(data,STATE_FORMAT,esp_log_timestamp(),DC_DISCONNECTED_MSG);
                    esp_mqtt_client_publish(client, STATE_TOPIC, data , 0, 1, 0); 
                    dc_state = DC_DISCONNECTED;
                    ESP_LOGI(SYSTEM_TAG,DC_DISCONNECTED_MSG);
                }
                break;
            }
            default:
                dc_state = DC_CONNECTED;
        }

        ESP_LOGI(ADC_TAG, "Saving data...");
        save_read_data(datas);
        vTaskDelay(rate * ONE_SEC);
    } 
}

static void task_config(void *arg)
{
    char config[STRING_LENGTH_SMALL];

    while(1) {   
        xQueueReceive( Queue_config, &config, portMAX_DELAY);

        char *ptr_1,*ptr_2;
        char *pt_threshold;
        char *pt_rate;

        int aux;

        ptr_1 = strtok (config,",");
        ptr_2 = strtok (NULL,",");
        
        //printf("%s | %s\n",ptr_1,ptr_2);
    
        pt_threshold = strtok (ptr_1,":");
        pt_threshold = strtok (NULL,":");

        pt_rate = strtok (ptr_2,":");
        pt_rate = strtok (NULL,":");

        pt_rate[strlen(pt_rate)-1] = '\0';

        //printf("%s | %s\n",pt_threshold,pt_rate);
    
        // Usar ATOI para tener el int
        aux = atoi(pt_threshold);

        if (aux >= TEMP_MIN)
        {
            threshold = aux;
            ESP_LOGI(SYSTEM_TAG,"Temperature threshold: %d °C",threshold);
        }

        aux = atoi(pt_rate);

        if (aux >= RATE_MIN)
        {
            rate = aux;
            ESP_LOGI(SYSTEM_TAG,"MQTT rate: %d seconds",rate);
        }
        save_config();

        vTaskDelay(CONFIG_RATE);
    } 
}

static void task_connector(void* arg)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    uint32_t size_L = 0;
    char idx[10];
    char aux[STRING_LENGTH_BIG];
    int i = 0;

    while(1)
    {
        // Open
        err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
        if (err != ESP_OK) 
        {
            sprintf(aux,STATE_FORMAT,esp_log_timestamp(),"NVS not available" );
            esp_mqtt_client_publish(client, STATE_TOPIC, aux , 0, 1, 0); 
        }

        // Read saved counter
        int32_t data_saved_counter = 0; // value will default to 0, if not set yet in NVS
        err = nvs_get_i32(my_handle, "saved_counter", &data_saved_counter);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) 
        {
            sprintf(aux,STATE_FORMAT,esp_log_timestamp(),  "NVS not available" );
            esp_mqtt_client_publish(client, STATE_TOPIC, aux , 0, 1, 0); 
        }

        // Read sent counter
        int32_t data_sent_counter = 0; // value will default to 0, if not set yet in NVS
        err = nvs_get_i32(my_handle, "sent_counter", &data_sent_counter);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        {
            sprintf(aux,STATE_FORMAT,esp_log_timestamp(),  "NVS not available" );
            esp_mqtt_client_publish(client, STATE_TOPIC, aux , 0, 1, 0); 
        }
        data_sent_counter++;

        ESP_LOGI(NVS_TAG,"Sending data to queue");
        if (data_saved_counter == 0) {
            ESP_LOGI(NVS_TAG,"Nothing saved yet!");
        } else {
            for (i = data_sent_counter; i <= data_saved_counter; i++) {
                
                sprintf(idx,"idx_%d",data_sent_counter % MAX_BUFFER_RING );
                ESP_LOGI(NVS_TAG,"%d (%d) de %d en %s",data_sent_counter, data_sent_counter % MAX_BUFFER_RING, data_saved_counter,idx);
            
                nvs_get_str(my_handle, idx, NULL, &size_L);
                char* read_value = malloc(size_L);
                nvs_get_str(my_handle, idx, read_value, &size_L);

                ESP_LOGI(NVS_TAG,"Datos(%d) = %.*s", size_L,size_L, read_value);
    
                xQueueSend( Queue_data, &read_value, portMAX_DELAY);

                data_sent_counter++;
                err = nvs_set_i32(my_handle, "sent_counter", data_sent_counter);
                if (err != ESP_OK)
                {
                    sprintf(aux,STATE_FORMAT,esp_log_timestamp(),  "NVS not available" );
                    esp_mqtt_client_publish(client, STATE_TOPIC, aux , 0, 1, 0); 
                }
            }
        }

        // Close
        nvs_close(my_handle);

        vTaskDelay(rate * ONE_SEC);
    }
}

static void task_mqtt(void *arg)
{
    ESP_LOGI(MQTT_TAG, "Starting MQTT ..." );

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);

    while(1) {   

        if (energia)
        {
            char* read_value;        
            xQueueReceive( Queue_data, &read_value, portMAX_DELAY);
            ESP_LOGI(MQTT_TAG, "Sending [%s] data by MQTT",read_value);

            esp_mqtt_client_publish(client, DATA_TOPIC, read_value , 0, 1, 0); 
            free(read_value);
        }
                
        vTaskDelay(rate * ONE_SEC);
    } 
}

void start_system(void)
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

    ESP_LOGI(SYSTEM_TAG,"%dMB %s flash", spi_flash_get_chip_size() / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    ESP_LOGI(SYSTEM_TAG,"Minimum free heap size: %d bytes", esp_get_minimum_free_heap_size());
}

void app_main(void)
{
    start_system();
    
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = load_config();
    if (ret != ESP_OK) 
    {
        printf("Error (%s) loading config from NVS!\n", esp_err_to_name(ret));
        threshold = THRESHOLD_DEFAULT;
        rate = RATE_DEFAULT;
    }

    ESP_LOGI(WIFI_TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    adc1_config_width(ADC_WIDTH_12Bit);
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_11db); // Measure up to 2.2V

    Queue_data = xQueueCreate( N_QUEUE , sizeof( message_t ) );
    Queue_config = xQueueCreate( N_QUEUE , sizeof( char[STRING_LENGTH_SMALL] ) );

    if( Queue_data == NULL || Queue_config == NULL)
    {
        ESP_LOGE(SYSTEM_TAG,"Not enough memory for queue");
        while(1);
    }

    xTaskCreate(task_led, "task_led", 2048, NULL, 5, NULL);
    xTaskCreate(task_adc_read, "task_adc_read", 2048, NULL, 5, NULL);
    xTaskCreate(task_mqtt, "task_mqtt", 2048, NULL, 5, NULL);
    xTaskCreate(task_config, "task_config", 2048, NULL, 5, NULL);
    xTaskCreate(task_connector, "task_connector", 2048, NULL, 5, NULL);
}




