
#include "main.h"

char configuration[STRING_LENGTH_SMALL];
int threshold_min = THRESHOLD_MIN_DEFAULT;
int threshold_max = THRESHOLD_MAX_DEFAULT;
int rate = RATE_DEFAULT;

//uint32_t data_saved_counter = 0, data_sent_counter = 0;
bool energy = false;

static EventGroupHandle_t wifi_event_group; /* FreeRTOS event group to signal when we are connected*/

static EventGroupHandle_t event_group; /* FreeRTOS event group to signal when we are connected*/

QueueHandle_t Queue_data,Queue_config,Queue_state;

extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[] asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[] asm("_binary_private_pem_key_end");

/**
 * @brief Default MQTT HOST URL is pulled from the aws_iot_config.h
 */
char HostAddress[255] = AWS_HOST;

/**
 * @brief Default MQTT port is pulled from the aws_iot_config.h
 */
uint32_t port = AWS_IOT_MQTT_PORT;

//esp_mqtt_client_config_t mqtt_cfg = {
//        .uri = BROKER_URL,
//        .client_id = NAME,
//    };

//esp_mqtt_client_handle_t client;

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

typedef char* message_t;

typedef enum
{
    DC_DISCONNECTED,
    DC_CONNECTING,
    DC_CONNECTED,
    DC_DISCONNECTING
} dc_state_t;

dc_state_t dc_state = DC_DISCONNECTED;

data_t datas;

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
        ESP_LOGD(WIFI_TAG, "AP Started");
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

void iot_subscribe_callback_handler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen, IoT_Publish_Message_Params *params, void *pData) {
    ESP_LOGI(AMAZON_TAG, "Subscribe callback");
    //ESP_LOGI(AMAZON_TAG, "%.*s\t%.*s", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload);

    char* configuration = malloc((int) params->payloadLen);
    if (configuration != NULL)
    {
        memcpy(configuration,(char *)params->payload,(int) params->payloadLen);
        configuration[(int) params->payloadLen] = '\0';
        ESP_LOGI(AMAZON_TAG,"%d|%s",(int) params->payloadLen,configuration);
        // {"tl":-10,"th":50,"rt":30}
        xQueueSend( Queue_config, &configuration, portMAX_DELAY);
    }    
}

void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data) {
    ESP_LOGW(AMAZON_TAG, "MQTT Disconnect");
    IoT_Error_t rc = FAILURE;

    if(NULL == pClient) {
        return;
    }

    if(aws_iot_is_autoreconnect_enabled(pClient)) {
        ESP_LOGI(AMAZON_TAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
    } else {
        ESP_LOGW(AMAZON_TAG, "Auto Reconnect not enabled. Starting manual reconnect...");
        rc = aws_iot_mqtt_attempt_reconnect(pClient);
        if(NETWORK_RECONNECTED == rc) {
            ESP_LOGW(AMAZON_TAG, "Manual Reconnect Successful");
        } else {
            ESP_LOGW(AMAZON_TAG, "Manual Reconnect Failed - %d", rc);
        }
    }
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

/*
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    client = event->client;
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
*/

void wifi_init_sta()
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();   

    #ifdef USING_WIFI
        ESP_ERROR_CHECK(esp_event_loop_create_default());
    #else
        ESP_ERROR_CHECK( esp_event_loop_init(esp32_wifi_eventHandler, NULL));
    #endif
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &esp32_wifi_eventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &esp32_wifi_eventHandler, NULL));

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

