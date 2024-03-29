#include "main.h"

extern data_t datas;
extern int threshold_min;
extern int threshold_max;

int rate = RATE_DEFAULT;

static EventGroupHandle_t wifi_event_group; /* FreeRTOS event group to signal when we are connected*/
QueueHandle_t Queue_data,Queue_config,Queue_state;
typedef char* message_t;

dc_state_t dc_state = DC_DISCONNECTED;
int threshold_min = THRESHOLD_MIN_DEFAULT;
int threshold_max = THRESHOLD_MAX_DEFAULT;
bool energy = false;

extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[] asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[] asm("_binary_private_pem_key_end");

extern data_t datas;
extern int rate;


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

static void aws_iot_task(void *param) 
{

    IoT_Error_t rc = FAILURE;

    AWS_IoT_Client client_AWS;
    IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
    IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;

    IoT_Publish_Message_Params paramsQOS0;

    ESP_LOGI(AMAZON_TAG, "AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

    mqttInitParams.enableAutoReconnect = false; // We enable this later below
    mqttInitParams.pHostURL = AWS_HOST;
    mqttInitParams.port = AWS_IOT_MQTT_PORT;

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