/*
void lte_start()
{
    #if CONFIG_LWIP_PPP_PAP_SUPPORT
    esp_netif_auth_type_t auth_type = NETIF_PPP_AUTHTYPE_PAP;
    #elif CONFIG_LWIP_PPP_CHAP_SUPPORT
    esp_netif_auth_type_t auth_type = NETIF_PPP_AUTHTYPE_CHAP;
    #elif !defined(CONFIG_EXAMPLE_MODEM_PPP_AUTH_NONE)
    #error "Unsupported AUTH Negotiation"
    #endif

    ESP_LOGI(LTE_TAG,"Starting LTE");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    event_group = xEventGroupCreate();

    ESP_LOGI(LTE_TAG,"Creating modem");
    // create dte object
    esp_modem_dte_config_t config = ESP_MODEM_DTE_DEFAULT_CONFIG();

    // setup UART specific configuration based on kconfig options 
    config.tx_io_num = MODEM_UART_TX_PIN;
    config.rx_io_num = MODEM_UART_RX_PIN;
    //config.rts_io_num = MODEM_UART_RTS_PIN;
    //config.cts_io_num = MODEM_UART_CTS_PIN;
    config.rx_buffer_size = MODEM_UART_RX_BUFFER_SIZE;
    config.tx_buffer_size = MODEM_UART_TX_BUFFER_SIZE;
    config.pattern_queue_size = MODEM_UART_PATTERN_QUEUE_SIZE;
    config.event_queue_size = MODEM_UART_EVENT_QUEUE_SIZE;
    config.event_task_stack_size = MODEM_UART_EVENT_TASK_STACK_SIZE;
    config.event_task_priority = MODEM_UART_EVENT_TASK_PRIORITY;
    config.line_buffer_size = MODEM_UART_RX_BUFFER_SIZE / 2;

    modem_dte_t *dte = esp_modem_dte_init(&config);

    ESP_LOGI(LTE_TAG,"Registing modem handler");
    // Register event handler 
    ESP_ERROR_CHECK(esp_modem_set_event_handler(dte, modem_event_handler, ESP_EVENT_ANY_ID, NULL));

    ESP_LOGI(LTE_TAG,"Creating network");
    // Init netif object
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&cfg);
    assert(esp_netif);

    void *modem_netif_adapter = esp_modem_netif_setup(dte);
    esp_modem_netif_set_default_handlers(modem_netif_adapter, esp_netif);

    ESP_LOGI(LTE_TAG,"Starting network");
    modem_dce_t *dce = NULL;

    do {
        ESP_LOGI(LTE_TAG, "Trying to initialize modem on GPIO TX: %d / RX: %d", config.tx_io_num, config.rx_io_num);
        
        dce = sim800_init(dte);

        // create dce object 
        #if CONFIG_EXAMPLE_MODEM_DEVICE_SIM800
            dce = sim800_init(dte);
        #elif CONFIG_EXAMPLE_MODEM_DEVICE_BG96
            dce = bg96_init(dte);
        #elif CONFIG_EXAMPLE_MODEM_DEVICE_SIM7600
            dce = sim7600_init(dte);
        #endif
        vTaskDelay(LTE_RATE / 10);
    } while (dce == NULL);
    
    assert(dce != NULL);
    
    ESP_LOGI(LTE_TAG,"Enabling CMUX");
    // Enable CMUX
    esp_modem_start_cmux(dte);

    ESP_LOGI(LTE_TAG,"Setting flow control");
    ESP_ERROR_CHECK(dce->set_flow_ctrl(dce, MODEM_FLOW_CONTROL_NONE));
    ESP_LOGI(LTE_TAG,"Storing profile");
    ESP_ERROR_CHECK(dce->store_profile(dce));
    ESP_LOGI(LTE_TAG,"Printing information");
    // Print Module ID, Operator, IMEI, IMSI 
    ESP_LOGI(LTE_TAG, "Module: %s", dce->name);
    ESP_LOGI(LTE_TAG, "Operator: %s", dce->oper);
    ESP_LOGI(LTE_TAG, "IMEI: %s", dce->imei);
    ESP_LOGI(LTE_TAG, "IMSI: %s", dce->imsi);
    ESP_LOGI(LTE_TAG,"Getting signal quality");
    // Get signal quality
    uint32_t rssi = 0, ber = 0;
    ESP_ERROR_CHECK(dce->get_signal_quality(dce, &rssi, &ber));
    ESP_LOGI(LTE_TAG, "rssi: %d, ber: %d", rssi, ber);
    ESP_LOGI(LTE_TAG,"Getting battery voltage");
    // Get battery voltage 
    uint32_t voltage = 0, bcs = 0, bcl = 0;
    ESP_ERROR_CHECK(dce->get_battery_status(dce, &bcs, &bcl, &voltage));
    ESP_LOGI(LTE_TAG, "Battery voltage: %d mV", voltage);
    ESP_LOGI(LTE_TAG,"Configurating PPPos network");
    // setup PPPoS network parameters
    #if !defined(CONFIG_EXAMPLE_MODEM_PPP_AUTH_NONE) && (defined(CONFIG_LWIP_PPP_PAP_SUPPORT) || defined(CONFIG_LWIP_PPP_CHAP_SUPPORT))
        esp_netif_ppp_set_auth(esp_netif, auth_type, MODEM_PPP_AUTH_USERNAME, MODEM_PPP_AUTH_PASSWORD);
    #endif

    // attach the modem to the network interface
    ESP_LOGI(LTE_TAG,"Attaching modem to the network");
    esp_netif_attach(esp_netif, modem_netif_adapter);
    // Wait for IP address

    ESP_LOGI(LTE_TAG,"Waiting IP address");
    xEventGroupWaitBits(event_group, CONNECT_BIT, pdTRUE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(LTE_TAG,"Configuring MQTT");
    // Config MQTT 
    esp_mqtt_client_config_t mqtt_config = {
        .uri = BROKER_URL,
        .event_handle = mqtt_event_handler,
    };
    esp_mqtt_client_handle_t mqtt_client = esp_mqtt_client_init(&mqtt_config);
    esp_mqtt_client_start(mqtt_client);
    xEventGroupWaitBits(event_group, GOT_DATA_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    esp_mqtt_client_destroy(mqtt_client);

    // Exit PPP mode 
    // ESP_ERROR_CHECK(esp_modem_stop_ppp(dte));
    // xEventGroupWaitBits(event_group, STOP_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    #if CONFIG_EXAMPLE_SEND_MSG
    const char *message = "Welcome to ESP32!";
    ESP_ERROR_CHECK(example_send_message_text(dce, CONFIG_EXAMPLE_SEND_MSG_PEER_PHONE_NUMBER, message));
    ESP_LOGI(LTE_TAG, "Send send message [%s] ok", message);
    #endif
}
*/

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

    char aux[STRING_LENGTH_BIG];
    char idx[7];

    sprintf(idx,"i_%d",data_saved_counter % MAX_BUFFER_RING);
    sprintf(aux,DATA_FORMAT,datas.timestamp,datas.temperature_1,datas.temperature_2,datas.temperature_3,3.3*(float)(datas.battery)/0xFFF);
    ESP_LOGI(NVS_TAG,"Saving <%s> %s",idx,aux);

    nvs_set_str(my_handle,idx,aux);

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

    // Read threshold_min
    //int32_t threshold_min = 0; // value will default to 0, if not set yet in NVS
    err = nvs_get_i32(my_handle, "threshold_min", &threshold_min);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    printf("threshold_min = %d\n", threshold_min);

    // Read threshold_max
    //int32_t threshold_max = 0; // value will default to 0, if not set yet in NVS
    err = nvs_get_i32(my_handle, "threshold_max", &threshold_max);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    printf("threshold_max = %d\n", threshold_max);

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
    err = nvs_set_i32(my_handle, "threshold_min", threshold_min);
    if (err != ESP_OK) return err;

    // Write
    err = nvs_set_i32(my_handle, "threshold_max", threshold_max);
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

int parserJsonIntValues( char const* json, int* parsedValues )
{
    bool saveNextChar = false;
    char stringValue[10];
    int stringValueIndex = 0;
    int i = 0;
    int numberOfParsedValues = 0;

    while( json[i] != '\0' ) {
        //printf( "i: %d\r\n", i );
        if( json[i] == ':' ) {
            saveNextChar = true;
            i++;
            continue;
        }
        if( json[i] == ',' || json[i] == '}' ) {
            stringValue[stringValueIndex] = '\0';
            parsedValues[numberOfParsedValues] = atoi(stringValue); // ASCII to Integer
            numberOfParsedValues++;
            stringValueIndex = 0;
            saveNextChar = false;
            if( json[i] == '}' ) {
                return numberOfParsedValues;
            }
            i++;
            continue;
        }
        if( saveNextChar ) {
            stringValue[stringValueIndex] = json[i];
            stringValueIndex++;
        }
        i++;
    }
    return numberOfParsedValues;
}

void aws_iot_task(void *param) {

    IoT_Error_t rc = FAILURE;

    AWS_IoT_Client client_AWS;
    IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
    IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;

    IoT_Publish_Message_Params paramsQOS0;

    ESP_LOGI(AMAZON_TAG, "AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

    mqttInitParams.enableAutoReconnect = false; // We enable this later below
    mqttInitParams.pHostURL = AWS_HOST;//HostAddress;
    mqttInitParams.port = port;

    mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
    mqttInitParams.pDeviceCertLocation = (const char *)certificate_pem_crt_start;
    mqttInitParams.pDevicePrivateKeyLocation = (const char *)private_pem_key_start;

    mqttInitParams.mqttCommandTimeout_ms = 20000;
    mqttInitParams.tlsHandshakeTimeout_ms = 5000;
    mqttInitParams.isSSLHostnameVerify = true;
    mqttInitParams.disconnectHandler = disconnectCallbackHandler;
    mqttInitParams.disconnectHandlerData = NULL;

    rc = aws_iot_mqtt_init(&client_AWS, &mqttInitParams);
    if(SUCCESS != rc) {
        ESP_LOGE(AMAZON_TAG, "aws_iot_mqtt_init returned error : %d ", rc);
        abort();
    }

    /* Wait for WiFI to show as connected */
    xEventGroupWaitBits(wifi_event_group, CONNECT_BIT,false, true, portMAX_DELAY);

    connectParams.keepAliveIntervalInSec = 10;
    connectParams.isCleanSession = true;
    connectParams.MQTTVersion = MQTT_3_1_1;

    connectParams.pClientID = AWS_CLIENT_ID;
    connectParams.clientIDLen = (uint16_t) strlen(AWS_CLIENT_ID);
    connectParams.isWillMsgPresent = false;

    ESP_LOGI(AMAZON_TAG, "Connecting to AWS...");
    do {
        rc = aws_iot_mqtt_connect(&client_AWS, &connectParams);
        if(SUCCESS != rc) {
            ESP_LOGE(AMAZON_TAG, "Error(%d) connecting to %s:%d", rc, mqttInitParams.pHostURL, mqttInitParams.port);
            vTaskDelay(1000 / portTICK_RATE_MS);
        }
    } while(SUCCESS != rc);

    // Enable Auto Reconnect functionality. Minimum and Maximum time of Exponential backoff are set in aws_iot_config.h
    //  #AWS_IOT_MQTT_MIN_RECONNECT_WAIT_INTERVAL
    //  #AWS_IOT_MQTT_MAX_RECONNECT_WAIT_INTERVAL
    
    rc = aws_iot_mqtt_autoreconnect_set_status(&client_AWS, true);
    if(SUCCESS != rc) {
        ESP_LOGE(AMAZON_TAG, "Unable to set Auto Reconnect to true - %d", rc);
        abort();
    }

    ESP_LOGI(AMAZON_TAG, "Subscribing to ... %s", CONFIG_TOPIC);
    rc = aws_iot_mqtt_subscribe(&client_AWS, CONFIG_TOPIC, strlen(CONFIG_TOPIC), QOS0, iot_subscribe_callback_handler, NULL);
    if(SUCCESS != rc) {
        ESP_LOGE(AMAZON_TAG, "Error subscribing : %d ", rc);
        abort();
    }
    ESP_LOGI(AMAZON_TAG, "Subscribed!");

    paramsQOS0.qos = QOS0;
    paramsQOS0.isRetained = 0;

    while((NETWORK_ATTEMPTING_RECONNECT == rc || NETWORK_RECONNECTED == rc || SUCCESS == rc)) {

        //Max time the yield function will wait for read messages
        rc = aws_iot_mqtt_yield(&client_AWS, 100);
        if(NETWORK_ATTEMPTING_RECONNECT == rc) {
            // If the client is attempting to reconnect we will skip the rest of the loop.
            continue;
        }

        char* read_value;
        char status[STRING_LENGTH_BIG];

        //printf("[%d | %d | %d]\r\n",uxQueueMessagesWaiting( Queue_data ),uxQueueMessagesWaiting( Queue_config ),uxQueueMessagesWaiting( Queue_state ));

        if(uxQueueMessagesWaiting( Queue_data ) > 0 && energy)
        {      
            xQueueReceive( Queue_data, &read_value, 0);
            ESP_LOGI(MQTT_TAG, "Sending [%s] data by MQTT",read_value);
            paramsQOS0.payload = (void *) read_value;
            paramsQOS0.payloadLen = strlen(read_value);
            rc = aws_iot_mqtt_publish(&client_AWS, DATA_TOPIC, strlen(DATA_TOPIC), &paramsQOS0);
            free(read_value);
        }
        
        if(uxQueueMessagesWaiting( Queue_state ) > 0)
        {      
            xQueueReceive( Queue_state, status, 0);
            ESP_LOGI(MQTT_TAG, "Sending [%s] state by MQTT",status);
            paramsQOS0.payload = (void *) status;
            paramsQOS0.payloadLen = strlen(status);
            rc = aws_iot_mqtt_publish(&client_AWS, STATE_TOPIC, strlen(STATE_TOPIC), &paramsQOS0);
        }

        //ESP_LOGI(AMAZON_TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(0.5* RATE_MIN * ONE_SEC);
    }

    ESP_LOGE(AMAZON_TAG, "An error occurred in the main loop.");
    abort();
}

static void task_led(void *arg)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT); // Set the GPIO as a push/pull output

    while(1) {       
        gpio_set_level(LED_GPIO, OFF);          // Blink off (output low)
        vTaskDelay(LED_RATE);  
        gpio_set_level(LED_GPIO, ON);          // Blink on (output high)
        //ESP_LOGI(LED_TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(LED_RATE);
    } 
}

static void task_lte(void *arg)
{  
    while(1) {          

        ESP_LOGI(LTE_TAG,"PROBANDO");

        //ESP_LOGI(LTE_TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(LTE_RATE);
    } 
}

static void task_adc_read(void *arg)
{
    char data[STRING_LENGTH_BIG];

    while(1) {  
        datas.timestamp = esp_log_timestamp();
        datas.temperature_1 = esp_random()/100000000;
        datas.temperature_2 = esp_random()/100000000;
        datas.temperature_3 = esp_random()/100000000;
        datas.battery = adc1_get_raw(ADC1_CHANNEL_6);

        //printf("%f || %d\n",3.3*(float)(datas.battery)/0xFFF,rate);

        if ( datas.temperature_1 <= threshold_max && datas.temperature_2 <= threshold_max && datas.temperature_3 <= threshold_max &&
             datas.temperature_1 >= threshold_min && datas.temperature_2 >= threshold_min && datas.temperature_3 <= threshold_max)
        {
            if ( datas.temperature_alert )
            {
                ESP_LOGI(ADC_TAG, "Normal Temperature" );
                sprintf(data,STATE_FORMAT,esp_log_timestamp(),NORMAL_TEMPERATURE);
                //esp_mqtt_client_publish(client, STATE_TOPIC, data , 0, 1, 0); 
                xQueueSend( Queue_state, data, portMAX_DELAY);
                datas.temperature_alert = false;
            }
        }
        else
        {
            if (! datas.temperature_alert )
            {
                if ( datas.temperature_1 > threshold_max || datas.temperature_2 > threshold_max || datas.temperature_3 > threshold_max)
                {
                    ESP_LOGI(ADC_TAG, "High Temperature" );
                    printf(data,STATE_FORMAT,esp_log_timestamp(),HIGH_TEMPERATURE);
                    //esp_mqtt_client_publish(client, STATE_TOPIC, data , 0, 1, 0); 
                    xQueueSend( Queue_state, data, portMAX_DELAY);
                    datas.temperature_alert = true;
                }

                if ( datas.temperature_1 < threshold_min || datas.temperature_2 < threshold_min || datas.temperature_3 < threshold_min)
                {
                    ESP_LOGI(ADC_TAG, "Low Temperature" );
                    printf(data,STATE_FORMAT,esp_log_timestamp(),LOW_TEMPERATURE);
                    //esp_mqtt_client_publish(client, STATE_TOPIC, data , 0, 1, 0); 
                    xQueueSend( Queue_state, data, portMAX_DELAY);
                    datas.temperature_alert = true;
                }             
            }
        }  

        switch(dc_state)
        {
            case DC_DISCONNECTED:
            {
                energy = false;
                if (datas.battery > 0)
                    dc_state = DC_CONNECTING;
                break;
            }
            case DC_CONNECTING:
            {
                if (datas.battery > 0)
                {
                    sprintf(data,STATE_FORMAT,esp_log_timestamp(),DC_CONNECTED_MSG);
                    //esp_mqtt_client_publish(client, STATE_TOPIC, data , 0, 1, 0); 
                    xQueueSend( Queue_state, data, portMAX_DELAY);
                    dc_state = DC_CONNECTED;
                    ESP_LOGI(ADC_TAG,DC_CONNECTED_MSG);
                }
                break;
            }
            case DC_CONNECTED:
            {
                energy = true;
                if (datas.battery == 0)
                    dc_state = DC_DISCONNECTING;
                break;
            }
            case DC_DISCONNECTING:
            {
                if (datas.battery == 0)
                {
                    sprintf(data,STATE_FORMAT,esp_log_timestamp(),DC_DISCONNECTED_MSG);
                    //esp_mqtt_client_publish(client, STATE_TOPIC, data , 0, 1, 0); 
                    xQueueSend( Queue_state, data, portMAX_DELAY);
                    dc_state = DC_DISCONNECTED;
                    ESP_LOGI(ADC_TAG,DC_DISCONNECTED_MSG);
                }
                break;
            }
            default:
                dc_state = DC_CONNECTED;
        }
        //ESP_LOGI(ADC_TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(ADC_RATE);
    } 
}

static void task_nvs(void *arg)
{
    while(1) {  
        ESP_LOGI(NVS_TAG, "Saving data...");
        save_read_data(datas);
        //ESP_LOGI(NVS_TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(rate * ONE_SEC);
    } 
}

static void task_config(void *arg)
{
    char* configuration;

    while(1) {   
        xQueueReceive( Queue_config, &configuration, portMAX_DELAY);
        int parsedValues[10];
    
        printf( "String to parse: %s\r\n\r\n", configuration );
        // Parsear tl, th y rt
        parserJsonIntValues( configuration, parsedValues );

        if (parsedValues[0] >= TEMP_MIN)
        {
            threshold_min = parsedValues[0];
            ESP_LOGI(SYSTEM_TAG,"Temperature threshold_min: %d °C",threshold_min);
        }

        if (parsedValues[1] <= TEMP_MAX)
        {
            threshold_max = parsedValues[1];
            ESP_LOGI(SYSTEM_TAG,"Temperature threshold_max: %d °C",threshold_max);
        }

        if (parsedValues[2] >= RATE_MIN)
        {
            rate = parsedValues[2];
            ESP_LOGI(SYSTEM_TAG,"MQTT rate: %d seconds",rate);
        }
        free(configuration);
        save_config();
        //ESP_LOGI(SYSTEM_TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(CONFIG_RATE);
    } 
}

static void task_connector(void* arg)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    uint32_t size_L = 0;
    char idx[7];
    char aux[STRING_LENGTH_BIG];
    int i;

    while(1)
    {
        // Open
        err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
        if (err != ESP_OK) 
        {
            sprintf(aux,STATE_FORMAT,esp_log_timestamp(),"NVS not available" );
            //esp_mqtt_client_publish(client, STATE_TOPIC, aux , 0, 1, 0); 
            xQueueSend( Queue_state, aux, portMAX_DELAY);
        }

        // Read saved counter
        int32_t data_saved_counter = 0; // value will default to 0, if not set yet in NVS
        err = nvs_get_i32(my_handle, "saved_counter", &data_saved_counter);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) 
        {
            sprintf(aux,STATE_FORMAT,esp_log_timestamp(),  "NVS not available" );
            //esp_mqtt_client_publish(client, STATE_TOPIC, aux , 0, 1, 0); 
            xQueueSend( Queue_state, aux, portMAX_DELAY);
        }

        // Read sent counter
        int32_t data_sent_counter = 0; // value will default to 0, if not set yet in NVS
        err = nvs_get_i32(my_handle, "sent_counter", &data_sent_counter);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        {
            sprintf(aux,STATE_FORMAT,esp_log_timestamp(),  "NVS not available" );
            //esp_mqtt_client_publish(client, STATE_TOPIC, aux , 0, 1, 0); 
            xQueueSend( Queue_state, aux, portMAX_DELAY);
        }
        data_sent_counter++;

        ESP_LOGI(NVS_TAG,"Sending data to queue");
        if (data_saved_counter == 0) {
            ESP_LOGI(NVS_TAG,"Nothing saved yet!");
        } else {
            for (i = data_sent_counter; i <= data_saved_counter; i++) {
                
                sprintf(idx,"i_%d",data_sent_counter % MAX_BUFFER_RING );
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
                    //esp_mqtt_client_publish(client, STATE_TOPIC, aux , 0, 1, 0); 
                    xQueueSend( Queue_state, aux, portMAX_DELAY);
                }
            }
        }
        // Close
        nvs_close(my_handle);
        //ESP_LOGI(NVS_TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(rate * ONE_SEC);
    }
}
/*
static void task_mqtt(void *arg)
{
    ESP_LOGI(MQTT_TAG, "Starting MQTT ..." );

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);

    while(1) {   

        if (energy)
        {
            char* read_value;        
            xQueueReceive( Queue_data, &read_value, portMAX_DELAY);
            ESP_LOGI(MQTT_TAG, "Sending [%s] data by MQTT",read_value);

            esp_mqtt_client_publish(client, DATA_TOPIC, read_value , 0, 1, 0); 
            free(read_value);
        }
        //ESP_LOGI(MQTT_TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));        
        vTaskDelay(0.5 * RATE_MIN * ONE_SEC);
    } 
}
*/
void start_system(void)
{
    printf("#################################################################\n");
    
    datas.temperature_alert = false;

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
        threshold_min = THRESHOLD_MIN_DEFAULT;
        threshold_max = THRESHOLD_MAX_DEFAULT;
        rate = RATE_DEFAULT;
    }

    ESP_LOGI(WIFI_TAG, "ESP_WIFI_MODE_STA");
  
    #if LTE_READY
        lte_start();
    #else
        wifi_init_sta();
    #endif

    adc1_config_width(ADC_WIDTH_12Bit);
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_11db); // Measure up to 2.2V

    Queue_data = xQueueCreate( N_QUEUE , sizeof( message_t ) );
    Queue_config = xQueueCreate( N_QUEUE , sizeof( message_t ) );
    Queue_state = xQueueCreate( N_QUEUE , sizeof( char[STRING_LENGTH_BIG] ) );

    if( Queue_data == NULL || Queue_config == NULL || Queue_state == NULL )
    {
        ESP_LOGE(SYSTEM_TAG,"Not enough memory for queue");
        while(1);
    }

    xTaskCreate(task_led, "task_led", 2048, NULL, 5, NULL);
    #if LTE_READY
    xTaskCreate(task_lte, "task_lte", 2048, NULL, 5, NULL);
    #endif
    //xTaskCreate(task_mqtt, "task_mqtt", 2048, NULL, 5, NULL);
    xTaskCreate(task_config, "task_config", 2048, NULL, 5, NULL);
    xTaskCreate(task_adc_read, "task_adc_read", 2048, NULL, 5, NULL);
    xTaskCreate(task_nvs, "task_nvs", 2048, NULL, 5, NULL);
    xTaskCreate(task_connector, "task_connector", 2048, NULL, 5, NULL);
    xTaskCreatePinnedToCore(&aws_iot_task, "aws_iot_task", 7500, NULL, 5, NULL, 1);
}




